/**
 * @file param_data_ops.c
 * @brief App 与 IP 共享的参数数据操作层实现
 *
 * 提供 g_param_data_ops[PARAM_TYPE_COUNT] 编译期分派表，
 * 包含 cache_update / read / save / load / reset 五种纯数据操作。
 * App 和 IP 各自的控制策略 (pre_write+apply / write+dirty_map)
 * 通过此表复用数据通路，消除代码重复。
 */

#include "param_manager.h"
#include "param_manager_internal.h"
#include "param_data_ops.h"
#include <string.h>

/* ================================================================
 *  范围裁剪 — App 和 IP 共用
 * ================================================================ */

param_value_t param_clamp_value_to_range(const param_range_entry_t *re, param_value_t value)
{
    if (!re->has_range) return value;

    param_value_t clamped = value;
    switch (re->type) {
    case PARAM_TYPE_UINT:
        if (clamped.u32 < re->min.u32) clamped.u32 = re->min.u32;
        if (clamped.u32 > re->max.u32) clamped.u32 = re->max.u32;
        break;
    case PARAM_TYPE_INT:
        if (clamped.i32 < re->min.i32) clamped.i32 = re->min.i32;
        if (clamped.i32 > re->max.i32) clamped.i32 = re->max.i32;
        break;
    case PARAM_TYPE_FLOAT:
        if (clamped.f32 < re->min.f32) clamped.f32 = re->min.f32;
        if (clamped.f32 > re->max.f32) clamped.f32 = re->max.f32;
        break;
    default:
        break;
    }
    return clamped;
}

/**
 * @brief 裁剪参数缓存值到合法范围 (App 和 IP 共用)
 *
 * 仅对 UINT / INT / FLOAT 类型生效。
 * 若参数不在合法范围则修正到 min~max 区间。
 */
void param_clamp_entry(param_entry_t *e)
{
    param_type_t t = entry_type(e);
    if (t == PARAM_TYPE_UINT || t == PARAM_TYPE_INT || t == PARAM_TYPE_FLOAT) {
        param_range_entry_t *re = (param_range_entry_t *)e;
        re->cache = param_clamp_value_to_range(re, re->cache);
    }
}

/* ================================================================
 *  枚举校验 — App 和 IP 共用
 * ================================================================ */

static bool validate_enum(const param_enum_entry_t *ee, param_value_t value)
{
    if (ee->enum_values && ee->enum_count > 0) {
        for (uint16_t i = 0; i < ee->enum_count; i++) {
            if (value.i32 == ee->enum_values[i]) return true;
        }
        return false;
    }
    return true;
}

/* ================================================================
 *  pre_write 校验函数 — App 和 IP 共用分派表
 *
 *  UINT/INT/FLOAT → 范围裁剪
 *  ENUM           → 枚举合法性校验
 *  其余类型       → 无条件放行
 * ================================================================ */

static bool pre_write_range(param_entry_t *e, param_value_t *value)
{
    param_range_entry_t *re = (param_range_entry_t *)e;
    *value = param_clamp_value_to_range(re, *value);
    return true;
}

static bool pre_write_enum(param_entry_t *e, param_value_t *value)
{
    param_enum_entry_t *ee = (param_enum_entry_t *)e;
    if (!validate_enum(ee, *value)) return false;
    return true;
}

static bool pre_write_nop(param_entry_t *e, param_value_t *value)
{
    (void)e; (void)value;
    return true;
}

const param_pre_write_fn g_param_pre_write[PARAM_TYPE_COUNT] = {
    [PARAM_TYPE_UINT]   = pre_write_range,
    [PARAM_TYPE_INT]    = pre_write_range,
    [PARAM_TYPE_FLOAT]  = pre_write_range,
    [PARAM_TYPE_BOOL]   = pre_write_nop,
    [PARAM_TYPE_ENUM]   = pre_write_enum,
    [PARAM_TYPE_BLOB]   = pre_write_nop,
    [PARAM_TYPE_STRING] = pre_write_nop,
    [PARAM_TYPE_EXEC]   = pre_write_nop,
};

