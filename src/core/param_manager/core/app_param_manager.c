/**
 * @file app_param_manager.c
 * @brief App 参数子系统实现 — 派生差异壳 + vtable 常量组装
 *
 * App 与 IP 的通用 vtable 实现位于 param_manager.c (基类)。
 * 本文件仅保留 App 真正差异的部分:
 *   - app_module_flush: 业务 flush 回调 (批量刷入) + 清脏
 *   - app_module_init:  清脏 + init 回调
 *   - app_vtable / app_module_vtable 常量组装
 *
 * pre_write / cache_update / read / save / load / reset
 * 均委托到 param_data_ops.c 中的 g_param_pre_write / g_param_data_ops 表。
 */

#include "param_manager.h"
#include "param_manager_internal.h"
#include "port.h"
#include "param_data_ops.h"
#include "app_param_manager.h"

/* ================================================================
 *  模块级差异实现 (App 独有)
 * ================================================================ */

static int app_module_flush(param_module_node_t *m)
{
    param_module_t *mod = (param_module_t *)m;

    /* take-and-clear (锁内): 先清脏再回调 —— 若回调执行期间有并发
       param_write 重新标脏, 新脏位不会被本次清理覆盖
       (旧实现先回调后清脏, 且回调在锁内执行, 会自死锁) */
    system_lock(SYS_LOCK_PARAM);
    for (uint16_t i = 0; i < m->param_count; i++) {
        param_entry_t *e = m->table[i];
        if (e) param_entry_clear_dirty(e);
    }
    m->dirty = 0;
    system_unlock(SYS_LOCK_PARAM);

    /* 用户回调 (锁外): 回调内可安全调用 param_read / param_save_one /
       param_read_cache 等加锁 API。失败时恢复模块级脏位, 下次
       param_flush() 自动重试 (与 IP 路径失败语义一致) */
    if (mod->flush) {
        int ret = mod->flush(mod->ctx);
        if (ret != PARAM_OK) {
            system_lock(SYS_LOCK_PARAM);
            m->dirty = 1;
            system_unlock(SYS_LOCK_PARAM);
            return ret;
        }
    }
    return PARAM_OK;
}

static int app_module_init(param_module_node_t *m)
{
    param_module_t *mod = (param_module_t *)m;
    m->dirty = 0;

    if (mod->init) {
        int ret = mod->init(mod->ctx);
        if (ret != PARAM_OK)
            return ret;
    }
    m->initialized = 1;
    return PARAM_OK;
}

/* ================================================================
 *  vtable 常量 (编译期, ROM) — 引用 param_manager.c 的基类实现
 * ================================================================ */

const param_vtable_t app_vtable = {
    .read             = param_entry_read,
    .write            = param_entry_write_apply,
    .write_cache      = param_entry_write,
    .write_immediate  = param_entry_write_immediate,
    .write_raw        = param_entry_write_raw_apply,
    .flush            = param_entry_flush_nop,
    .save             = param_vtable_save,
    .load             = param_entry_load,
    .reset            = param_entry_reset,
};

const param_module_vtable_t app_module_vtable = {
    .mark_dirty  = param_module_mark_dirty,
    .clear_dirty = param_module_clear_dirty,
    .flush       = app_module_flush,
    .init        = app_module_init,
    .exec        = param_module_exec,
    .reset       = param_module_reset,
    .deinit      = param_module_deinit,
};

/* ================================================================
 *  App 模块注册 (公开 API)
 *
 *  (与 ip_param_manager.c 的 ip_driver_register 位置对称:
 *   App 侧注册实现位于 param_manager.c —
 *   param_module_register / param_module_register_node)
 * ================================================================ */
