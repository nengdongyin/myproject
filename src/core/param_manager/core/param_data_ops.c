/**
 * @file param_data_ops.c
 * @brief App 与 IP 共享的参数数据操作层实现
 *
 * 提供 g_param_data_ops[PARAM_TYPE_COUNT] 编译期分派表，
 * 包含 cache_update / read / save / load / reset 五种纯数据操作。
 * App 和 IP 各自的控制策略 (pre_write+apply / write+条目 dirty 标志)
 * 保持不变, 数据通路经此表共用。
 */

#include "param_manager.h"
#include "param_manager_internal.h"
#include "param_data_ops.h"
#include <string.h>

/* ================================================================
 *  范围裁剪工具 — App 和 IP 共用
 *
 *  注意: 写入路径 (pre_write) 对超范围值**拒绝** (见 pre_write_range),
 *  本函数仅服务维护类操作: param_validate_all / param_set_range /
 *  re-apply (load_one / reset_one / param_reapply) 的收敛语义。
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
 *  UINT/INT/FLOAT → 范围**校验** (越界拒绝, 与 ENUM 对齐)
 *  ENUM           → 枚举合法性校验
 *  其余类型       → 无条件放行
 *
 *  设计语义: 用户写入非法值必须**可见** (OUT_OF_RANGE),
 *  静默裁剪留给显式维护操作 (param_validate_all / PARAM_WR_CLAMP /
 *  re-apply 路径), 由调用方决定何时收敛, 而非每次写入悄悄改值。
 * ================================================================ */

static bool pre_write_range(param_entry_t *e, param_value_t *value)
{
    param_range_entry_t *re = (param_range_entry_t *)e;

    /* 范围校验: has_range 时值必须在 [min,max] 内 */
    if (re->has_range) {
        switch (re->type) {
        case PARAM_TYPE_UINT:
            return value->u32 >= re->min.u32 && value->u32 <= re->max.u32;
        case PARAM_TYPE_INT:
            return value->i32 >= re->min.i32 && value->i32 <= re->max.i32;
        case PARAM_TYPE_FLOAT:
            return value->f32 >= re->min.f32 && value->f32 <= re->max.f32;
        default:
            return true;
        }
    }

    /* PARAM_FLAG_FIXED: min==max==default 为唯一合法值 (固定值强制校验,
       修复"min==max 时范围校验被关闭"的零校验漏洞) */
    if (entry_flags(e) & PARAM_FLAG_FIXED) {
        switch (re->type) {
        case PARAM_TYPE_UINT:
            return value->u32 == re->min.u32;
        case PARAM_TYPE_INT:
            return value->i32 == re->min.i32;
        case PARAM_TYPE_FLOAT:
            return value->f32 == re->min.f32;
        default:
            return true;
        }
    }

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
    /* src==dst 时跳过: param_load_one / param_reset_one / param_reapply
       会把 cache 值原样传回写入路径, memcpy 自拷贝属未定义行为 */
    if (be->blob_size > 0 && value.ptr && be->cache.ptr != value.ptr)
        memcpy(be->cache.ptr, value.ptr, be->blob_size);
}

