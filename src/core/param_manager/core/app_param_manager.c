/**
 * @file app_param_manager.c
 * @brief App 参数子系统实现 — vtable 虚函数 + 类型分派表
 *
 * 核心设计: g_type_handlers[PARAM_TYPE_COUNT] 分派表
 *
 * 每种参数类型拥有一套完整操作函数:
 *   pre_write  → 写入前校验 (范围裁剪 / 枚举合法性)
 *   cache_update → 将校验后的值写入参数缓存
 *   read       → 读取缓存
 *   save       → 持久化到 Flash
 *   load       → 从 Flash 恢复
 *   reset      → 重置为默认值
 *
 * 所有按类型分支的操作均通过查表完成，消除 switch-case。
 */

#include "param_manager.h"
#include "param_manager_port.h"
#include "app_param_manager.h"
#include <string.h>

/* ================================================================
 *  App 内部校验工具
 * ================================================================ */

/**
 * @brief 校验枚举值是否在允许列表中
 *
 * @param ee    枚举型参数条目
 * @param value 待校验的值
 * @return true 合法; false 非法。若 enum_count==0 或无枚举列表则无条件通过。
 */
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
 *  范围裁剪核心函数 — 单一实现，全局复用
 * ================================================================ */

/**
 * @brief 将参数值裁剪到合法范围 [min, max]
 *
 * 若 has_range==0 则直接返回原值。支持 UINT / INT / FLOAT 三种数值类型。
 * BOOL 和 ENUM 不受范围裁剪影响。
 *
 * @param re    数值范围型参数条目
 * @param value 待裁剪的值
 * @return 裁剪后的值
 */
static param_value_t clamp_value_to_range(const param_range_entry_t *re, param_value_t value)
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

/* ================================================================
 *  类型分派表 — 完整统一所有操作
 *
 *  每个 handler 包含 6 个函数指针:
 *    pre_write     — 写入前校验/裁剪，返回 false 拒绝写入
 *    cache_update  — 将值写入缓存 (类型特定的写入方式)
 *    read / save / load / reset — 标准 CRUD + 生命周期
 * ================================================================ */

typedef bool (*app_pre_write_fn)(param_entry_t *e, param_value_t *value);
typedef void (*app_cache_update_fn)(param_entry_t *e, param_value_t value);
typedef int (*app_read_fn)(param_entry_t *e, param_value_t *value);
typedef int (*app_save_fn)(param_entry_t *e);
typedef int (*app_load_fn)(param_entry_t *e);
typedef int (*app_reset_fn)(param_entry_t *e);

typedef struct {
    app_pre_write_fn     pre_write;
    app_cache_update_fn  cache_update;
    app_read_fn          read;
    app_save_fn          save;
    app_load_fn          load;
    app_reset_fn         reset;
} app_type_handler_t;

/* ================================================================
 *  值类型（UINT/INT/FLOAT/BOOL/ENUM）共用函数
 *
 *  这些类型的 cache 均为 param_value_t (8B)，save/load/reset 操作完全一致，
 *  仅 pre_write 和 cache_update 因类型不同而有差异。
 * ================================================================ */

/** @brief 值类型 read — 返回 entry->cache 值拷贝 */
static int value_type_read(param_entry_t *e, param_value_t *value)
{
    *value = *entry_cache(e);
    return PARAM_OK;
}

/**
 * @brief 值类型 save — 将 entry->cache 整块写入持久化存储
 *
 * 仅当 flags 含 PARAM_FLAG_PERSIST 时才执行。
 */
static int value_type_save(param_entry_t *e)
{
    if (!(entry_flags(e) & PARAM_FLAG_PERSIST)) return PARAM_OK;

    const param_storage_drv_t *storage = param_get_storage();
    if (!storage || !storage->save) return PARAM_ERR_NOT_FOUND;

    return storage->save(storage->ctx, e->param_id, (const uint8_t *)entry_cache(e), sizeof(param_value_t));
}

