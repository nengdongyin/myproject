/**
 * @file ip_param_manager.c
 * @brief IP 参数子系统实现 — 基于 driver 回调 + 64-bit dirty_map
 *
 * IP 参数不直接操作硬件寄存器。所有硬件访问通过 inst->read_cb / inst->write_cb
 * 回调完成，硬件地址/偏移/位宽由 driver 内部封装。
 *
 * dirty 追踪采用 64-bit 位图:
 *   - inst->dirty_map 的 bit[i] 对应第 i 个参数 (0-indexed)
 *   - 所有参数 clean 时 dirty_map==0 且 node.dirty==0
 *   - flush 按位遍历 dirty_map，批量写入后清零
 */

#include "param_manager.h"
#include "param_manager_port.h"
#include "ip_param_manager.h"
#include <string.h>

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(offsetof(ip_param_t, base) == 0,
               "ip_param_t.base must be first member");
_Static_assert(offsetof(ip_param_t, base) == offsetof(param_entry_head_t, base) &&
               offsetof(ip_param_t, type) == offsetof(param_entry_head_t, type) &&
               offsetof(ip_param_t, flags) == offsetof(param_entry_head_t, flags),
               "ip_param_t and param_entry_head_t layout mismatch");
_Static_assert(offsetof(ip_instance_t, node) == 0,
               "ip_instance_t.node must be first member");
#endif

/**
 * @brief 通过 module_id 查找 IP 实例
 *
 * 利用统一链表 + vtable 类型识别，找到 ip_module_vtable 类型的节点。
 *
 * @param ip_id IP 模块 ID
 * @return IP 实例指针，未找到或类型不匹配返回 NULL
 */
static ip_instance_t *ip_find(uint16_t ip_id)
{
    param_module_node_t *node = param_module_find(ip_id);
    if (node && node->vtable == &ip_module_vtable)
        return (ip_instance_t *)node;
    return NULL;
}

/* ================================================================
 *  ip_vtable 虚函数实现
 * ================================================================ */

/** @brief IP 参数读取 — 直接返回 entry 缓存值 */
static int ip_param_read(param_entry_t *e, param_value_t *value)
{
    ip_param_t *ip = (ip_param_t *)e;
    *value = ip->cache;
    return PARAM_OK;
}

/**
 * @brief IP 参数写入 — 仅更新缓存，不写硬件
 *
 * 写入后标记 dirty (entry + stats)，后续由 flush 统一下发硬件。
 */
static int ip_param_write(param_entry_t *e, param_value_t value)
{
    ip_param_t *ip = (ip_param_t *)e;
    if (ip->flags & PARAM_FLAG_EXEC) return PARAM_ERR_READONLY;
    ip_instance_t *inst = ip_find((uint16_t)PARAM_MODULE_ID(e->param_id));
    if (!inst) return PARAM_ERR_NOT_FOUND;

    LOCK();
    ip->cache = value;
    if (ip->dirty == 0) { param_stats_dirty_inc(); ip->dirty = 1; }
    UNLOCK();
    return PARAM_OK;
}

/**
 * @brief IP 立即写入 — 更新缓存并通过 write_cb 直通硬件
 *
 * EXEC 参数直接路由到 exec_cb(local_id, value)，
 * value.u32 携带整型命令值，不写缓存、不产生 dirty。
 * 成功后清除 dirty 标记 (因为已同步到硬件)。
 */
static int ip_write_immediate(param_entry_t *e, param_value_t value)
{
    ip_param_t *ip = (ip_param_t *)e;
    if (ip->flags & PARAM_FLAG_EXEC) {
        param_module_node_t *m = param_module_find(PARAM_MODULE_ID(e->param_id));
        if (!m || !m->exec_cb) return PARAM_ERR_NOT_FOUND;
        return m->exec_cb(e->param_id, value);
    }
    ip_instance_t *inst = ip_find((uint16_t)PARAM_MODULE_ID(e->param_id));
    if (!inst || !inst->write_cb) return PARAM_ERR_NOT_FOUND;

    int ret = inst->write_cb(inst->driver, e->param_id, value);
    if (ret == PARAM_OK) {
        LOCK();
        ip->cache = value;
        if (ip->dirty) { param_stats_dirty_dec(); ip->dirty = 0; }
        UNLOCK();
    }
    return ret;
}

