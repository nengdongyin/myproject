/**
 * @file ip_param_manager.c
 * @brief IP 参数子系统实现 — 派生差异壳 + vtable 常量组装
 *
 * App 与 IP 的通用 vtable 实现位于 param_manager.c (基类)。
 * 本文件仅保留 IP 真正差异的部分:
 *   - ip_param_load:        基类 load + 标脏尾巴 (恢复值需下发硬件)
 *   - _ip_flush_write_entry / ip_flush_one: 单条目硬件写通路
 *   - ip_module_flush:      逐条 driver write + 批量后处理
 *   - ip_module_init:       全量标脏 (开机下发默认值) + init 回调
 *   - ip_vtable / ip_module_vtable 常量组装
 *   - ip_driver_register:   注册入口 (注入 ip_module_vtable)
 *
 * IP 参数不直接操作硬件寄存器。所有硬件访问通过 inst->read / inst->write
 * 回调完成，硬件地址/偏移/位宽由 driver 内部封装。
 *
 * pre_write / cache_update / read / save / load / reset
 * 均委托到 param_data_ops.c 中的 g_param_pre_write / g_param_data_ops 表。
 */

#include "param_manager.h"
#include "param_manager_internal.h"
#include "port.h"
#include "param_data_ops.h"
#include "ip_param_manager.h"
#include <string.h>

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(offsetof(ip_instance_t, node) == 0,
               "ip_instance_t.node must be first member");
#endif

/* ================================================================
 *  参数级差异实现 (IP 独有)
 * ================================================================ */

static int ip_param_load(param_entry_t *e)
{
    int ret = param_entry_load(e);

    /* IP 参数恢复后必须下发硬件: 标脏条目 + 标脏模块 */
    if (ret >= 0 && param_entry_is_writable(e)) {
        param_entry_mark_dirty(e);
        param_module_node_t *m = param_module_find((uint16_t)PARAM_MODULE_ID(e->param_id));
        if (m && m->vtable && m->vtable->mark_dirty)
            m->vtable->mark_dirty(m, PARAM_LOCAL_ID(e->param_id));
    }
    return ret;
}

/**
 * @brief 单条目刷入 — take-and-clear + 锁外写硬件
 *
 * @details
 * 分三步，保证"清脏"与"硬件写入"之间不会吞掉并发写入:
 *   1. **锁内取快照 + 清脏**: 一次性读出缓存值并清除 dirty 标志。
 *      此后若有并发 param_write 更新缓存并重新标脏，新脏位保留，
 *      下一轮 param_flush 会再次下发。
 *   2. **锁外写硬件**: inst->write() 是用户回调，期间允许其调用
 *      param_read / param_read_cache / param_save_one 等加锁 API。
 *      (旧实现全程持全局锁调用回调，在非递归 mutex 上会自死锁)
 *   3. **失败回滚脏位 (锁内)**: 硬件写失败恢复 dirty，下次
 *      param_flush() 自动重试 (缓存不回滚，有意语义)。
 *
 * @param e    参数条目
 * @param inst IP 实例
 * @return PARAM_OK 成功或无需下发；否则为读缓存/硬件写入的错误码
 */
static int _ip_flush_write_entry(param_entry_t *e, ip_instance_t *inst)
{
    param_type_t t = entry_type(e);
    param_value_t v;
    int ret = PARAM_OK;
    uint8_t do_write = 0;

    /* ---- 步骤 1 (锁内): take-and-clear ---- */
    system_lock(SYS_LOCK_PARAM);
    if (entry_dirty(e)) {
        if (!param_entry_is_writable(e)) {
            /* 防御: READONLY/EXEC 条目正常流程不会标脏 (init/load 已过滤)，
               若被外部路径误标脏则在此清除并跳过，绝不写回硬件 */
            param_entry_clear_dirty(e);
        }
        else if (!inst->write) {
            /* 驱动无 write 回调: 清除 dirty 避免死循环 */
            param_entry_clear_dirty(e);
        }
        else if (t >= PARAM_TYPE_COUNT) {
            ret = PARAM_ERR_TYPE_MISMATCH; /* 条目损坏: 保留脏位 */
        }
        else {
            ret = g_param_data_ops[t].read(e, &v);
            if (ret == PARAM_OK) {
                param_entry_clear_dirty(e);
                do_write = 1;
            }
            /* 取快照失败: 保留脏位, 下次重试 */
        }
    }
    system_unlock(SYS_LOCK_PARAM);

    if (!do_write)
        return ret;

    /* ---- 步骤 2 (锁外): 硬件写入, 回调内可安全调用加锁 API ---- */
    ret = inst->write(inst->ctx, e->param_id, v);
    if (ret != PARAM_OK) {
        /* ---- 步骤 3 (锁内): 恢复脏位, 下次 param_flush 自动重试 ---- */
        system_lock(SYS_LOCK_PARAM);
        param_entry_mark_dirty(e);
        system_unlock(SYS_LOCK_PARAM);
    }
    return ret;
}

