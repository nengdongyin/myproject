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
    /* src==dst 时跳过: param_load_one / param_reset_one / param_update
       会把 cache 值原样传回写入路径, memcpy 自拷贝属未定义行为 */
    if (be->blob_size > 0 && value.ptr && be->cache.ptr != value.ptr)
        memcpy(be->cache.ptr, value.ptr, be->blob_size);
}

static void cache_update_string(param_entry_t *e, param_value_t value)
{
    param_string_entry_t *se = (param_string_entry_t *)e;
    /* src==dst 时跳过: param_load_one / param_reset_one / param_update
       会把 cache 值原样传回写入路径, strncpy 自拷贝属未定义行为 */
    if (se->max_len > 0 && value.ptr && se->cache.ptr != value.ptr) {
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
 *  load 函数
 *
 *  分层约定:
 *   - param_storage_drv_t::load (后端层, 返回 param_status_t +
 *     read_len 出参): PARAM_OK = 记录已拷入; PARAM_ERR_NOT_FOUND =
 *     键不存在 (框架保持默认值, 非错误); 其他负值 = 读失败/数据损坏。
 *   - param_data_ops_t::load (本文件, 适配层): 把上述结果翻译成
 *     缓存可见的最终状态 —— 仅"记录存在"时提交缓存,
 *     "不存在"归为 PARAM_OK (缓存不动), 错误码原样上抛。
 *   - param_vtable_t 的 8 个槽位一律只返回 param_status_t。
 *
 *  read_len 只在适配层内部使用 (短记录补零需要知道"到底写满了多少"),
 *  不向 param_vtable_t::load 的调用者传播。
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

    /* 短记录 (旧固件 / 数据损坏 / 格式变更) → 显式清零未覆盖的尾部。
       缓存是就地填充的, 且 BLOB 为单缓冲配置 (cache 与 default_val 共享
       缓冲, 已无法回滚到默认值), 因此必须保证未覆盖字节为 0 —— 否则缓存
       成为"半新半旧"的混合体, 残留字节来自上一次写入或外部缓冲区, 随后
       被 param_read / param_save_one 当作有效数据。
       语义与 value_type_load 一致 (短记录按零补齐)。
       注: param_cache_restore 对短 BLOB 记录直接拒绝 —— 那里的源是共享的
       scan 缓冲区, 越过 len 读会越界; 此处目标缓冲区由本条目独占,
       零补齐是安全且确定的。 */
    /* 为何不"读取前整块 memset": BLOB 为单缓冲配置, cache 与 default_val
       是同一块内存, 键不存在 (PARAM_ERR_NOT_FOUND) 时预清零会连出厂默认值
       一起抹掉。value_type_load 能"先清零再回填", 是因为它清的是栈上
       临时变量而非缓存。 */
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
       - 记录不含 '\0' 时 (损坏/旧格式), 在 n 处强制截断;
       - 记录短于缓冲区时, 从 n 到缓冲末尾 [n, max_len] 全部清零。
       这两步同时解决两个真实泄漏: param_read_raw 拷贝 max_len+1 字节、
       string_save 持久化 max_len+1 字节 —— 尾部残留字节否则会被当作
       有效字符读出去、并写进 Flash。语义同 value_type_load/blob_load
       (短记录按零补齐)。 */
    uint16_t n = (got > se->max_len) ? se->max_len : got;
    /* 同 blob_load: STRING 亦为单缓冲, 不能读取前预清零 (会抹掉默认字符串) */
    memset((char *)se->cache.ptr + n, 0, (size_t)cap - (size_t)n);

    return PARAM_OK;
}

/* ================================================================
 *  批量恢复 — 供存储后端 load_all 使用
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
