/**
 * @file app_param_manager.c
 * @brief App 参数子系统实现 — vtable 虚函数 + 控制策略层
 *
 * 控制策略 (App 独有):
 *   - apply:     模块级业务回调 (拒绝/修正/副作用)
 *
 * pre_write / cache_update / read / save / load / reset
 * 均委托到 param_data_ops.c 中的 g_param_pre_write / g_param_data_ops 表。
 */

#include "param_manager.h"
#include "param_manager_internal.h"
#include "port.h"
#include "param_data_ops.h"
#include "app_param_manager.h"
#include <string.h>

/**
 * @brief 通过 module_id 查找 App 模块（带 vtable 校验）
 */
static param_module_t *app_find(uint16_t m_id)
{
    param_module_node_t *node = param_module_find(m_id);
    if (node && node->vtable == &app_module_vtable)
        return (param_module_t *)node;
    return NULL;
}

/* ================================================================
 *  app_vtable 虚函数实现
 *
 *  控制策略: pre_write → apply → cache_update → mark_dirty
 *  数据通路: g_param_data_ops[t].xxx()
 * ================================================================ */

static int app_read(param_entry_t *e, param_value_t *value)
{
    param_module_t *m = app_find((uint16_t)PARAM_MODULE_ID(e->param_id));

    /* 阶段 1: 尝试通过 read 回调获取实时值 (与 IP ip_param_read 对称) */
    if (m && m->read) {
        int ret = m->read(m->ctx, e->param_id, value);
        if (ret == PARAM_OK) {
            param_type_t t = entry_type(e);
            if (t < PARAM_TYPE_COUNT) {
                system_lock(SYS_LOCK_PARAM);
                g_param_data_ops[t].cache_update(e, *value);
                system_unlock(SYS_LOCK_PARAM);
            }
            return PARAM_OK;
        }
    }

    /* 阶段 2: 无回调 / 回调失败 / 回调不处理该 ID → 回退缓存 */
    param_type_t t = entry_type(e);
    if (t >= PARAM_TYPE_COUNT) return PARAM_ERR_TYPE_MISMATCH;
    return g_param_data_ops[t].read(e, value);
}

static int app_write(param_entry_t *e, param_value_t value)
{
    param_module_t *m = app_find((uint16_t)PARAM_MODULE_ID(e->param_id));
    if (!m) return PARAM_ERR_NOT_FOUND;

    param_type_t t = entry_type(e);
    if (t >= PARAM_TYPE_COUNT) return PARAM_ERR_TYPE_MISMATCH;

    if (entry_flags(e) & PARAM_FLAG_EXEC)
        return param_entry_is_exec(e) ? PARAM_ERR_READONLY : PARAM_ERR_TYPE_MISMATCH;

    if (entry_flags(e) & PARAM_FLAG_READONLY) return PARAM_ERR_READONLY;

    if (!g_param_pre_write[t](e, &value)) return PARAM_ERR_OUT_OF_RANGE;

    if (m->write) {
        int ret = m->write(m->ctx, e->param_id, value);
        if (ret != PARAM_OK) return ret;
    }

    system_lock(SYS_LOCK_PARAM);
    g_param_data_ops[t].cache_update(e, value);
    param_entry_mark_dirty(e);
    system_unlock(SYS_LOCK_PARAM);

    return PARAM_OK;
}

static int app_write_cache(param_entry_t *e, param_value_t value)
{
    param_type_t t = entry_type(e);
    if (t >= PARAM_TYPE_COUNT) return PARAM_ERR_TYPE_MISMATCH;

    if (entry_flags(e) & PARAM_FLAG_EXEC)
        return param_entry_is_exec(e) ? PARAM_ERR_READONLY : PARAM_ERR_TYPE_MISMATCH;

    if (entry_flags(e) & PARAM_FLAG_READONLY) return PARAM_ERR_READONLY;

    if (!g_param_pre_write[t](e, &value)) return PARAM_ERR_OUT_OF_RANGE;

    system_lock(SYS_LOCK_PARAM);
    g_param_data_ops[t].cache_update(e, value);
    param_entry_mark_dirty(e);
    system_unlock(SYS_LOCK_PARAM);

    return PARAM_OK;
}

static int app_write_immediate(param_entry_t *e, param_value_t value)
{
    param_module_t *m = app_find((uint16_t)PARAM_MODULE_ID(e->param_id));
    if (!m) return PARAM_ERR_NOT_FOUND;

    param_type_t t = entry_type(e);
    if (t >= PARAM_TYPE_COUNT) return PARAM_ERR_TYPE_MISMATCH;

    if (entry_flags(e) & PARAM_FLAG_EXEC) {
        if (!param_entry_is_exec(e)) return PARAM_ERR_TYPE_MISMATCH;
        if (m->exec) {
            int ret = m->exec(m->ctx, e->param_id, value);
            if (ret == PARAM_OK) {
                system_lock(SYS_LOCK_PARAM);
                param_entry_clear_dirty(e);
                system_unlock(SYS_LOCK_PARAM);
            }
            return ret;
        }
        return PARAM_ERR_NOT_FOUND;
    }

    if (entry_flags(e) & PARAM_FLAG_READONLY) return PARAM_ERR_READONLY;

    if (!g_param_pre_write[t](e, &value)) return PARAM_ERR_OUT_OF_RANGE;

    if (!m->write) return PARAM_ERR_NOT_FOUND;

    int ret = m->write(m->ctx, e->param_id, value);
    if (ret == PARAM_OK) {
        system_lock(SYS_LOCK_PARAM);
        g_param_data_ops[t].cache_update(e, value);
        param_entry_clear_dirty(e);
        system_unlock(SYS_LOCK_PARAM);
    }
    return ret;
}