static int ip_flush_one(param_entry_t *e)
{
    ip_instance_t *inst = (ip_instance_t *)param_module_find_typed(
        (uint16_t)PARAM_MODULE_ID(e->param_id), &ip_module_vtable);
    if (!inst || !inst->write) return PARAM_ERR_NOT_FOUND;

    if (!entry_dirty(e)) return PARAM_OK;

    /* READONLY/EXEC 永不写硬件 (与 ip_module_flush 同规则) */
    if (!param_entry_is_writable(e)) {
        param_entry_clear_dirty(e);
        return PARAM_OK;
    }

    return _ip_flush_write_entry(e, inst);
}

/* ================================================================
 *  模块级差异实现 (IP 独有)
 * ================================================================ */

static int ip_module_flush(param_module_node_t *m)
{
    ip_instance_t *inst = (ip_instance_t *)m;
    if (!m->dirty) return PARAM_OK;

    int last_err = PARAM_OK;

    /* 逐条 take-and-clear + 锁外写硬件 (见 _ip_flush_write_entry)。
       非脏条目由该函数在锁内快速返回 PARAM_OK。

       m->dirty 在锁外读取 (param_flush 阶段 2 已不持锁) 属于 benign
       竞争: 最坏情况是多扫一遍全 clean 的参数表。 */
    for (uint16_t i = 0; i < m->param_count; i++) {
        param_entry_t *e = m->table[i];
        if (!e)
            continue;

        int ret = _ip_flush_write_entry(e, inst);
        if (ret != PARAM_OK) {
            last_err = ret;
            /* 脏位已在 _ip_flush_write_entry 内回滚: 缓存已先行更新为
               新值 (有意不回滚), 硬件写入失败时保持脏, 下次 param_flush()
               自动重试, 避免缓存与硬件静默分叉。调用者通过 param_flush
               返回的错误码与 flush_error_count 感知失败。
               与 app_module_flush 失败语义对齐 (App flush 回调
               失败同样保留 dirty)。 */
        }
    }

    /* 批量后处理回调 (锁外): 驱动可执行 I2C/SPI burst write 等聚合操作，
       回调内可安全调用 param_* 加锁 API */
    if (inst->flush) {
        int ret = inst->flush(inst->ctx);
        if (ret != PARAM_OK)
            last_err = ret;
    }

    /* 锁内重算模块级 dirty: 逐条刷入后若已无脏条目则清模块脏位。
       阶段 2 期间被并发置脏的条目会让 has_dirty 为真, 模块保持脏,
       下一轮 param_flush 继续下发 —— 新值不会被吞掉 */
    uint8_t has_dirty = 0;
    system_lock(SYS_LOCK_PARAM);
    for (uint16_t i = 0; i < m->param_count; i++) {
        param_entry_t *e = m->table[i];
        if (e && entry_dirty(e)) { has_dirty = 1; break; }
    }
    if (!has_dirty)
        m->dirty = 0;
    system_unlock(SYS_LOCK_PARAM);

    return last_err;
}

static int ip_module_init(param_module_node_t *m)
{
    ip_instance_t *inst = (ip_instance_t *)m;

    /* 将所有可写参数标记为 dirty，确保默认值可被 flush 下发硬件。
     * 即使不调用 param_load_all()，param_flush() 也能生效。
     * READONLY (硬件状态) 与 EXEC (命令) 条目永不进入写硬件路径，
     * 不标脏。 */
    uint8_t any = 0;
    for (uint16_t i = 0; i < m->param_count; i++) {
        if (m->table[i] && param_entry_is_writable(m->table[i])) {
            param_entry_mark_dirty(m->table[i]);
            any = 1;
        }
    }
    m->dirty = any;

    if (inst->init) {
        int ret = inst->init(inst->ctx);
        if (ret != PARAM_OK)
            return ret;
    }
    m->initialized = 1;
    return PARAM_OK;
}

/* ================================================================
 *  vtable 常量 (编译期, ROM) — 引用 param_manager.c 的基类实现
 * ================================================================ */

const param_vtable_t ip_vtable = {
    .read             = param_entry_read,
    .write            = param_entry_write,
    /* IP 参数无 apply 回调，write 和 write_cache 均仅更新缓存+标记 dirty;
       实际硬件写入由 param_flush → ip_module_flush 批量触发，因此二者等价。 */
    .write_cache      = param_entry_write,
    .write_immediate  = param_entry_write_immediate,
    .write_raw        = param_entry_write_raw,
    .flush            = ip_flush_one,
    .save             = param_vtable_save,
    .load             = ip_param_load,
    .reset            = param_entry_reset,
};

const param_module_vtable_t ip_module_vtable = {
    .mark_dirty  = param_module_mark_dirty,
    .clear_dirty = param_module_clear_dirty,
    .flush       = ip_module_flush,
    .init        = ip_module_init,
    .exec        = param_module_exec,
    .reset       = param_module_reset,
    .deinit      = param_module_deinit,
};

/* ================================================================
 *  IP Driver 注册 (公开 API)
 * ================================================================ */

int ip_driver_register(ip_instance_t *inst,
                       param_entry_t **entries,
                       uint16_t count)
{
    if (!inst || !entries || count == 0) return PARAM_ERR_INVALID_ID;

    inst->node.vtable = &ip_module_vtable;

    return param_module_register_node(&inst->node, entries, count);
}
