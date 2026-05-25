/**
 * @file ip_param_manager.c
 * @brief IP 参数子系统实现 — 基于 driver 回调 + 64-bit dirty_map + BLOB/STRING 支持
 *
 * IP 参数不直接操作硬件寄存器。所有硬件访问通过 inst->read / inst->write
 * 回调完成，硬件地址/偏移/位宽由 driver 内部封装。
 *
 * dirty 追踪采用 64-bit 位图:
 *   - inst->dirty_map 的 bit[i] 对应第 i 个参数 (0-indexed)
 *   - 所有参数 clean 时 dirty_map==0 且 node.dirty==0
 *   - flush 按位遍历 dirty_map，批量写入后清零
 *
 * 控制策略 (IP 独有):
 *   - dirty: 位图精确追踪
 *   - flush: 逐参数 write，可选 flush 批量后处理
 *
 * pre_write / cache_update / read / save / load / reset
 * 均委托到 param_data_ops.c 中的 g_param_pre_write / g_param_data_ops 表。
 */

#include "param_manager.h"
#include "param_manager_internal.h"
#include "param_manager_port.h"
#include "param_data_ops.h"
#include "ip_param_manager.h"
#include <string.h>

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(offsetof(ip_instance_t, node) == 0,
               "ip_instance_t.node must be first member");
#endif

/**
 * @brief 通过 module_id 查找 IP 实例
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
 *
 *  控制策略: 无校验 → cache_update → 置 dirty_map 位
 *  数据通路: g_param_data_ops[t].xxx()
 * ================================================================ */

static int ip_param_read(param_entry_t *e, param_value_t *value)
{
    ip_instance_t *inst = ip_find((uint16_t)PARAM_MODULE_ID(e->param_id));
    
    // 1. 优先从硬件读取
    if (inst && inst->read) {
        int ret = inst->read(inst->driver, e->param_id, value);
        if (ret == PARAM_OK) {
            // 读取成功，更新缓存
            LOCK();
            param_type_t t = entry_type(e);
            if (t < PARAM_TYPE_COUNT) {
                g_param_data_ops[t].cache_update(e, *value);
            }
            UNLOCK();
            return PARAM_OK;
        }
    }
    
    // 2. 硬件读取失败，从缓存读取
    param_type_t t = entry_type(e);
    if (t >= PARAM_TYPE_COUNT) return PARAM_ERR_TYPE_MISMATCH;
    return g_param_data_ops[t].read(e, value);
}

static int ip_param_write(param_entry_t *e, param_value_t value)
{
    ip_instance_t *inst = ip_find((uint16_t)PARAM_MODULE_ID(e->param_id));
    if (!inst) return PARAM_ERR_NOT_FOUND;

    param_type_t t = entry_type(e);
    if (t >= PARAM_TYPE_COUNT) return PARAM_ERR_TYPE_MISMATCH;

    if (entry_flags(e) & PARAM_FLAG_EXEC) return PARAM_ERR_READONLY;

    if (entry_flags(e) & PARAM_FLAG_READONLY) return PARAM_ERR_READONLY;

    if (!g_param_pre_write[t](e, &value)) return PARAM_ERR_OUT_OF_RANGE;

    LOCK();
    g_param_data_ops[t].cache_update(e, value);
    param_entry_mark_dirty(e);
    UNLOCK();
    return PARAM_OK;
}

static int ip_write_immediate(param_entry_t *e, param_value_t value)
{
    ip_instance_t *inst = ip_find((uint16_t)PARAM_MODULE_ID(e->param_id));
    if (!inst) return PARAM_ERR_NOT_FOUND;

    param_type_t t = entry_type(e);
    if (t >= PARAM_TYPE_COUNT) return PARAM_ERR_TYPE_MISMATCH;

    if (entry_flags(e) & PARAM_FLAG_EXEC) {
        if (inst->exec) {
            int ret = inst->exec(inst->driver, e->param_id, value);
            if (ret == PARAM_OK) {
                LOCK();
                param_entry_clear_dirty(e);
                UNLOCK();
            }
            return ret;
        }
        return PARAM_ERR_NOT_FOUND;
    }

    if (entry_flags(e) & PARAM_FLAG_READONLY) return PARAM_ERR_READONLY;

    if (!g_param_pre_write[t](e, &value)) return PARAM_ERR_OUT_OF_RANGE;

    if (!inst->write) return PARAM_ERR_NOT_FOUND;

    int ret = inst->write(inst->driver, e->param_id, value);
    if (ret == PARAM_OK) {
        LOCK();
        g_param_data_ops[t].cache_update(e, value);
        param_entry_clear_dirty(e);
        UNLOCK();
    }
    return ret;
}