/** @brief 值类型 load — 从持久化存储恢复 entry->cache */
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

/** @brief 值类型 reset — 恢复 entry->cache 为 default_val */
static int value_type_reset(param_entry_t *e)
{
    *entry_cache_ptr(e) = *entry_default(e);
    param_entry_clear_dirty(e);
    return PARAM_OK;
}

/* ================================================================
 *  BLOB 类型专用函数
 *
 *  BLOB 与值类型的关键区别:
 *   - 数据存放在 cache.ptr 指向的外部缓冲区
 *   - save/load 的长度由 blob_size 决定，而非固定 sizeof(param_value_t)
 *   - reset 用 memcpy/memset 而非值拷贝
 * ================================================================ */

/** @brief BLOB read — 返回 cache.ptr 外部缓冲区指针 */
static int blob_read(param_entry_t *e, param_value_t *value)
{
    param_blob_entry_t *be = (param_blob_entry_t *)e;
    if (!be->cache.ptr) return PARAM_ERR_NOT_FOUND;
    value->ptr = be->cache.ptr;
    return PARAM_OK;
}

/** @brief BLOB save — 写入 blob_size 字节到持久化存储 */
static int blob_save(param_entry_t *e)
{
    if (!(entry_flags(e) & PARAM_FLAG_PERSIST)) return PARAM_OK;

    const param_storage_drv_t *storage = param_get_storage();
    if (!storage || !storage->save) return PARAM_ERR_NOT_FOUND;

    param_blob_entry_t *be = (param_blob_entry_t *)e;
    if (!be->cache.ptr || be->blob_size == 0) return PARAM_OK;
    return storage->save(storage->ctx, e->param_id, (uint8_t *)be->cache.ptr, be->blob_size);
}

/** @brief BLOB load — 从持久化存储恢复 blob_size 字节到外部缓冲区 */
static int blob_load(param_entry_t *e)
{
    if (!(entry_flags(e) & PARAM_FLAG_PERSIST)) return PARAM_OK;

    const param_storage_drv_t *storage = param_get_storage();
    if (!storage || !storage->load) return PARAM_ERR_NOT_FOUND;

    param_blob_entry_t *be = (param_blob_entry_t *)e;
    if (!be->cache.ptr || be->blob_size == 0) return PARAM_OK;
    return storage->load(storage->ctx, e->param_id, (uint8_t *)be->cache.ptr, be->blob_size);
}

/**
 * @brief BLOB 重置: 优先用 default_val.ptr 恢复，否则 memset 清零
 */
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

/* ================================================================
 *  STRING 类型专用函数
 *
 *  与 BLOB 类似但以 '\\0' 结尾的 C 字符串语义:
 *   - 缓存和默认值: char 缓冲区 (长度 = max_len + 1)
 *   - save/load 存 max_len+1 字节 (含 '\\0')
 *   - strncpy 保证不溢出且末尾有 '\\0'
 * ================================================================ */

/** @brief STRING read — 返回 cache.ptr 外部缓冲区指针 */
static int string_read(param_entry_t *e, param_value_t *value)
{
    param_string_entry_t *se = (param_string_entry_t *)e;
    if (!se->cache.ptr) return PARAM_ERR_NOT_FOUND;
    value->ptr = se->cache.ptr;
    return PARAM_OK;
}

/** @brief STRING save — 写入 max_len+1 字节 (含 '\\0') 到持久化存储 */
static int string_save(param_entry_t *e)
{
    if (!(entry_flags(e) & PARAM_FLAG_PERSIST)) return PARAM_OK;

    const param_storage_drv_t *storage = param_get_storage();
    if (!storage || !storage->save) return PARAM_ERR_NOT_FOUND;

    param_string_entry_t *se = (param_string_entry_t *)e;
    if (!se->cache.ptr || se->max_len == 0) return PARAM_OK;
    return storage->save(storage->ctx, e->param_id, (uint8_t *)se->cache.ptr, se->max_len + 1);
}

