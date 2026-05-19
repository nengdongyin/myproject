/**
 * @file param_dump.c
 * @brief 参数格式化输出 — 将运行时参数状态转为可读文本
 *
 * 采用列对齐的固定宽度格式化方案:
 *   每列有固定宽度 (COL_xxx_W)，内容不足宽度则空格填充，超出则截断。
 *
 * 支持 6 种 App 类型 (UINT/INT/FLOAT/BOOL/ENUM/BLOB) 及 IP 类型的格式化，
 * 通过 g_dump_formatters[PARAM_TYPE_COUNT] 分派表查表调度。
 */

#include "param_dump.h"
#include "param_manager.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/**
 * @name 列宽度定义
 * @{
 */
#define COL_ID_W     7   /**< ID 列: [XXXX] */
#define COL_NAME_W   22  /**< 名称列 */
#define COL_VALUE_W  30  /**< 值列: TYPE=VALUE */
#define COL_RANGE_W  14  /**< 范围列: [min,max] */
#define COL_DIRTY_W  4   /**< dirty 列: d=X */
#define COL_FLAGS_W  7   /**< flags 列: f=0xXX */
/** @} */

/**
 * @brief 列格式化填充 — 不足宽度空格补全
 *
 * 用于 printf 风格的格式化，结果右截断到 width 字符。
 *
 * @param dst   输出缓冲区
 * @param width 列宽度
 * @param fmt   格式化字符串
 * @param ...   格式化参数
 */
static void col_fill(char *dst, uint16_t width, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(dst, width + 1, fmt, args);
    va_end(args);
    if (n >= 0 && n < (int)width)
        memset(dst + n, ' ', width - n);
    dst[width] = '\0';
}

/**
 * @brief 整列填充空格 (无数据)
 */
static void col_empty(char *dst, uint16_t width)
{
    memset(dst, ' ', width);
    dst[width] = '\0';
}

/**
 * @brief 参数标志位转为可读字符串
 *
 * 映射: P=PERSIST, R=READONLY, H=HIDDEN, D=DEPRECATED, E=EXEC。
 * 无标志时返回 "-"。输出缓冲区为静态数组，不可重入。
 *
 * @param flags 属性标志位
 * @return 静态缓冲区指针 (如 "PR"、"E"、"-")
 */
static const char *flags_to_str(uint16_t flags)
{
    static const char map[] = { 'P', 'R', 'H', 'D', 'E' };
    static char buf[6];
    int pos = 0;
    if (!flags) {
        buf[0] = '-'; buf[1] = '\0';
        return buf;
    }
    for (int i = 0; i < 5 && pos < 5; i++)
        if (flags & (1u << i))
            buf[pos++] = map[i];
    buf[pos] = '\0';
    return buf;
}

/** 参数条目格式化函数类型 */
typedef void (*param_entry_fmt_fn)(param_entry_t *e, const char *name,
                                     char *line, uint16_t size);

/* ---- 各类型的格式化器 ---- */

/**
 * @brief 格式化 UINT 类型参数为一行文本
 *
 * 格式: [XXXX] NAME UINT=val(0xXXXX) [min,max] d=X f=0xXX
 */
static void fmt_uint(param_entry_t *e, const char *name,
                     char *line, uint16_t size)
{
    param_range_entry_t *re = (param_range_entry_t *)e;
    param_value_t v = *entry_cache(e);
    uint16_t flags = entry_flags(e);
    uint8_t dirty = entry_dirty(e);

    char c_id[COL_ID_W + 1];
    char c_nm[COL_NAME_W + 1];
    char c_vl[COL_VALUE_W + 1];
    char c_rg[COL_RANGE_W + 1];
    char c_dt[COL_DIRTY_W + 1];
    char c_fl[COL_FLAGS_W + 1];

    col_fill(c_id, COL_ID_W, " [%04X]", (unsigned)e->param_id & 0xFFFF);
    col_fill(c_nm, COL_NAME_W, "%s", name);
    col_fill(c_vl, COL_VALUE_W, "UINT=%lu(0x%08X)", (unsigned long)v.u32, (unsigned)v.u32);
    if (re->has_range)
        col_fill(c_rg, COL_RANGE_W, "[%lu,%lu]", (unsigned long)re->min.u32, (unsigned long)re->max.u32);
    else
        col_empty(c_rg, COL_RANGE_W);
    col_fill(c_dt, COL_DIRTY_W, "d=%u", dirty);
    col_fill(c_fl, COL_FLAGS_W, "f=%s", flags_to_str(flags));

    snprintf(line, size, "%s%s%s%s%s%s\n", c_id, c_nm, c_vl, c_rg, c_dt, c_fl);
}

/** @brief 格式化 INT 类型: INT=val [min,max] d=X f=0xXX */
static void fmt_int(param_entry_t *e, const char *name,
                    char *line, uint16_t size)
{
    param_range_entry_t *re = (param_range_entry_t *)e;
    param_value_t v = *entry_cache(e);
    uint16_t flags = entry_flags(e);
    uint8_t dirty = entry_dirty(e);

    char c_id[COL_ID_W + 1];
    char c_nm[COL_NAME_W + 1];
    char c_vl[COL_VALUE_W + 1];
    char c_rg[COL_RANGE_W + 1];
    char c_dt[COL_DIRTY_W + 1];
    char c_fl[COL_FLAGS_W + 1];

    col_fill(c_id, COL_ID_W, " [%04X]", (unsigned)e->param_id & 0xFFFF);
    col_fill(c_nm, COL_NAME_W, "%s", name);
    col_fill(c_vl, COL_VALUE_W, "INT=%ld", (long)v.i32);
    if (re->has_range)
        col_fill(c_rg, COL_RANGE_W, "[%ld,%ld]", (long)re->min.i32, (long)re->max.i32);
    else
        col_empty(c_rg, COL_RANGE_W);
    col_fill(c_dt, COL_DIRTY_W, "d=%u", dirty);
    col_fill(c_fl, COL_FLAGS_W, "f=%s", flags_to_str(flags));

    snprintf(line, size, "%s%s%s%s%s%s\n", c_id, c_nm, c_vl, c_rg, c_dt, c_fl);
}

/** @brief 格式化 FLOAT 类型: FLOAT=val [min,max] d=X f=0xXX */
static void fmt_float(param_entry_t *e, const char *name,
                      char *line, uint16_t size)
{
    param_range_entry_t *re = (param_range_entry_t *)e;
    param_value_t v = *entry_cache(e);
    uint16_t flags = entry_flags(e);
    uint8_t dirty = entry_dirty(e);

    char c_id[COL_ID_W + 1];
    char c_nm[COL_NAME_W + 1];
    char c_vl[COL_VALUE_W + 1];
    char c_rg[COL_RANGE_W + 1];
    char c_dt[COL_DIRTY_W + 1];
    char c_fl[COL_FLAGS_W + 1];

    col_fill(c_id, COL_ID_W, " [%04X]", (unsigned)e->param_id & 0xFFFF);
    col_fill(c_nm, COL_NAME_W, "%s", name);
    col_fill(c_vl, COL_VALUE_W, "FLOAT=%.3f", (double)v.f32);
    if (re->has_range)
        col_fill(c_rg, COL_RANGE_W, "[%.3f,%.3f]", (double)re->min.f32, (double)re->max.f32);
    else
        col_empty(c_rg, COL_RANGE_W);
    col_fill(c_dt, COL_DIRTY_W, "d=%u", dirty);
    col_fill(c_fl, COL_FLAGS_W, "f=%s", flags_to_str(flags));

    snprintf(line, size, "%s%s%s%s%s%s\n", c_id, c_nm, c_vl, c_rg, c_dt, c_fl);
}

/** @brief 格式化 BOOL 类型: BOOL=true/false d=X f=0xXX */
static void fmt_bool(param_entry_t *e, const char *name,
                     char *line, uint16_t size)
{
    param_value_t v = *entry_cache(e);
    uint16_t flags = entry_flags(e);
    uint8_t dirty = entry_dirty(e);

    char c_id[COL_ID_W + 1];
    char c_nm[COL_NAME_W + 1];
    char c_vl[COL_VALUE_W + 1];
    char c_rg[COL_RANGE_W + 1];
    char c_dt[COL_DIRTY_W + 1];
    char c_fl[COL_FLAGS_W + 1];

    col_fill(c_id, COL_ID_W, " [%04X]", (unsigned)e->param_id & 0xFFFF);
    col_fill(c_nm, COL_NAME_W, "%s", name);
    col_fill(c_vl, COL_VALUE_W, "BOOL=%s", v.b ? "true" : "false");
    col_empty(c_rg, COL_RANGE_W);
    col_fill(c_dt, COL_DIRTY_W, "d=%u", dirty);
    col_fill(c_fl, COL_FLAGS_W, "f=%s", flags_to_str(flags));

    snprintf(line, size, "%s%s%s%s%s%s\n", c_id, c_nm, c_vl, c_rg, c_dt, c_fl);
}

/** @brief 格式化 EXEC 类型: EXEC d=X f=0xXX */
static void fmt_exec(param_entry_t *e, const char *name,
                     char *line, uint16_t size)
{
    uint16_t flags = entry_flags(e);
    uint8_t dirty = entry_dirty(e);

    char c_id[COL_ID_W + 1];
    char c_nm[COL_NAME_W + 1];
    char c_vl[COL_VALUE_W + 1];
    char c_rg[COL_RANGE_W + 1];
    char c_dt[COL_DIRTY_W + 1];
    char c_fl[COL_FLAGS_W + 1];

    col_fill(c_id, COL_ID_W, " [%04X]", (unsigned)e->param_id & 0xFFFF);
    col_fill(c_nm, COL_NAME_W, "%s", name);
    col_fill(c_vl, COL_VALUE_W, "EXEC");
    col_empty(c_rg, COL_RANGE_W);
    col_fill(c_dt, COL_DIRTY_W, "d=%u", dirty);
    col_fill(c_fl, COL_FLAGS_W, "f=%s", flags_to_str(flags));

    snprintf(line, size, "%s%s%s%s%s%s\n", c_id, c_nm, c_vl, c_rg, c_dt, c_fl);
}

/** @brief 格式化 ENUM 类型: ENUM=val n=count d=X f=0xXX */
static void fmt_enum(param_entry_t *e, const char *name,
                     char *line, uint16_t size)
{
    param_enum_entry_t *ee = (param_enum_entry_t *)e;
    param_value_t v = *entry_cache(e);
    uint16_t flags = entry_flags(e);
    uint8_t dirty = entry_dirty(e);

    char c_id[COL_ID_W + 1];
    char c_nm[COL_NAME_W + 1];
    char c_vl[COL_VALUE_W + 1];
    char c_rg[COL_RANGE_W + 1];
    char c_dt[COL_DIRTY_W + 1];
    char c_fl[COL_FLAGS_W + 1];

    col_fill(c_id, COL_ID_W, " [%04X]", (unsigned)e->param_id & 0xFFFF);
    col_fill(c_nm, COL_NAME_W, "%s", name);
    col_fill(c_vl, COL_VALUE_W, "ENUM=%ld n=%u", (long)v.i32, ee->enum_count);
    col_empty(c_rg, COL_RANGE_W);
    col_fill(c_dt, COL_DIRTY_W, "d=%u", dirty);
    col_fill(c_fl, COL_FLAGS_W, "f=%s", flags_to_str(flags));

    snprintf(line, size, "%s%s%s%s%s%s\n", c_id, c_nm, c_vl, c_rg, c_dt, c_fl);
}

/**
 * @brief 格式化 BLOB 类型: 显示前 6 字节的十六进制 (空格分隔)
 *
 * 格式: [XXXX] NAME BLOB=sz=N[XX XX XX ...] d=X f=0xXX
 * 每字节两位大写十六进制，空格分隔。若 blob 大小超过 6 则末尾加 "..."
 */
static void fmt_blob(param_entry_t *e, const char *name,
                     char *line, uint16_t size)
{
    param_blob_entry_t *be = (param_blob_entry_t *)e;
    param_value_t v = *entry_cache(e);
    uint16_t flags = entry_flags(e);
    uint8_t dirty = entry_dirty(e);
    const uint8_t *data = (const uint8_t *)v.ptr;

    char c_id[COL_ID_W + 1];
    char c_nm[COL_NAME_W + 1];
    char c_vl[COL_VALUE_W + 1];
    char c_rg[COL_RANGE_W + 1];
    char c_dt[COL_DIRTY_W + 1];
    char c_fl[COL_FLAGS_W + 1];

    uint16_t show = be->blob_size < 9u ? be->blob_size : 9u;
    char bl_buf[40];
    int pos = snprintf(bl_buf, sizeof(bl_buf), "sz=%u[", be->blob_size);
    for (uint16_t i = 0; i < show && pos < (int)sizeof(bl_buf) - 5; i++)
        pos += snprintf(bl_buf + pos, sizeof(bl_buf) - pos,
                        "%s%02X", i > 0 ? " " : "", data[i]);
    if (be->blob_size > 9u)
        pos += snprintf(bl_buf + pos, sizeof(bl_buf) - pos, "...");
    snprintf(bl_buf + pos, sizeof(bl_buf) - pos, "]");
    col_fill(c_id, COL_ID_W, " [%04X]", (unsigned)e->param_id & 0xFFFF);
    col_fill(c_nm, COL_NAME_W, "%s", name);
    col_fill(c_vl, COL_VALUE_W, "BLOB=%s", bl_buf);
    col_empty(c_rg, COL_RANGE_W);
    col_fill(c_dt, COL_DIRTY_W, "d=%u", dirty);
    col_fill(c_fl, COL_FLAGS_W, "f=%s", flags_to_str(flags));

    snprintf(line, size, "%s%s%s%s%s%s\n", c_id, c_nm, c_vl, c_rg, c_dt, c_fl);
}

/**
 * @brief 格式化 STRING 类型: 输出引号包裹的可读字符串
 *
 * 格式: [XXXX] NAME STRING="val" d=X f=0xXX
 * 超长字符串截断并追加 "..."
 */
static void fmt_string(param_entry_t *e, const char *name,
                       char *line, uint16_t size)
{
    param_string_entry_t *se = (param_string_entry_t *)e;
    const char *data = (const char *)se->cache.ptr;
    uint16_t flags = entry_flags(e);
    uint8_t dirty = entry_dirty(e);

    char c_id[COL_ID_W + 1];
    char c_nm[COL_NAME_W + 1];
    char c_vl[COL_VALUE_W + 1];
    char c_rg[COL_RANGE_W + 1];
    char c_dt[COL_DIRTY_W + 1];
    char c_fl[COL_FLAGS_W + 1];

    char str_buf[20];
    if (!data || !*data) {
        snprintf(str_buf, sizeof(str_buf), "\"\"");
    } else {
        uint16_t show = se->max_len < 16u ? se->max_len : 16u;
        uint16_t dlen = (uint16_t)strnlen(data, show);
        snprintf(str_buf, sizeof(str_buf), "\"%.*s%s\"",
                 dlen, data, (dlen < (uint16_t)strlen(data)) ? "..." : "");
    }

    col_fill(c_id, COL_ID_W, " [%04X]", (unsigned)e->param_id & 0xFFFF);
    col_fill(c_nm, COL_NAME_W, "%s", name);
    col_fill(c_vl, COL_VALUE_W, "STRING=%s", str_buf);
    col_empty(c_rg, COL_RANGE_W);
    col_fill(c_dt, COL_DIRTY_W, "d=%u", dirty);
    col_fill(c_fl, COL_FLAGS_W, "f=%s", flags_to_str(flags));

    snprintf(line, size, "%s%s%s%s%s%s\n", c_id, c_nm, c_vl, c_rg, c_dt, c_fl);
}

/**
 * @brief Dump 分派表 — 按 param_type_t 索引选择格式化器
 */
static const param_entry_fmt_fn g_dump_formatters[PARAM_TYPE_COUNT] = {
    [PARAM_TYPE_UINT]   = fmt_uint,
    [PARAM_TYPE_INT]    = fmt_int,
    [PARAM_TYPE_FLOAT]  = fmt_float,
    [PARAM_TYPE_BOOL]   = fmt_bool,
    [PARAM_TYPE_ENUM]   = fmt_enum,
    [PARAM_TYPE_BLOB]   = fmt_blob,
    [PARAM_TYPE_STRING] = fmt_string,
    [PARAM_TYPE_EXEC]   = fmt_exec,
};

/**
 * @brief IP 参数格式化 — 不访问 range 字段 (ip_param_t 无 has_range/min/max)
 *
 * 与 App 同类型共享值格式化逻辑，但范围列固定为空。
 * 通过 PARAM_ENTRY_HEAD 统一访问器读取 cache/type/flags/dirty，
 * 按 param_type_t 分派到具体值格式，绝不对 param_range_entry_t 强制转型。
 */
static void fmt_ip(param_entry_t *e, const char *name,
                   char *line, uint16_t size)
{
    param_value_t v = *entry_cache(e);
    param_type_t t = entry_type(e);
    uint16_t flags = entry_flags(e);
    uint8_t dirty = entry_dirty(e);

    char c_id[COL_ID_W + 1];
    char c_nm[COL_NAME_W + 1];
    char c_vl[COL_VALUE_W + 1];
    char c_rg[COL_RANGE_W + 1];
    char c_dt[COL_DIRTY_W + 1];
    char c_fl[COL_FLAGS_W + 1];

    col_fill(c_id, COL_ID_W, " [%04X]", (unsigned)e->param_id & 0xFFFF);
    col_fill(c_nm, COL_NAME_W, "%s", name);

    switch (t) {
    case PARAM_TYPE_UINT:
        col_fill(c_vl, COL_VALUE_W, "UINT=%lu(0x%08X)", (unsigned long)v.u32, (unsigned)v.u32);
        break;
    case PARAM_TYPE_INT:
        col_fill(c_vl, COL_VALUE_W, "INT=%ld", (long)v.i32);
        break;
    case PARAM_TYPE_FLOAT:
        col_fill(c_vl, COL_VALUE_W, "FLOAT=%.3f", (double)v.f32);
        break;
    case PARAM_TYPE_BOOL:
        col_fill(c_vl, COL_VALUE_W, "BOOL=%s", v.b ? "true" : "false");
        break;
    case PARAM_TYPE_EXEC:
        col_fill(c_vl, COL_VALUE_W, "EXEC");
        break;
    default:
        col_fill(c_vl, COL_VALUE_W, "IP=%lu(0x%08X)", (unsigned long)v.u32, (unsigned)v.u32);
        break;
    }

    col_empty(c_rg, COL_RANGE_W);
    col_fill(c_dt, COL_DIRTY_W, "d=%u", dirty);
    col_fill(c_fl, COL_FLAGS_W, "f=%s", flags_to_str(flags));

    snprintf(line, size, "%s%s%s%s%s%s\n", c_id, c_nm, c_vl, c_rg, c_dt, c_fl);
}

/**
 * @brief 格式化单个参数条目为一整行文本
 *
 * 根据 vtable 类型 (App/IP) 和 param_type_t 自动分派到对应格式化器。
 * 若 e 为 NULL 或类型不支持，line 输出为空字符串。
 */
void param_dump_entry(param_entry_t *e, char *line, uint16_t size)

{
    line[0] = '\0';
    if (!e) return;

    param_type_t t = entry_type(e);
    const char *name = param_entry_name(e);

    if (is_app(e) && t < PARAM_TYPE_COUNT && g_dump_formatters[t]) {
        g_dump_formatters[t](e, name, line, size);
    } else if (is_ip(e)) {
        fmt_ip(e, name, line, size);
    }
}

/** dump 遍历上下文 */
typedef struct {
    uint16_t       module_id;
    param_dump_fn  cb;
    void          *user_data;
} dump_ctx_t;

/**
 * @brief 模块遍历回调 — 逐模块、逐参数输出格式化行
 *
 * 先输出模块标题行 "[ModuleName] id=0xXXXX params=N dirty=X"，然后逐参数调用
 * param_dump_entry() 格式化。每行通过 dc->cb 回调输出。
 */
static void dump_module_iter(param_module_node_t *m, void *ctx)
{
    dump_ctx_t *dc = (dump_ctx_t *)ctx;
    if (dc->module_id != 0 && m->module_id != dc->module_id) return;

    char line[256];
    snprintf(line, sizeof(line), "[%s] id=0x%04X params=%u dirty=%u\n",
             m->name ? m->name : "?", m->module_id,
             m->param_count, m->dirty);
    dc->cb(line, dc->user_data);

    for (uint16_t i = 0; i < m->param_count; i++) {
        param_entry_t *e = m->table[i];
        if (!e) continue;
        param_dump_entry(e, line, sizeof(line));
        if (line[0]) dc->cb(line, dc->user_data);
    }
}

/**
 * @brief 打印所有/指定模块的参数状态
 *
 * module_id==0 时遍历所有模块，否则只输出指定模块。
 * 通过 param_module_foreach 遍历模块链表 (即 MODULE_INIT_ORDER 顺序)。
 */
void param_dump(uint16_t module_id, param_dump_fn cb, void *user_data)
{
    if (!cb) return;
    dump_ctx_t ctx = { module_id, cb, user_data };
    param_module_foreach(dump_module_iter, &ctx);
}