static int ip_param_write_raw(param_entry_t *e, const uint8_t *data, uint16_t len)
{
    ip_instance_t *inst = ip_find((uint16_t)PARAM_MODULE_ID(e->param_id));
    if (!inst) return PARAM_ERR_NOT_FOUND;

    param_type_t t = entry_type(e);
    if (t >= PARAM_TYPE_COUNT) return PARAM_ERR_TYPE_MISMATCH;

    if (entry_flags(e) & PARAM_FLAG_EXEC) {
        if (inst->exec) {
            param_value_t arg = { .ptr = (void *)data };
            int ret = inst->exec(inst->driver, e->param_id, arg);
            if (ret == PARAM_OK) {
                LOCK();
                param_entry_clear_dirty(e);
                UNLOCK();
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
        return ip_param_write(e, value);
    }

    if (t == PARAM_TYPE_STRING) {
        param_value_t value = { .ptr = (void *)data };
        return ip_param_write(e, value);
    }

    if (len > sizeof(param_value_t)) return PARAM_ERR_TYPE_MISMATCH;

    param_value_t value;
    memset(&value, 0, sizeof(value));
    memcpy(&value, data, len);
    return ip_param_write(e, value);
}

static int _ip_flush_write_entry(param_entry_t *e, ip_instance_t *inst)
{
    param_type_t t = entry_type(e);
    param_value_t v;
    int read_ret = g_param_data_ops[t].read(e, &v);
    if (read_ret != PARAM_OK) return read_ret;

    int ret = inst->write(inst->driver, e->param_id, v);
    if (ret != PARAM_OK) return ret;

    param_entry_clear_dirty(e);
    return PARAM_OK;
}

static int ip_flush_one(param_entry_t *e)
{
    ip_instance_t *inst = ip_find((uint16_t)PARAM_MODULE_ID(e->param_id));
    if (!inst || !inst->write) return PARAM_ERR_NOT_FOUND;

    uint16_t lid = PARAM_LOCAL_ID(e->param_id);
    if (!((inst->dirty_map >> lid) & 1)) return PARAM_OK;

    int ret = _ip_flush_write_entry(e, inst);
    if (ret == PARAM_OK)
        inst->dirty_map &= ~(1ULL << lid);
    return ret;
}

static int ip_param_save(param_entry_t *e)
{
    param_type_t t = entry_type(e);
    if (t >= PARAM_TYPE_COUNT) return PARAM_ERR_TYPE_MISMATCH;
    return g_param_data_ops[t].save(e);
}

static int ip_param_load(param_entry_t *e)
{
    param_type_t t = entry_type(e);
    if (t >= PARAM_TYPE_COUNT) return PARAM_ERR_TYPE_MISMATCH;

    int ret = g_param_data_ops[t].load(e);
    if (ret == 0) {
        param_entry_mark_dirty(e);
        param_module_node_t *m = param_module_find((uint16_t)PARAM_MODULE_ID(e->param_id));
        if (m && m->vtable && m->vtable->mark_dirty)
            m->vtable->mark_dirty(m, PARAM_LOCAL_ID(e->param_id));
    }
    return ret;
}

static int ip_param_reset(param_entry_t *e)
{
    param_type_t t = entry_type(e);
    if (t >= PARAM_TYPE_COUNT) return PARAM_ERR_TYPE_MISMATCH;

    int ret = g_param_data_ops[t].reset(e);

    ip_instance_t *inst = ip_find((uint16_t)PARAM_MODULE_ID(e->param_id));
    if (inst) {
        inst->dirty_map &= ~(1ULL << PARAM_LOCAL_ID(e->param_id));
        if (inst->dirty_map == 0)
            inst->node.dirty = 0;
    }
    return ret;
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
 * ================================================================ */

static void ip_module_mark_dirty(param_module_node_t *m, uint16_t local_id)
{
    ip_instance_t *inst = (ip_instance_t *)m;
    m->dirty = 1;
    if (local_id < IP_DIRTY_MAP_BITS)
        inst->dirty_map |= (1ULL << local_id);
}

static void ip_module_clear_dirty(param_module_node_t *m, uint16_t local_id)
{
    ip_instance_t *inst = (ip_instance_t *)m;
    if (local_id < IP_DIRTY_MAP_BITS) {
        inst->dirty_map &= ~(1ULL << local_id);
        if (inst->dirty_map == 0)
            m->dirty = 0;
    }
}

static int ip_module_flush(param_module_node_t *m)
{
    ip_instance_t *inst = (ip_instance_t *)m;
    if (inst->dirty_map == 0) return PARAM_OK;

    for (uint16_t i = 0; i < m->param_count; i++) {
        if ((inst->dirty_map >> i) & 1) {
            param_entry_t *e = m->table[i];
            if (!e || !inst->write) continue;

            int ret = _ip_flush_write_entry(e, inst);
            if (ret != PARAM_OK) return ret;
        }
    }

    if (inst->flush) {
        int ret = inst->flush(inst->driver, inst->dirty_map);
        if (ret != PARAM_OK) return ret;
    }

    inst->dirty_map = 0;
    m->dirty = 0;
    return PARAM_OK;
}

static void ip_module_deinit(param_module_node_t *m)
{
    ip_instance_t *inst = (ip_instance_t *)m;
    inst->dirty_map = 0;
}

static void ip_module_reset(param_module_node_t *m)
{
    ip_instance_t *inst = (ip_instance_t *)m;
    inst->dirty_map = 0;
    m->dirty = 0;
}

static int ip_module_init(param_module_node_t *m)
{
    ip_instance_t *inst = (ip_instance_t *)m;
    if (inst->init)
        return inst->init(inst->driver);
    return PARAM_OK;
}

static int ip_module_exec(param_module_node_t *m, uint32_t cmd_id, param_value_t arg)
{
    ip_instance_t *inst = (ip_instance_t *)m;
    if (inst->exec)
        return inst->exec(inst->driver, cmd_id, arg);
    return PARAM_ERR_NOT_FOUND;
}

const param_module_vtable_t ip_module_vtable = {
    .mark_dirty  = ip_module_mark_dirty,
    .clear_dirty = ip_module_clear_dirty,
    .flush       = ip_module_flush,
    .init        = ip_module_init,
    .exec        = ip_module_exec,
    .reset       = ip_module_reset,
    .deinit      = ip_module_deinit,
};

/* ================================================================
 *  IP Driver 注册 (公开 API)
 * ================================================================ */

int ip_driver_register(ip_instance_t *inst,
                       param_entry_t **entries,
                       uint16_t count)
{
    if (!inst || !entries || count == 0) return PARAM_ERR_INVALID_ID;
    if (count > IP_DIRTY_MAP_BITS) return PARAM_ERR_OUT_OF_RANGE;

    inst->node.vtable = &ip_module_vtable;

    return param_module_register_node(&inst->node, entries, count);
}

int ip_control(uint16_t ip_id, uint16_t local_id, uint8_t *data,
               uint8_t len, ip_action_t action)
{
    if (len != 1 && len != 2 && len != 4) return PARAM_ERR_INVALID_ID;
    if (!data) return PARAM_ERR_INVALID_ID;

    LOCK();
    ip_instance_t *inst = ip_find(ip_id);
    if (!inst) { UNLOCK(); return PARAM_ERR_NOT_FOUND; }

    if (action == IP_WRITE) {
        if (!inst->write) { UNLOCK(); return PARAM_ERR_NOT_FOUND; }
        param_value_t val;
        memset(&val, 0, sizeof(val));
        memcpy(&val, data, len);
        int ret = inst->write(inst->driver, MAKE_PARAM_ID(ip_id, local_id), val);
        UNLOCK();
        return ret;
    } else {
        if (!inst->read) { UNLOCK(); return PARAM_ERR_NOT_FOUND; }
        param_value_t val;
        memset(&val, 0, sizeof(val));
        int ret = inst->read(inst->driver, MAKE_PARAM_ID(ip_id, local_id), &val);
        if (ret == PARAM_OK) memcpy(data, &val, len);
        UNLOCK();
        return ret;
    }
}