/** @brief STRING load — 从持久化存储恢复 max_len+1 字节，强制末尾 '\\0' */
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

/** @brief STRING reset — 用 default_val 或空字符串恢复缓存 */
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
 *  缓存更新函数 — 每种类型将值写入 cache 的方式
 *
 *  UINT/INT/FLOAT 共用 cache_update_range (直接 param_value_t 赋值)
 *  BLOB 用 memcpy (因为 ptr 指向外部缓冲区)
 *  BOOL / ENUM 也只需赋值，但结构体类型不同所以需要独立函数
 * ================================================================ */

/** @brief UINT/INT/FLOAT 的 cache_update — 直接 param_value_t 赋值 */
static void cache_update_range(param_entry_t *e, param_value_t value)
{
    param_range_entry_t *re = (param_range_entry_t *)e;
    re->cache = value;
}

/** @brief ENUM 的 cache_update */
static void cache_update_enum(param_entry_t *e, param_value_t value)
{
    param_enum_entry_t *ee = (param_enum_entry_t *)e;
    ee->cache = value;
}

/** @brief BOOL 的 cache_update */
static void cache_update_bool(param_entry_t *e, param_value_t value)
{
    param_bool_entry_t *be = (param_bool_entry_t *)e;
    be->cache = value;
}

/** @brief BLOB 的 cache_update — memcpy blob_size 字节到外部缓冲区 */
static void cache_update_blob(param_entry_t *e, param_value_t value)
{
    param_blob_entry_t *be = (param_blob_entry_t *)e;
    if (be->blob_size > 0)
        memcpy(be->cache.ptr, value.ptr, be->blob_size);
}

/** @brief STRING 的 cache_update — strncpy 并保证末尾 '\\0' */
static void cache_update_string(param_entry_t *e, param_value_t value)
{
    param_string_entry_t *se = (param_string_entry_t *)e;
    if (se->max_len > 0 && value.ptr) {
        strncpy((char *)se->cache.ptr, (const char *)value.ptr, se->max_len);
        ((char *)se->cache.ptr)[se->max_len] = '\0';
    }
}

/* ================================================================
 *  pre_write 函数 — 写入前校验/裁剪
 *
 *  返回 false 表示拒绝写入 (值非法)
 *  返回 true  表示通过，value 可能已被原地修改 (裁剪到合法范围)
 * ================================================================ */

/**
 * @brief UINT/INT/FLOAT 的 pre_write: 范围裁剪
 *
 * clamp_value_to_range 在必要时将 value 裁剪到 [min, max]。
 * 始终返回 true (范围裁剪不会拒绝写入)。
 */
static bool pre_write_range(param_entry_t *e, param_value_t *value)
{
    param_range_entry_t *re = (param_range_entry_t *)e;
    *value = clamp_value_to_range(re, *value);
    return true;
}

/** @brief ENUM 的 pre_write — 校验值是否在枚举列表中 */
static bool pre_write_enum(param_entry_t *e, param_value_t *value)
{
    param_enum_entry_t *ee = (param_enum_entry_t *)e;
    if (!validate_enum(ee, *value)) return false;
    return true;
}

/** @brief BOOL / BLOB 的 pre_write: 无校验，直接放行 */
static bool pre_write_nop(param_entry_t *e, param_value_t *value)
{
    (void)e; (void)value;
    return true;
}

/* ================================================================
 *  编译期常量分派表
 *
 *  用 C99 指定初始化器 (designated initializer) 按 PARAM_TYPE_xxx 索引。
 *  g_type_handlers 存入 .rodata，不占 RAM。
 * ================================================================ */