/**
 * @brief IP 原始字节流写入 — EXEC 参数路由到 exec_cb, data 封装为 param_value_t
 */
static int ip_param_write_raw(param_entry_t *e, const uint8_t *data, uint16_t len)
{
    ip_param_t *ip = (ip_param_t *)e;
    if (ip->flags & PARAM_FLAG_EXEC) {
        param_module_node_t *m = param_module_find(PARAM_MODULE_ID(e->param_id));
        if (!m || !m->exec_cb) return PARAM_ERR_NOT_FOUND;
        param_value_t arg = { .ptr = (void *)data };
        return m->exec_cb(e->param_id, arg);
    }
    if (len > sizeof(param_value_t)) return PARAM_ERR_TYPE_MISMATCH;

    param_value_t value;
    memset(&value, 0, sizeof(value));
    memcpy(&value, data, len);
    return ip_param_write(e, value);
}

/**
 * @brief 单个 IP 参数的 flush 操作
 *
 * 查询 dirty_map 判断该参数是否 dirty，若是则调 write_cb 写硬件。
 * 成功后清除 entry dirty 和 dirty_map 对应位。
 */
static int ip_flush_one(param_entry_t *e)
{
    ip_param_t *ip = (ip_param_t *)e;
    ip_instance_t *inst = ip_find((uint16_t)PARAM_MODULE_ID(e->param_id));
    if (!inst || !inst->write_cb) return PARAM_ERR_NOT_FOUND;

    uint16_t lid = PARAM_LOCAL_ID(e->param_id);
    if (!((inst->dirty_map >> lid) & 1)) return PARAM_OK;

    int ret = inst->write_cb(inst->driver, e->param_id, ip->cache);
    if (ret != PARAM_OK) return ret;

    if (ip->dirty) {
        ip->dirty = 0;
        param_stats_dirty_dec();
    }
    inst->dirty_map &= ~(1ULL << lid);
    return PARAM_OK;
}

/** @brief IP 参数 save — 仅 PERSIST 标志的参数才执行持久化 */
static int ip_param_save(param_entry_t *e)
{
    ip_param_t *ip = (ip_param_t *)e;
    if (!(ip->flags & PARAM_FLAG_PERSIST)) return PARAM_OK;

    const param_storage_drv_t *storage = param_get_storage();
    if (!storage || !storage->save) return PARAM_ERR_NOT_FOUND;

    return storage->save(storage->ctx, e->param_id, (uint8_t *)&ip->cache, sizeof(param_value_t));
}

/**
 * @brief IP 参数 load — 从持久化存储恢复缓存值
 *
 * 加载成功后自动标记 dirty (因为缓存值来自 Flash，可能与硬件不一致)，
 * 同时更新 dirty_map。
 */
static int ip_param_load(param_entry_t *e)
{
    ip_param_t *ip = (ip_param_t *)e;
    if (!(ip->flags & PARAM_FLAG_PERSIST)) return PARAM_OK;

    const param_storage_drv_t *storage = param_get_storage();
    if (!storage || !storage->load) return PARAM_ERR_NOT_FOUND;

    int ret = storage->load(storage->ctx, e->param_id, (uint8_t *)&ip->cache, sizeof(param_value_t));
    if (ret == 0) {
        if (ip->dirty == 0) { param_stats_dirty_inc(); ip->dirty = 1; }
        param_module_node_t *m = param_module_find((uint16_t)PARAM_MODULE_ID(e->param_id));
        if (m && m->vtable && m->vtable->mark_dirty)
            m->vtable->mark_dirty(m, PARAM_LOCAL_ID(e->param_id));
    }
    return ret;
}

/** @brief IP 参数 reset — 恢复为默认值并清除 dirty */
static int ip_param_reset(param_entry_t *e)
{
    *entry_cache_ptr(e) = *entry_default(e);
    param_entry_clear_dirty(e);

    ip_instance_t *inst = ip_find((uint16_t)PARAM_MODULE_ID(e->param_id));
    if (inst) {
        inst->dirty_map &= ~(1ULL << PARAM_LOCAL_ID(e->param_id));
        if (inst->dirty_map == 0)
            inst->node.dirty = 0;
    }
    return PARAM_OK;
}

/* ================================================================
 *  ip_vtable (编译期常量, ROM)
 * ================================================================ */

