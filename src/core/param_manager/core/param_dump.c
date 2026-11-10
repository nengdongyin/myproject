/**
 * @file param_dump.c
 * @brief 参数格式化输出 — 将运行时参数状态转为可读文本
 *
 * 采用列对齐的固定宽度格式化方案:
 *   每列有固定宽度 (COL_xxx_W)，内容不足宽度则空格填充，超出则截断。
 *
 * 覆盖全部 8 种 param_type_t (UINT/INT/FLOAT/BOOL/ENUM/BLOB/STRING/EXEC)，
 * 通过 g_dump_formatters[PARAM_TYPE_COUNT] 分派表查表调度 ——
 * 分派只看条目类型, 与条目属于 App 还是 IP 无关 (不按 vtable 分流)。
 *
 * 受 PARAM_DUMP_ENABLE 宏控制: 禁用时整个文件为空（仅含 #include）。
 */

#include "param_dump.h"

#if PARAM_DUMP_ENABLE

#include "param_manager.h"
#include "param_manager_internal.h"
#include "port.h"
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
#define COL_RANGE_W  24  /**< 范围列: [min,max] — 覆盖全 UINT32/INT32 */
#define COL_DIRTY_W  4   /**< dirty 列: d=X */
#define COL_FLAGS_W  7   /**< flags 列: f=0xXX */
/** @} */

/**
 * @brief 列格式化填充 — printf 风格格式化后空格补全到固定宽度
 *
 * 先调用 vsnprintf 格式化内容，若输出不足 width 字符则用空格填充剩余位置。
 * 结果始终右截断到 width 字符，末尾补 '\0' 形成合法 C 字符串。
 *
 * @param dst   输出缓冲区（长度至少 width+1）
 * @param width 列宽度（字符数）
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
 * @brief 整列填充空格 — 用于无可显示内容的列
 *
 * @param dst   输出缓冲区（长度至少 width+1）
 * @param width 列宽度
 */
static void col_empty(char *dst, uint16_t width)
{
    memset(dst, ' ', width);
    dst[width] = '\0';
}

/**
 * @brief 参数标志位转为可读字符串（单字符映射）
 *
 * 映射关系: bit0=PERSIST→'P', bit1=READONLY→'R', bit2=HIDDEN→'H',
 *           bit3=DEPRECATED→'D', bit4=EXEC→'E'。
 * 无任何标志时返回 "-"。输出写入调用者提供的缓冲区，可重入。
 *
 * @param flags    属性标志位（param_flag_t 位或组合）
 * @param buf      输出缓冲区（调用者分配）
 * @param buf_size 缓冲区大小
 * @return buf 指针（与输入相同），如 "PR"、"E"、"-"
 */
static const char *flags_to_str(uint16_t flags, char *buf, uint16_t buf_size)
{
    static const char map[] = { 'P', 'R', 'H', 'D', 'E' };
    int pos = 0;
    if (!flags) {
        if (buf_size >= 2) {
            buf[0] = '-'; buf[1] = '\0';
        } else if (buf_size == 1) {
            buf[0] = '\0';
        }
        /* buf_size == 0: nothing writable, return buf as-is */
        return buf;
    }
    for (int i = 0; i < 5 && pos < (int)buf_size - 1; i++)
        if (flags & (1u << i))
            buf[pos++] = map[i];
    buf[pos] = '\0';
    return buf;
}

/** 参数条目格式化函数类型 */
typedef void (*param_entry_fmt_fn)(param_entry_t *e, const char *name,
                                     char *line, uint16_t size);

/**
 * @brief 公共列格式化 — 所有类型格式化器共用的列拼接函数
 *
 * 封装 6 列（ID / 名称 / 值 / 范围 / dirty / flags）的填充与拼接。
 * 调用者只需提供已格式化的值字符串 val_str 和范围字符串 range_str
 * （可为 NULL 表示无范围信息），其余列从此函数内部从 entry 提取。
 *
 * 输出格式: `[XXXX] NAME TYPE=VALUE [min,max] d=X f=XX\n`
 *
 * @param e         参数条目指针
 * @param name      参数调试名称
 * @param val_str   已格式化的值字符串（如 "UINT=100(0x64)"）
 * @param range_str 范围字符串（如 "[0,255]"，NULL 表示无范围）
 * @param line      输出行缓冲区
 * @param size      缓冲区大小
 */