/* ================================================================
 *  缓存更新函数 — 每种类型将值写入 cache 的方式
 *
 *  值类型 (UINT/INT/FLOAT/BOOL/ENUM): 直接 param_value_t 赋值
 *  BLOB: memcpy blob_size 字节到外部缓冲区
 *  STRING: strncpy 并保证末尾 '\0'
 *  EXEC: 空操作 (EXEC 不可写入缓存)
 * ================================================================ */

static void cache_update_value(param_entry_t *e, param_value_t value)
{
    *entry_cache_ptr(e) = value;
}

static void cache_update_blob(param_entry_t *e, param_value_t value)
{
    param_blob_entry_t *be = (param_blob_entry_t *)e;
    if (be->blob_size > 0 && value.ptr)
        memcpy(be->cache.ptr, value.ptr, be->blob_size);
}

static void cache_update_string(param_entry_t *e, param_value_t value)
{
    param_string_entry_t *se = (param_string_entry_t *)e;
    if (se->max_len > 0 && value.ptr) {
        strncpy((char *)se->cache.ptr, (const char *)value.ptr, se->max_len);
        ((char *)se->cache.ptr)[se->max_len] = '\0';
    }
}

static void cache_update_nop(param_entry_t *e, param_value_t value)
{
    (void)e;
    (void)value;
}

/* ================================================================
 *  read 函数
 * ================================================================ */

static int value_type_read(param_entry_t *e, param_value_t *value)
{
    *value = *entry_cache(e);
    return PARAM_OK;
}

static int blob_read(param_entry_t *e, param_value_t *value)
{
    param_blob_entry_t *be = (param_blob_entry_t *)e;
    if (!be->cache.ptr) return PARAM_ERR_NOT_FOUND;
    value->ptr = be->cache.ptr;
    return PARAM_OK;
}

static int string_read(param_entry_t *e, param_value_t *value)
{
    param_string_entry_t *se = (param_string_entry_t *)e;
    if (!se->cache.ptr) return PARAM_ERR_NOT_FOUND;
    value->ptr = se->cache.ptr;
    return PARAM_OK;
}

/* ================================================================
 *  save 函数
 * ================================================================ */

static int value_type_save(param_entry_t *e)
{
    if (!(entry_flags(e) & PARAM_FLAG_PERSIST)) return PARAM_OK;

    const param_storage_drv_t *storage = param_get_storage();
    if (!storage || !storage->save) return PARAM_ERR_NOT_FOUND;

    return storage->save(storage->ctx, e->param_id, (const uint8_t *)entry_cache(e), sizeof(param_value_t));
}

static int blob_save(param_entry_t *e)
{
    if (!(entry_flags(e) & PARAM_FLAG_PERSIST)) return PARAM_OK;

    const param_storage_drv_t *storage = param_get_storage();
    if (!storage || !storage->save) return PARAM_ERR_NOT_FOUND;

    param_blob_entry_t *be = (param_blob_entry_t *)e;
    if (!be->cache.ptr || be->blob_size == 0) return PARAM_OK;
    return storage->save(storage->ctx, e->param_id, (uint8_t *)be->cache.ptr, be->blob_size);
}

static int string_save(param_entry_t *e)
{
    if (!(entry_flags(e) & PARAM_FLAG_PERSIST)) return PARAM_OK;

    const param_storage_drv_t *storage = param_get_storage();
    if (!storage || !storage->save) return PARAM_ERR_NOT_FOUND;

    param_string_entry_t *se = (param_string_entry_t *)e;
    if (!se->cache.ptr || se->max_len == 0) return PARAM_OK;
    return storage->save(storage->ctx, e->param_id, (uint8_t *)se->cache.ptr, se->max_len + 1);
}

/* ================================================================
 *  load 函数
 * ================================================================ */

static int value_type_load(param_entry_t *e)
{
    if (!(entry_flags(e) & PARAM_FLAG_PERSIST)) return PARAM_OK;

    const param_storage_drv_t *storage = param_get_storage();
    if (!storage || !storage->load) return PARAM_ERR_NOT_FOUND;

    param_value_t stored;
    int ret = storage->load(storage->ctx, e->param_id, (uint8_t *)&stored, sizeof(stored));
    if (ret == 0) *entry_cache_ptr(e) = stored;
    return ret;
}