static const app_type_handler_t g_type_handlers[PARAM_TYPE_COUNT] = {
    [PARAM_TYPE_UINT] = {
        .pre_write    = pre_write_range,
        .cache_update = cache_update_range,
        .read         = value_type_read,
        .save         = value_type_save,
        .load         = value_type_load,
        .reset        = value_type_reset
    },
    [PARAM_TYPE_INT] = {
        .pre_write    = pre_write_range,
        .cache_update = cache_update_range,
        .read         = value_type_read,
        .save         = value_type_save,
        .load         = value_type_load,
        .reset        = value_type_reset
    },
    [PARAM_TYPE_FLOAT] = {
        .pre_write    = pre_write_range,
        .cache_update = cache_update_range,
        .read         = value_type_read,
        .save         = value_type_save,
        .load         = value_type_load,
        .reset        = value_type_reset
    },
    [PARAM_TYPE_BOOL] = {
        .pre_write    = pre_write_nop,
        .cache_update = cache_update_bool,
        .read         = value_type_read,
        .save         = value_type_save,
        .load         = value_type_load,
        .reset        = value_type_reset
    },
    [PARAM_TYPE_ENUM] = {
        .pre_write    = pre_write_enum,
        .cache_update = cache_update_enum,
        .read         = value_type_read,
        .save         = value_type_save,
        .load         = value_type_load,
        .reset        = value_type_reset
    },
    [PARAM_TYPE_BLOB] = {
        .pre_write    = pre_write_nop,
        .cache_update = cache_update_blob,
        .read         = blob_read,
        .save         = blob_save,
        .load         = blob_load,
        .reset        = blob_reset
    },
    [PARAM_TYPE_STRING] = {
        .pre_write    = pre_write_nop,
        .cache_update = cache_update_string,
        .read         = string_read,
        .save         = string_save,
        .load         = string_load,
        .reset        = string_reset
    },
    [PARAM_TYPE_EXEC] = {
        .pre_write    = pre_write_nop,
        .cache_update = cache_update_bool,
        .read         = value_type_read,
        .save         = value_type_save,
        .load         = value_type_load,
        .reset        = value_type_reset
    }
};

/**
 * @brief 仅在 entry 当前不 dirty 时才递增 dirty 计数并置位
 *
 * 避免对同一个参数连续写多次导致 dirty_count 重复累加。
 */
static void mark_dirty_if_needed(param_entry_t *e)
{
    if (!entry_dirty(e)) {
        param_stats_dirty_inc();
        entry_set_dirty(e, 1);
    }
}

/**
 * @brief 裁剪指定 App 参数的缓存值到合法范围
 *
 * 仅对 UINT / INT / FLOAT 类型生效。
 * 若参数不在合法范围则修正到 min~max 区间。
 *
 * @param e 参数条目指针
 */
void app_clamp_entry(param_entry_t *e)
{
    if (!is_app(e)) return;
    param_type_t t = entry_type(e);
    if (t == PARAM_TYPE_UINT || t == PARAM_TYPE_INT || t == PARAM_TYPE_FLOAT) {
        param_range_entry_t *re = (param_range_entry_t *)e;
        re->cache = clamp_value_to_range(re, re->cache);
    }
}

/* ================================================================
 *  app_vtable 虚函数实现 — 100% 表驱动
 *
 *  所有函数均通过 g_type_handlers[t] 查表，无 switch-case。
 *  新增参数类型只需在 g_type_handlers 中追加一行即可。
 * ================================================================ */

static int app_read(param_entry_t *e, param_value_t *value)
{
    param_type_t t = entry_type(e);
    if (t >= PARAM_TYPE_COUNT) return PARAM_ERR_TYPE_MISMATCH;
    return g_type_handlers[t].read(e, value);
}

/**
 * @brief App 参数写入流程
 *
 * 1. 检查 READONLY 标志
 * 2. 查表获取 pre_write → 写入前校验 (范围/枚举)
 * 3. 调用模块 apply 回调 (业务层校验)
 * 4. LOCK → cache_update → mark_dirty_if_needed → UNLOCK
 */