static void cache_update_string(param_entry_t *e, param_value_t value)
{
    param_string_entry_t *se = (param_string_entry_t *)e;
    /* src==dst 时跳过: param_load_one / param_reset_one / param_reapply
       会把 cache 值原样传回写入路径, 自拷贝属未定义行为。
       有界拷贝: memchr 只扫 max_len+1 字节, 不依赖调用方缓冲的 '\0'
       位置, 也不会越过 max_len+1 窗口读取 (调用方缓冲可能不是
       '\0' 终止的 C 串, 如 param_write_raw 的字节负载)。 */
    if (se->max_len > 0 && value.ptr && se->cache.ptr != value.ptr) {
        const char *src = (const char *)value.ptr;
        const char *nul = (const char *)memchr(src, '\0', (size_t)se->max_len + 1);
        size_t n = nul ? (size_t)(nul - src) : (size_t)se->max_len;
        memcpy((char *)se->cache.ptr, src, n);
        ((char *)se->cache.ptr)[n] = '\0';
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
    if (!storage || !storage->save) return PARAM_ERR_NOT_SUPPORTED;

    return storage->save(storage->ctx, e->param_id, (const uint8_t *)entry_cache(e), sizeof(param_value_t));
}

static int blob_save(param_entry_t *e)
{
    if (!(entry_flags(e) & PARAM_FLAG_PERSIST)) return PARAM_OK;

    const param_storage_drv_t *storage = param_get_storage();
    if (!storage || !storage->save) return PARAM_ERR_NOT_SUPPORTED;

    param_blob_entry_t *be = (param_blob_entry_t *)e;
    if (!be->cache.ptr || be->blob_size == 0) return PARAM_OK;
    return storage->save(storage->ctx, e->param_id, (uint8_t *)be->cache.ptr, be->blob_size);
}

static int string_save(param_entry_t *e)
{
    if (!(entry_flags(e) & PARAM_FLAG_PERSIST)) return PARAM_OK;

    const param_storage_drv_t *storage = param_get_storage();
    if (!storage || !storage->save) return PARAM_ERR_NOT_SUPPORTED;

    param_string_entry_t *se = (param_string_entry_t *)e;
    if (!se->cache.ptr || se->max_len == 0) return PARAM_OK;
    return storage->save(storage->ctx, e->param_id, (uint8_t *)se->cache.ptr, se->max_len + 1);
}

/* ================================================================
 *  load 函数 — 三层职责划分
 *   - 后端 (param_storage_drv_t::load): 返回状态码 + read_len 出参;
 *     PARAM_ERR_NOT_FOUND 专指"键不存在"。
 *   - 适配层 (本文件): 仅"记录存在"时提交缓存, "不存在"归为 PARAM_OK
 *     (缓存不动), 错误码原样上抛。read_len 只在本层使用 (短记录补零),
 *     不向 vtable::load 的调用者传播。
 *   - vtable 层: 返回 param_status_t; 唯一例外是 save —— 透传后端 0/非 0,
 *     由公共 API 统一翻译为 PARAM_ERR_FLASH_FAIL。
 * ================================================================ */

static int value_type_load(param_entry_t *e)
{
    if (!(entry_flags(e) & PARAM_FLAG_PERSIST)) return PARAM_OK;

    const param_storage_drv_t *storage = param_get_storage();
    if (!storage || !storage->load) return PARAM_ERR_NOT_SUPPORTED;

    /* 先清零再回填: 短记录 (如旧固件按 4 字节写入而本构建为 8 字节)
       未写满的字节必须为 0, 不能引入未初始化读 */
    param_value_t stored;
    memset(&stored, 0, sizeof(stored));

    int ret = storage->load(storage->ctx, e->param_id, (uint8_t *)&stored,
                            (uint16_t)sizeof(stored), NULL);
    if (ret == PARAM_ERR_NOT_FOUND) return PARAM_OK; /* 键不存在: 保持默认值 */
    if (ret != PARAM_OK) return ret;                 /* 读失败/损坏: 上抛 */
    *entry_cache_ptr(e) = stored;
    return PARAM_OK;
}

static int blob_load(param_entry_t *e)
{
    if (!(entry_flags(e) & PARAM_FLAG_PERSIST)) return PARAM_OK;

    const param_storage_drv_t *storage = param_get_storage();
    if (!storage || !storage->load) return PARAM_ERR_NOT_SUPPORTED;

    param_blob_entry_t *be = (param_blob_entry_t *)e;
    if (!be->cache.ptr || be->blob_size == 0) return PARAM_OK;

    uint16_t got = 0;
    int ret = storage->load(storage->ctx, e->param_id, (uint8_t *)be->cache.ptr,
                            be->blob_size, &got);
    if (ret == PARAM_ERR_NOT_FOUND) return PARAM_OK; /* 键不存在: 缓存保持默认值 */
    if (ret != PARAM_OK) return ret;                 /* 读失败/记录超长: 上抛 */

    /* 短记录 (旧固件 / 数据损坏 / 格式变更) → 清零未覆盖的尾部。
       BLOB 是单缓冲配置 (cache 与 default_val 共享缓冲, 无法回滚到默认值),
       而缓存是就地填充的: 不清零会留下"半新半旧"的混合体, 残留字节随后
       被 param_read / param_save_one 当作有效数据。语义与 value_type_load
       一致 (短记录按零补齐)。
       不在读取前整块 memset 的原因: 单缓冲下预清零会连出厂默认值一起抹掉
       (键不存在时缓存必须保持默认值); value_type_load 能预清零, 是因为它
       清的是栈上临时变量而非缓存。 */
    if (got < be->blob_size)
        memset((uint8_t *)be->cache.ptr + got, 0,
               (size_t)be->blob_size - (size_t)got);

    return PARAM_OK;
}

static int string_load(param_entry_t *e)
{
    if (!(entry_flags(e) & PARAM_FLAG_PERSIST)) return PARAM_OK;

    const param_storage_drv_t *storage = param_get_storage();
    if (!storage || !storage->load) return PARAM_ERR_NOT_SUPPORTED;

    param_string_entry_t *se = (param_string_entry_t *)e;
    if (!se->cache.ptr || se->max_len == 0) return PARAM_OK;

    uint16_t cap = (uint16_t)(se->max_len + 1); /* 缓冲容量 (含结尾 '\0') */
    uint16_t got = 0;
    int ret = storage->load(storage->ctx, e->param_id, (uint8_t *)se->cache.ptr,
                            cap, &got);
    if (ret == PARAM_ERR_NOT_FOUND) return PARAM_OK; /* 键不存在: 缓存保持默认值 */
    if (ret != PARAM_OK) return ret;                 /* 读失败/记录超长: 上抛 */

    /* 记录长度 n = min(read_len, max_len):
       - 不含 '\0' 的损坏记录在 n 处强制截断;
       - 短记录从 n 到缓冲末尾全部清零 —— param_read_raw / string_save 都会
         搬 max_len+1 字节, 尾部残留会被当成有效字符读出并写进 Flash。
       同 blob_load: 单缓冲下不能读取前预清零 (会抹掉默认字符串)。 */
    uint16_t n = (got > se->max_len) ? se->max_len : got;
    /* 同 blob_load: STRING 亦为单缓冲, 不能读取前预清零 (会抹掉默认字符串) */
    memset((char *)se->cache.ptr + n, 0, (size_t)cap - (size_t)n);

    return PARAM_OK;
}

/* ================================================================
 *  批量恢复 — 供框架的 sink 实现使用 (存储后端经 param_restore_sink_t
 *  间接调用, 不直接依赖本函数)
 * ================================================================ */

int param_cache_restore(uint32_t param_id, const uint8_t *data, uint16_t len)
{
    param_entry_t *e = param_entry_find(param_id);
    if (!e) return PARAM_ERR_NOT_FOUND;

    param_type_t t = entry_type(e);
    if (t >= PARAM_TYPE_COUNT) return PARAM_ERR_TYPE_MISMATCH;

    /* 墓碑/删除 → 重置为默认值 */
    if (!data || len == 0) {
        g_param_data_ops[t].reset(e);
        return PARAM_OK;
    }

    /* ---- BLOB: 定长, 记录长度必须与条目容量严格一致 ----
       存储里出现短记录 (旧固件/损坏/格式变更) 时直接拒绝,
       避免越过 scan 缓冲区读 */
    if (t == PARAM_TYPE_BLOB) {
        param_blob_entry_t *be = (param_blob_entry_t *)e;
        if (!be->cache.ptr || be->blob_size == 0) return PARAM_ERR_NOT_FOUND;
        if (len != be->blob_size) return PARAM_ERR_OUT_OF_RANGE;

        param_value_t value = { .ptr = (void *)data };
        g_param_data_ops[t].cache_update(e, value);
        return PARAM_OK;
    }

    /* ---- STRING: 长度可变, 按 len 限界拷贝并强制补 '\0' ----
       记录不含 '\0' 时若直接 strncpy(max_len) 会越过 len 扫描,
       因此在此按 len 显式定界 */
    if (t == PARAM_TYPE_STRING) {
        param_string_entry_t *se = (param_string_entry_t *)e;
        if (!se->cache.ptr || se->max_len == 0) return PARAM_ERR_NOT_FOUND;
        if (len > (uint16_t)(se->max_len + 1)) return PARAM_ERR_OUT_OF_RANGE;

        uint16_t n = (len > se->max_len) ? se->max_len : len;
        if (se->cache.ptr != data)
            memcpy(se->cache.ptr, data, n);
        ((char *)se->cache.ptr)[n] = '\0';
        return PARAM_OK;
    }

    /* ---- 值类型: 按 len 限界, 剩余字节补 0 (兼容短记录) ---- */
    param_value_t value;
    memset(&value, 0, sizeof(value));
    memcpy(&value, data, len < sizeof(value) ? len : sizeof(value));
    g_param_data_ops[t].cache_update(e, value);
    return PARAM_OK;
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
    if (be->blob_size > 0 && be->default_val.ptr) {
        /* src==dst 时跳过: BLOB 为单缓冲配置 (cache 与 default 共享,
           有意不提供双缓冲默认值 — 体积大、收益有限), 自拷贝属
           未定义行为且无法恢复默认, 仅清 dirty 保持现状 */
        if (be->cache.ptr != be->default_val.ptr)
            memcpy(be->cache.ptr, be->default_val.ptr, be->blob_size);
    } else if (be->blob_size > 0) {
        memset(be->cache.ptr, 0, be->blob_size);
    }
    param_entry_clear_dirty(e);
    return PARAM_OK;
}

static int string_reset(param_entry_t *e)
{
    param_string_entry_t *se = (param_string_entry_t *)e;
    if (se->max_len > 0 && se->default_val.ptr) {
        /* src==dst 时跳过: 单缓冲配置 (PARAM_STRING 外部缓冲 / IP) 下
           自拷贝属未定义行为且默认已丢失, 仅清 dirty 保持现状 */
        if (se->cache.ptr != se->default_val.ptr) {
            strncpy((char *)se->cache.ptr, (const char *)se->default_val.ptr, se->max_len);
            ((char *)se->cache.ptr)[se->max_len] = '\0';
        }
    } else if (se->max_len > 0) {
        ((char *)se->cache.ptr)[0] = '\0';
    }
    param_entry_clear_dirty(e);
    return PARAM_OK;
}

/* ================================================================
 *  共享 vtable save — App 和 IP 共用同一实现
 * ================================================================ */

int param_vtable_save(param_entry_t *e)
{
    param_type_t t = entry_type(e);
    if (t >= PARAM_TYPE_COUNT) return PARAM_ERR_TYPE_MISMATCH;
    return g_param_data_ops[t].save(e);
}

/* ================================================================
 *  编译期常量分派表
 *
 *  用 C99 指定初始化器按 PARAM_TYPE_xxx 索引。
 *  存入 .rodata，不占 RAM。
 *
 *  值类型 (UINT/INT/FLOAT/BOOL/ENUM/EXEC) 共用同一组函数指针，
 *  通过 VALUE_DATA_OPS 宏避免源级重复；链接器在 .rodata 中
 *  可能合并相同常量段。
 * ================================================================ */

#define VALUE_DATA_OPS \
    .cache_update = cache_update_value, \
    .read         = value_type_read,    \
    .save         = value_type_save,    \
    .load         = value_type_load,    \
    .reset        = value_type_reset

const param_data_ops_t g_param_data_ops[PARAM_TYPE_COUNT] = {
    [PARAM_TYPE_UINT]   = { VALUE_DATA_OPS },
    [PARAM_TYPE_INT]    = { VALUE_DATA_OPS },
    [PARAM_TYPE_FLOAT]  = { VALUE_DATA_OPS },
    [PARAM_TYPE_BOOL]   = { VALUE_DATA_OPS },
    [PARAM_TYPE_ENUM]   = { VALUE_DATA_OPS },
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
    [PARAM_TYPE_EXEC] = { VALUE_DATA_OPS,
        .cache_update = cache_update_nop,
    },
};