static void dump_line(param_entry_t *e, const char *name,
                      const char *val_str, const char *range_str,
                      char *line, uint16_t size)
{
    uint16_t flags = entry_flags(e);
    uint8_t dirty = entry_dirty(e);
    char c_id[COL_ID_W + 1], c_nm[COL_NAME_W + 1],
         c_vl[COL_VALUE_W + 1], c_rg[COL_RANGE_W + 1],
         c_dt[COL_DIRTY_W + 1], c_fl[COL_FLAGS_W + 1], fb[8];

    col_fill(c_id, COL_ID_W, " [%04X]", (unsigned)PARAM_LOCAL_ID(e->param_id));
    col_fill(c_nm, COL_NAME_W, "%s", name);
    col_fill(c_vl, COL_VALUE_W, "%s", val_str);
    if (range_str)
        col_fill(c_rg, COL_RANGE_W, "%s", range_str);
    else
        col_empty(c_rg, COL_RANGE_W);
    col_fill(c_dt, COL_DIRTY_W, "d=%u", dirty);
    col_fill(c_fl, COL_FLAGS_W, "f=%s", flags_to_str(flags, fb, sizeof(fb)));
    snprintf(line, size, "%s%s%s%s%s%s\n", c_id, c_nm, c_vl, c_rg, c_dt, c_fl);
}

/* ---- 各类型的格式化器 — 仅构造 val_str / range_str ---- */

/**
 * @brief 格式化 UINT 类型参数
 *
 * 格式: [XXXX] NAME UINT=val(0xXXXX) [min,max] d=X f=0xXX
 */
static void fmt_uint(param_entry_t *e, const char *name,
                     char *line, uint16_t size)
{
    param_range_entry_t *re = (param_range_entry_t *)e;
    param_value_t v = *entry_cache(e);
    char val[40], rng[40] = "";

    snprintf(val, sizeof(val), "UINT=%lu(0x%08X)", (unsigned long)v.u32, (unsigned)v.u32);
    if (re->has_range)
        snprintf(rng, sizeof(rng), "[%lu,%lu]", (unsigned long)re->min.u32, (unsigned long)re->max.u32);
    dump_line(e, name, val, rng[0] ? rng : NULL, line, size);
}

/** @brief 格式化 INT 类型: INT=val [min,max] d=X f=0xXX */
static void fmt_int(param_entry_t *e, const char *name,
                    char *line, uint16_t size)
{
    param_range_entry_t *re = (param_range_entry_t *)e;
    param_value_t v = *entry_cache(e);
    char val[40], rng[40] = "";

    snprintf(val, sizeof(val), "INT=%ld", (long)v.i32);
    if (re->has_range)
        snprintf(rng, sizeof(rng), "[%ld,%ld]", (long)re->min.i32, (long)re->max.i32);
    dump_line(e, name, val, rng[0] ? rng : NULL, line, size);
}

/** @brief 格式化 FLOAT 类型: FLOAT=val [min,max] d=X f=0xXX */
static void fmt_float(param_entry_t *e, const char *name,
                      char *line, uint16_t size)
{
    param_range_entry_t *re = (param_range_entry_t *)e;
    param_value_t v = *entry_cache(e);
    char val[40], rng[40] = "";

    snprintf(val, sizeof(val), "FLOAT=%.3f", (double)v.f32);
    if (re->has_range)
        snprintf(rng, sizeof(rng), "[%.3f,%.3f]", (double)re->min.f32, (double)re->max.f32);
    dump_line(e, name, val, rng[0] ? rng : NULL, line, size);
}

/** @brief 格式化 BOOL 类型: BOOL=true/false d=X f=0xXX */
static void fmt_bool(param_entry_t *e, const char *name,
                     char *line, uint16_t size)
{
    param_value_t v = *entry_cache(e);
    char val[16];
    snprintf(val, sizeof(val), "BOOL=%s", v.b ? "true" : "false");
    dump_line(e, name, val, NULL, line, size);
}

/** @brief 格式化 EXEC 类型: EXEC d=X f=0xXX */
static void fmt_exec(param_entry_t *e, const char *name,
                     char *line, uint16_t size)
{
    dump_line(e, name, "EXEC", NULL, line, size);
}

/** @brief 格式化 ENUM 类型: ENUM=val n=count d=X f=0xXX */
static void fmt_enum(param_entry_t *e, const char *name,
                     char *line, uint16_t size)
{
    param_enum_entry_t *ee = (param_enum_entry_t *)e;
    param_value_t v = *entry_cache(e);
    char val[40];
    snprintf(val, sizeof(val), "ENUM=%ld n=%u", (long)v.i32, ee->enum_count);
    dump_line(e, name, val, NULL, line, size);
}

/**
 * @brief 格式化 BLOB 类型: 显示前 9 字节的十六进制 (空格分隔)
 *
 * 格式: [XXXX] NAME BLOB=sz=N[XX XX XX ...] d=X f=0xXX
 * 每字节两位大写十六进制，空格分隔。若 blob 大小超过 9 则末尾加 "..."
 */