static int app_write(param_entry_t *e, param_value_t value)
{
    if (entry_flags(e) & (PARAM_FLAG_READONLY | PARAM_FLAG_EXEC))
        return PARAM_ERR_READONLY;

    param_type_t t = entry_type(e);
    if (t >= PARAM_TYPE_COUNT) return PARAM_ERR_TYPE_MISMATCH;

    const app_type_handler_t *h = &g_type_handlers[t];
    if (!h->pre_write(e, &value)) return PARAM_ERR_OUT_OF_RANGE;

    uint16_t m_id = (uint16_t)PARAM_MODULE_ID(e->param_id);
    param_module_node_t *node = param_module_find(m_id);
    param_module_t *m = (param_module_t *)node;
    if (m && m->apply) {
        int ret = m->apply(e->param_id, value);
        if (ret != PARAM_OK) return ret;
    }

    LOCK();
    h->cache_update(e, value);
    mark_dirty_if_needed(e);
    UNLOCK();

    return PARAM_OK;
}

/**
 * @brief App 写缓存 — 跳过 apply 回调，仅更新缓存 + 标记 dirty
 *
 * 与 app_write 的区别: 不调用模块 apply 回调，允许 apply 内部使用。
 */
static int app_write_cache(param_entry_t *e, param_value_t value)
{
    if (entry_flags(e) & (PARAM_FLAG_READONLY | PARAM_FLAG_EXEC))
        return PARAM_ERR_READONLY;

    param_type_t t = entry_type(e);
    if (t >= PARAM_TYPE_COUNT) return PARAM_ERR_TYPE_MISMATCH;

    const app_type_handler_t *h = &g_type_handlers[t];
    if (!h->pre_write(e, &value)) return PARAM_ERR_OUT_OF_RANGE;

    LOCK();
    h->cache_update(e, value);
    mark_dirty_if_needed(e);
    UNLOCK();

    return PARAM_OK;
}

/**
 * @brief App 立即写入 — 跳过缓存，直接调 apply 回写硬件
 *
 * 成功时不产生 dirty 标记 (因为已同步到硬件)。
 */
static int app_write_immediate(param_entry_t *e, param_value_t value)
{
    if (entry_flags(e) & (PARAM_FLAG_READONLY | PARAM_FLAG_EXEC))
        return PARAM_ERR_READONLY;

    uint16_t m_id = (uint16_t)PARAM_MODULE_ID(e->param_id);
    param_module_node_t *node = param_module_find(m_id);
    param_module_t *m = (param_module_t *)node;
    if (!m || !m->apply) return PARAM_ERR_NOT_FOUND;

    param_type_t t = entry_type(e);
    if (t >= PARAM_TYPE_COUNT) return PARAM_ERR_TYPE_MISMATCH;

    const app_type_handler_t *h = &g_type_handlers[t];
    if (!h->pre_write(e, &value)) return PARAM_ERR_OUT_OF_RANGE;

    int ret = m->apply(e->param_id, value);
    if (ret == PARAM_OK) {
        LOCK();
        h->cache_update(e, value);
        param_entry_clear_dirty(e);
        UNLOCK();
    }
    return ret;
}

/**
 * @brief App 原始字节流写入
 *
 * EXEC 参数直接路由到 exec_cb, data 封装为 param_value_t 联合体。
 * BLOB 类型走专用路径 (大小校验);
 * 其他类型将 data 转换为 param_value_t 后写入。
 */