const param_vtable_t ip_vtable = {
    .read             = ip_param_read,
    .write            = ip_param_write,
    .write_cache      = ip_param_write,
    .write_immediate  = ip_write_immediate,
    .write_raw        = ip_param_write_raw,
    .flush            = ip_flush_one,
    .save             = ip_param_save,
    .load             = ip_param_load,
    .reset            = ip_param_reset,
};

/* ================================================================
 *  ip_module_vtable (编译期常量, ROM)
 *
 *  IP 模块 dirty 追踪策略 (位图模式):
 *   - mark_dirty:   置 dirty_map 对应位 + 模块 dirty=1
 *   - clear_dirty:  清除 dirty_map 对应位，全零时清模块 dirty
 *   - flush:        遍历 dirty_map，对所有置位参数调 write_cb
 *   - init:         调 inst->init_cb
 * ================================================================ */

/**
 * @brief IP 模块 dirty 标记 — 置 dirty_map 对应位 + 模块 dirty=1
 *
 * @param m        模块节点
 * @param local_id 局部参数 ID (0~63)
 */
static void ip_module_mark_dirty(param_module_node_t *m, uint16_t local_id)
{
    ip_instance_t *inst = (ip_instance_t *)m;
    m->dirty = 1;
    if (local_id < IP_DIRTY_MAP_BITS)
        inst->dirty_map |= (1ULL << local_id);
}

/**
 * @brief IP 模块清除 dirty — 清除 dirty_map 对应位，全零时清模块 dirty
 *
 * @param m        模块节点
 * @param local_id 局部参数 ID (0~63)
 */
static void ip_module_clear_dirty(param_module_node_t *m, uint16_t local_id)
{
    ip_instance_t *inst = (ip_instance_t *)m;
    if (local_id < IP_DIRTY_MAP_BITS) {
        inst->dirty_map &= ~(1ULL << local_id);
        if (inst->dirty_map == 0)
            m->dirty = 0;
    }
}

/**
 * @brief IP 模块 flush — 遍历 dirty_map，批量写入硬件
 *
 * 从第 0 位到 param_count-1 遍历 dirty_map。
 * 对每个 dirty 位调用 write_cb。任意一个失败则立即返回，后续不再写入。
 * 成功写入的条目清除 entry dirty 和 dirty_map 对应位。
 */
static int ip_module_flush(param_module_node_t *m)
{
    ip_instance_t *inst = (ip_instance_t *)m;
    if (inst->dirty_map == 0) return PARAM_OK;

    for (uint16_t i = 0; i < m->param_count; i++) {
        if ((inst->dirty_map >> i) & 1) {
            ip_param_t *p = (ip_param_t *)m->table[i];
            if (p && inst->write_cb) {
                int ret = inst->write_cb(inst->driver, p->base.param_id, p->cache);
                if (ret != PARAM_OK) return ret;
            }
        }
    }

    if (inst->flush_cb) {
        int ret = inst->flush_cb(inst->driver, inst->dirty_map);
        if (ret != PARAM_OK) return ret;
    }

    for (uint16_t i = 0; i < m->param_count; i++) {
        if ((inst->dirty_map >> i) & 1) {
            ip_param_t *p = (ip_param_t *)m->table[i];
            if (p && p->dirty) {
                p->dirty = 0;
                param_stats_dirty_dec();
            }
        }
    }
    inst->dirty_map = 0;
    m->dirty = 0;
    return PARAM_OK;
}

/** @brief IP 模块 deinit — 清零 dirty_map */
static void ip_module_deinit(param_module_node_t *m)
{
    ip_instance_t *inst = (ip_instance_t *)m;
    inst->dirty_map = 0;
}

/** @brief IP 模块 reset — 清零 dirty_map 和模块 dirty */
static void ip_module_reset(param_module_node_t *m)
{
    ip_instance_t *inst = (ip_instance_t *)m;
    inst->dirty_map = 0;
    m->dirty = 0;
}

/**
 * @brief IP 模块 init — 调用 inst->init_cb 初始化 driver 硬件
 *
 * @param m 模块节点
 * @return PARAM_OK 成功
 */
static int ip_module_init(param_module_node_t *m)
{
    ip_instance_t *inst = (ip_instance_t *)m;
    if (inst->init_cb)
        return inst->init_cb(inst->driver);
    return PARAM_OK;
}