static void fmt_blob(param_entry_t *e, const char *name,
                     char *line, uint16_t size)
{
    param_blob_entry_t *be = (param_blob_entry_t *)e;
    const uint8_t *data = (const uint8_t *)entry_cache(e)->ptr;
    char val[72];

    if (!data) {
        dump_line(e, name, "BLOB=(null)", NULL, line, size);
        return;
    }

    uint16_t show = be->blob_size < 9u ? be->blob_size : 9u;
    int pos = snprintf(val, sizeof(val), "BLOB=sz=%u[", be->blob_size);
    for (uint16_t i = 0; i < show && pos < (int)sizeof(val) - 5; i++)
        pos += snprintf(val + pos, sizeof(val) - pos,
                        "%s%02X", i > 0 ? " " : "", data[i]);
    if (be->blob_size > 9u)
        pos += snprintf(val + pos, sizeof(val) - pos, "...");
    snprintf(val + pos, sizeof(val) - pos, "]");
    dump_line(e, name, val, NULL, line, size);
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
    char val[COL_VALUE_W + 1];

    if (!data || !*data) {
        snprintf(val, sizeof(val), "STRING=\"\"");
    } else {
        uint16_t show = se->max_len < 16u ? se->max_len : 16u;
        const char *nul = memchr(data, '\0', show);
        uint16_t dlen = nul ? (uint16_t)(nul - data) : show;
        snprintf(val, sizeof(val), "STRING=\"%.*s%s\"",
                 dlen, data, (dlen < (uint16_t)strlen(data)) ? "..." : "");
    }
    dump_line(e, name, val, NULL, line, size);
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
 * @brief 按 param_type_t 分派到对应格式化器 (App / IP 通用)
 *
 * 格式化只依赖条目类型, 与条目属于哪个子系统无关: App 与 IP 的
 * UINT/INT/FLOAT/BOOL/ENUM/EXEC 结构体布局完全一致 (同一 PARAM_ENTRY_HEAD),
 * BLOB/STRING 亦各有专用格式化器 (无范围语义), 因此不需要也不应
 * 按 vtable 分流 —— 基类代码不命名派生侧 vtable 符号
 * (依赖方向: 派生 → 基类)。
 */
static void fmt_by_type(param_entry_t *e, const char *name,
                        char *line, uint16_t size)
{
    param_type_t t = entry_type(e);

    if (t < PARAM_TYPE_COUNT && g_dump_formatters[t]) {
        g_dump_formatters[t](e, name, line, size);
        return;
    }

    /* 未知/损坏类型 fallback — 类型字段越界时不应假设 u32 */
    char val[COL_VALUE_W + 1];
    snprintf(val, sizeof(val), "type=%d", (int)t);
    dump_line(e, name, val, NULL, line, size);
}

/**
 * @brief 格式化单个参数条目为一整行文本
 *
 * 按 param_type_t 分派, 与条目所属子系统无关 (无需判别 App / IP)。
 * 若 e 为 NULL 或未装配 vtable, line 输出为空字符串;
 * 类型越界时输出 "type=N" fallback 行。
 */
void param_dump_entry(param_entry_t *e, char *line, uint16_t size)
{
    line[0] = '\0';
    if (!e || !e->vtable) return;

    fmt_by_type(e, param_entry_name(e), line, size);
}

/** dump 输出回调 — 全局单例, 由 param_dump_set_output 注册 */
static param_dump_fn g_dump_output = NULL;

/**
 * @brief 注册 dump 输出回调
 *
 * @param cb 输出回调; NULL 表示关闭 dump 输出
 */
void param_dump_set_output(param_dump_fn cb)
{
    g_dump_output = cb;
}

/** dump 遍历上下文 */
typedef struct {
    uint16_t       module_id;
    void          *user_data;
} dump_ctx_t;

/**
 * @brief 模块遍历回调 — 逐模块、逐参数输出格式化行
 *
 * 先输出模块标题行 "[ModuleName] id=0xXXXX params=N dirty=X"，然后逐参数调用
 * param_dump_entry() 格式化。每行通过 g_dump_output 回调输出。
 */
static void dump_module_iter(param_module_node_t *m, void *ctx)
{
    dump_ctx_t *dc = (dump_ctx_t *)ctx;
    if (dc->module_id != 0 && m->module_id != dc->module_id) return;

    char line[256];
    snprintf(line, sizeof(line), "[%s] id=0x%04X params=%u dirty=%u\n",
             m->name ? m->name : "?", m->module_id,
             m->param_count, m->dirty);
    g_dump_output(line, dc->user_data);

    for (uint16_t i = 0; i < m->param_count; i++) {
        param_entry_t *e = m->table[i];
        if (!e) continue;
        param_dump_entry(e, line, sizeof(line));
        if (line[0]) g_dump_output(line, dc->user_data);
    }
}

/**
 * @brief 打印所有/指定模块的参数状态
 *
 * module_id==0 时遍历所有模块，否则只输出指定模块。
 * 通过 param_module_foreach 遍历模块链表 (即 MODULE_INIT_ORDER 顺序)。
 */
void param_dump(uint16_t module_id, void *user_data)
{
    if (!g_dump_output) return;
    dump_ctx_t ctx = { module_id, user_data };
    int ret = param_module_foreach(dump_module_iter, &ctx);
    (void)ret;
}

#endif /* PARAM_DUMP_ENABLE */