static int app_write_raw(param_entry_t *e, const uint8_t *data, uint16_t len)
{
    param_module_t *m = app_find((uint16_t)PARAM_MODULE_ID(e->param_id));
    if (!m) return PARAM_ERR_NOT_FOUND;

    param_type_t t = entry_type(e);
    if (t >= PARAM_TYPE_COUNT) return PARAM_ERR_TYPE_MISMATCH;

    if (entry_flags(e) & PARAM_FLAG_EXEC) {
        if (!param_entry_is_exec(e)) return PARAM_ERR_TYPE_MISMATCH;
        if (m->exec) {
            param_value_t arg = { .ptr = (void *)data };
            int ret = m->exec(m->ctx, e->param_id, arg);
            if (ret == PARAM_OK) {
                system_lock(SYS_LOCK_PARAM);
                param_entry_clear_dirty(e);
                system_unlock(SYS_LOCK_PARAM);
            }
            return ret;
        }
        return PARAM_ERR_NOT_FOUND;
    }

    if (entry_flags(e) & PARAM_FLAG_READONLY) return PARAM_ERR_READONLY;

    if (t == PARAM_TYPE_BLOB) {
        param_blob_entry_t *be = (param_blob_entry_t *)e;
        if (len != be->blob_size) return PARAM_ERR_OUT_OF_RANGE;
        param_value_t value = { .ptr = (void *)data };
        return app_write(e, value);
    }

    if (len == 0 || len > sizeof(param_value_t)) return PARAM_ERR_TYPE_MISMATCH;

    param_value_t value;
    memset(&value, 0, sizeof(value));
    memcpy(&value, data, len);

    return app_write(e, value);
}

static int app_flush(param_entry_t *e)
{
    (void)e;
    return PARAM_OK;
}

static int app_load(param_entry_t *e)
{
    param_type_t t = entry_type(e);
    if (t >= PARAM_TYPE_COUNT) return PARAM_ERR_TYPE_MISMATCH;
    return g_param_data_ops[t].load(e);
}

static int app_reset(param_entry_t *e)
{
    param_type_t t = entry_type(e);
    if (t >= PARAM_TYPE_COUNT) return PARAM_ERR_TYPE_MISMATCH;
    return g_param_data_ops[t].reset(e);
}

/* ================================================================
 *  app_vtable (编译期常量, ROM)
 * ================================================================ */

const param_vtable_t app_vtable = {
    .read             = app_read,
    .write            = app_write,
    .write_cache      = app_write_cache,
    .write_immediate  = app_write_immediate,
    .write_raw        = app_write_raw,
    .flush            = app_flush,
    .save             = param_vtable_save,
    .load             = app_load,
    .reset            = app_reset,
};

/* ================================================================
 *  app_module_vtable (编译期常量, ROM)
 * ================================================================ */

static void app_module_mark_dirty(param_module_node_t *m, uint16_t local_id)
{
    (void)local_id;
    m->dirty = 1;
}

static void app_module_clear_dirty(param_module_node_t *m, uint16_t local_id)
{
    (void)local_id;
    uint8_t has_dirty = 0;
    for (uint16_t i = 0; i < m->param_count; i++) {
        param_entry_t *pe = m->table[i];
        if (pe && entry_dirty(pe)) { has_dirty = 1; break; }
    }
    if (!has_dirty) m->dirty = 0;
}

static int app_module_flush(param_module_node_t *m)
{
    param_module_t *mod = (param_module_t *)m;
    if (mod->flush) {
        int ret = mod->flush(mod->ctx);
        if (ret != PARAM_OK) return ret;
    }
    for (uint16_t i = 0; i < m->param_count; i++) {
        param_entry_t *e = m->table[i];
        if (e) param_entry_clear_dirty(e);
    }
    m->dirty = 0;
    return PARAM_OK;
}

static void app_module_reset(param_module_node_t *m)
{
    m->dirty = 0;
}

static int app_module_init(param_module_node_t *m)
{
    param_module_t *mod = (param_module_t *)m;
    m->dirty = 0;
    if (mod->init)
        return mod->init(mod->ctx);
    return PARAM_OK;
}

static int app_module_exec(param_module_node_t *m, uint32_t cmd_id, param_value_t arg)
{
    param_module_t *mod = (param_module_t *)m;
    if (mod->exec)
        return mod->exec(mod->ctx, cmd_id, arg);
    return PARAM_ERR_NOT_FOUND;
}

static void app_module_deinit(param_module_node_t *m)
{
    (void)m;
}

const param_module_vtable_t app_module_vtable = {
    .mark_dirty  = app_module_mark_dirty,
    .clear_dirty = app_module_clear_dirty,
    .flush       = app_module_flush,
    .init        = app_module_init,
    .exec        = app_module_exec,
    .reset       = app_module_reset,
    .deinit      = app_module_deinit,
};