const param_module_vtable_t ip_module_vtable = {
    .mark_dirty  = ip_module_mark_dirty,
    .clear_dirty = ip_module_clear_dirty,
    .flush       = ip_module_flush,
    .init        = ip_module_init,
    .reset       = ip_module_reset,
    .deinit      = ip_module_deinit,
};

/* ================================================================
 *  IP Driver 注册 (公开 API)
 *
 *  流程与 param_module_register 镜像:
 *    1. 检查重复注册
 *    2. 参数数量上限检查 (<= 64)
 *    3. 参数 ID 冲突检查
 *    4. 参数插入哈希表
 *    5. 模块节点按 ORDER 插入有序链表
 *    6. 更新统计信息
 * ================================================================ */

/**
 * @brief 注册 IP Driver 及其参数表
 *
 * 与 param_module_register 镜像流程:
 *   1. 检查重复注册
 *   2. count > 64 返回错误
 *   3. 参数 ID 冲突检查
 *   4. 参数插入哈希表
 *   5. 模块节点按 ORDER 插入有序链表
 *   6. 更新统计信息
 *
 * @param inst    IP 实例指针
 * @param entries 参数条目指针数组
 * @param count   参数数量 (<= 64)
 * @return PARAM_OK 成功，或错误码
 */
int ip_driver_register(ip_instance_t *inst,
                       param_entry_t **entries,
                       uint16_t count)
{
    if (!inst || !entries || count == 0) return PARAM_ERR_INVALID_ID;

    LOCK();

    if (param_module_find(inst->node.module_id)) {
        UNLOCK();
        return PARAM_ERR_ALREADY_REG;
    }

    if (count > IP_DIRTY_MAP_BITS) {
        UNLOCK();
        return PARAM_ERR_OUT_OF_RANGE;
    }

    for (uint16_t i = 0; i < count; i++) {
        if (!entries[i]) continue;
        if (param_entry_find(entries[i]->param_id)) {
            UNLOCK();
            return PARAM_ERR_ALREADY_REG;
        }
    }

    inst->node.table       = entries;
    inst->node.param_count = count;
    inst->node.vtable      = &ip_module_vtable;

    for (uint16_t i = 0; i < count; i++) {
        if (!entries[i]) continue;
        if (!param_hash_insert_entry(entries[i])) {
            UNLOCK();
            return PARAM_ERR_NO_MEMORY;
        }
    }

    param_module_node_insert(&inst->node);

    param_stats_module_inc();
    param_stats_params_add(count);

    for (uint16_t i = 0; i < count; i++)
        param_stats_persist_inc(entries[i]);

    UNLOCK();
    return PARAM_OK;
}

/**
 * @brief IP 控制接口 — 绕过缓存直接与 driver 通信
 *
 * 仅支持 1/2/4 字节传输长度，对应 uint8_t / uint16_t / uint32_t。
 * 写入: 将 data 转为 param_value_t 后调 write_cb
 * 读取: 调 read_cb 后将结果拷贝回 data
 */
int ip_control(uint16_t ip_id, uint16_t local_id, uint8_t *data,
               uint8_t len, ip_action_t action)
{
    if (len != 1 && len != 2 && len != 4) return PARAM_ERR_INVALID_ID;
    if (!data) return PARAM_ERR_INVALID_ID;

    LOCK();
    ip_instance_t *inst = ip_find(ip_id);
    if (!inst) { UNLOCK(); return PARAM_ERR_NOT_FOUND; }

    if (action == IP_WRITE) {
        if (!inst->write_cb) { UNLOCK(); return PARAM_ERR_NOT_FOUND; }
        param_value_t val;
        memset(&val, 0, sizeof(val));
        memcpy(&val, data, len);
        int ret = inst->write_cb(inst->driver, MAKE_PARAM_ID(ip_id, local_id), val);
        UNLOCK();
        return ret;
    } else {
        if (!inst->read_cb) { UNLOCK(); return PARAM_ERR_NOT_FOUND; }
        param_value_t val;
        memset(&val, 0, sizeof(val));
        int ret = inst->read_cb(inst->driver, MAKE_PARAM_ID(ip_id, local_id), &val);
        if (ret == PARAM_OK) memcpy(data, &val, len);
        UNLOCK();
        return ret;
    }
}