static int blob_load(param_entry_t *e)
{
    if (!(entry_flags(e) & PARAM_FLAG_PERSIST)) return PARAM_OK;

    const param_storage_drv_t *storage = param_get_storage();
    if (!storage || !storage->load) return PARAM_ERR_NOT_FOUND;

    param_blob_entry_t *be = (param_blob_entry_t *)e;
    if (!be->cache.ptr || be->blob_size == 0) return PARAM_OK;
    return storage->load(storage->ctx, e->param_id, (uint8_t *)be->cache.ptr, be->blob_size);
}

static int string_load(param_entry_t *e)
{
    if (!(entry_flags(e) & PARAM_FLAG_PERSIST)) return PARAM_OK;

    const param_storage_drv_t *storage = param_get_storage();
    if (!storage || !storage->load) return PARAM_ERR_NOT_FOUND;

    param_string_entry_t *se = (param_string_entry_t *)e;
    if (!se->cache.ptr || se->max_len == 0) return PARAM_OK;
    int ret = storage->load(storage->ctx, e->param_id, (uint8_t *)se->cache.ptr, se->max_len + 1);
    ((char *)se->cache.ptr)[se->max_len] = '\0';
    return ret;
}

/* ================================================================
 *  reset 函数
 * ================================================================ */

static int value_type_reset(param_entry_t *e)
{
    *entry_cache_ptr(e) = *entry_default(e);
    param_entry_clear_dirty(e);
    return PARAM_OK;
}

static int blob_reset(param_entry_t *e)
{
    param_blob_entry_t *be = (param_blob_entry_t *)e;
    if (be->blob_size > 0 && be->default_val.ptr)
        memcpy(be->cache.ptr, be->default_val.ptr, be->blob_size);
    else if (be->blob_size > 0)
        memset(be->cache.ptr, 0, be->blob_size);
    param_entry_clear_dirty(e);
    return PARAM_OK;
}

static int string_reset(param_entry_t *e)
{
    param_string_entry_t *se = (param_string_entry_t *)e;
    if (se->max_len > 0 && se->default_val.ptr) {
        strncpy((char *)se->cache.ptr, (const char *)se->default_val.ptr, se->max_len);
        ((char *)se->cache.ptr)[se->max_len] = '\0';
    } else if (se->max_len > 0) {
        ((char *)se->cache.ptr)[0] = '\0';
    }
    param_entry_clear_dirty(e);
    return PARAM_OK;
}

/* ================================================================
 *  编译期常量分派表
 *
 *  用 C99 指定初始化器按 PARAM_TYPE_xxx 索引。
 *  存入 .rodata，不占 RAM。
 * ================================================================ */

const param_data_ops_t g_param_data_ops[PARAM_TYPE_COUNT] = {
    [PARAM_TYPE_UINT] = {
        .cache_update = cache_update_value,
        .read         = value_type_read,
        .save         = value_type_save,
        .load         = value_type_load,
        .reset        = value_type_reset,
    },
    [PARAM_TYPE_INT] = {
        .cache_update = cache_update_value,
        .read         = value_type_read,
        .save         = value_type_save,
        .load         = value_type_load,
        .reset        = value_type_reset,
    },
    [PARAM_TYPE_FLOAT] = {
        .cache_update = cache_update_value,
        .read         = value_type_read,
        .save         = value_type_save,
        .load         = value_type_load,
        .reset        = value_type_reset,
    },
    [PARAM_TYPE_BOOL] = {
        .cache_update = cache_update_value,
        .read         = value_type_read,
        .save         = value_type_save,
        .load         = value_type_load,
        .reset        = value_type_reset,
    },
    [PARAM_TYPE_ENUM] = {
        .cache_update = cache_update_value,
        .read         = value_type_read,
        .save         = value_type_save,
        .load         = value_type_load,
        .reset        = value_type_reset,
    },
    [PARAM_TYPE_BLOB] = {
        .cache_update = cache_update_blob,
        .read         = blob_read,
        .save         = blob_save,
        .load         = blob_load,
        .reset        = blob_reset,
    },
    [PARAM_TYPE_STRING] = {
        .cache_update = cache_update_string,
        .read         = string_read,
        .save         = string_save,
        .load         = string_load,
        .reset        = string_reset,
    },
    [PARAM_TYPE_EXEC] = {
        .cache_update = cache_update_nop,
        .read         = value_type_read,
        .save         = value_type_save,
        .load         = value_type_load,
        .reset        = value_type_reset,
    },
};