static int app_write_raw(param_entry_t *e, const uint8_t *data, uint16_t len)
{
    if (entry_flags(e) & PARAM_FLAG_EXEC) {
        param_module_node_t *m = param_module_find(PARAM_MODULE_ID(e->param_id));
        if (!m || !m->exec_cb) return PARAM_ERR_NOT_FOUND;
        param_value_t arg = { .ptr = (void *)data };
        return m->exec_cb(e->param_id, arg);
    }
    if (entry_flags(e) & PARAM_FLAG_READONLY) return PARAM_ERR_READONLY;

    param_type_t t = entry_type(e);

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

/**
 * @brief App 参数级 flush — 无操作 (真正的 flush 在模块级完成)
 */
static int app_flush(param_entry_t *e)
{
    (void)e;
    return PARAM_OK;
}

/** @brief App 参数 save — 查表分派到对应类型的 save handler */
static int app_save(param_entry_t *e)
{
    param_type_t t = entry_type(e);
    if (t >= PARAM_TYPE_COUNT) return PARAM_ERR_TYPE_MISMATCH;
    return g_type_handlers[t].save(e);
}

/** @brief App 参数 load — 查表分派到对应类型的 load handler */
static int app_load(param_entry_t *e)
{
    param_type_t t = entry_type(e);
    if (t >= PARAM_TYPE_COUNT) return PARAM_ERR_TYPE_MISMATCH;
    return g_type_handlers[t].load(e);
}

/** @brief App 参数 reset — 查表分派到对应类型的 reset handler */
static int app_reset(param_entry_t *e)
{
    param_type_t t = entry_type(e);
    if (t >= PARAM_TYPE_COUNT) return PARAM_ERR_TYPE_MISMATCH;
    return g_type_handlers[t].reset(e);
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
    .save             = app_save,
    .load             = app_load,
    .reset            = app_reset,
};

/* ================================================================
 *  app_module_vtable (编译期常量, ROM)
 *
 *  App 模块的 dirty 追踪策略:
 *   - mark_dirty:   直接将模块标记为 dirty (module->dirty = 1)
 *   - clear_dirty:  遍历所有 entry，只有全部 clean 才清除模块 dirty
 *   - flush:        先调用户回调 mod->flush()，再遍历清除所有 entry dirty
 *   - init:         调用户回调 mod->init()
 * ================================================================ */

/** @brief App 模块 dirty 标记 — 直接将模块标记为 dirty */
static void app_module_mark_dirty(param_module_node_t *m, uint16_t local_id)
{
    (void)local_id;
    m->dirty = 1;
}

/**
 * @brief App 模块清除 dirty — 遍历所有 entry，只有全部 clean 才清模块 dirty
 */
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

/**
 * @brief App 模块 flush — 先调用户回调，再清除所有 entry dirty
 *
 * 即使 flush 回调失败，也会继续清除 dirty 标记。
 */
static int app_module_flush(param_module_node_t *m)
{
    param_module_t *mod = (param_module_t *)m;
    if (mod->flush) {
        int ret = mod->flush(mod->flush_ctx);
        if (ret != PARAM_OK) return ret;
    }
    for (uint16_t i = 0; i < m->param_count; i++) {
        param_entry_t *e = m->table[i];
        if (e) param_entry_clear_dirty(e);
    }
    m->dirty = 0;
    return PARAM_OK;
}

/** @brief App 模块 reset — 清除模块 dirty 标记 */
static void app_module_reset(param_module_node_t *m)
{
    m->dirty = 0;
}

/**
 * @brief App 模块 init — 调用户 mod->init 回调，清理 dirty
 *
 * @param m 模块节点
 * @return PARAM_OK 成功
 */
static int app_module_init(param_module_node_t *m)
{
    param_module_t *mod = (param_module_t *)m;
    m->dirty = 0;
    if (mod->init)
        return mod->init(mod->flush_ctx);
    return PARAM_OK;
}

/** @brief App 模块 deinit — 空操作 */
static void app_module_deinit(param_module_node_t *m)
{
    (void)m;
}

const param_module_vtable_t app_module_vtable = {
    .mark_dirty  = app_module_mark_dirty,
    .clear_dirty = app_module_clear_dirty,
    .flush       = app_module_flush,
    .init        = app_module_init,
    .reset       = app_module_reset,
    .deinit      = app_module_deinit,
};
