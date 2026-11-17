/**
 * @file param_storage_iniparser.c
 * @brief 基于 iniparser 库的持久化存储后端实现
 *
 * @details
 * 本文件实现 param_storage_drv_t 接口的 iniparser 适配层。
 * 采用工厂模式管理多存储实例:
 *   - g_ctx[MAX_INSTANCES] 静态池，按分区名查找/创建
 *   - 同一分区名返回同一实例（幂等）
 *   - param_storage_iniparser_create() 实现智能启动分区选择:
 *     读取 param_boot 文件 → 选择目标用户分区 → 空则回退 factory
 *
 * 每个参数以一行键值对存储在 [params] 段下, 键为 param_id 的 32 位 hex 形态,
 * 值为"<参数名>:<类型标签>:<字面量>"三段式 (参数名仅作可读标注, 不参与身份)。
 * 目标是 cat 一眼可读, 不必回头查 hex 转储:
 *   [params]
 *   0x00020000 = "boot_index:uint:3"
 *   0x00020001 = "exposure_gain:float:0.75"
 *   0x00020002 = "device_name:string:"CAM-01""
 *   0x00020003 = "calibration:blob:0A1B2C3D"
 * (最外层双引号是 iniparser 自己的转义, 不属于值文本)
 * 高 16 位 = module_id, 低 16 位 = local_id, 便于对照 module_ids.h。
 * 标签与字面量约定见下文 "值文本编解码" 一节; 无标签的旧形态
 * "<参数名>:<hex>" 仍按 hex 字节流解码, 因此格式演进不会丢旧数据。
 *
 * 每次 save() 全量重写 INI 文件，通过写临时文件 + rename 保证原子性。
 *
 * @attention 全局实例池 g_ctx[] 无锁保护，仅支持单线程使用。
 *
 * @note 本文件无条件提供完整实现 (不存在 stub 变体): 要不要用它由两件事决定 ——
 *   1. 构建层: 本文件是否被编入 (不编入则工厂符号不存在);
 *   2. 调用层: 启动流程是否调用 param_storage_iniparser_create()。
 * 不需要持久化的产品直接传 NULL 给 param_init()。
 *
 * @see param_storage_drv_t 接口定义
 * @see param_storage_iniparser.h 公开 API
 */

#include "param_storage_iniparser.h"
#include "param_storage_common.h"
/* 本后端对框架内部 API 的依赖只有两处, 且都只读:
   - param_entry_name(): 值的可读标注 (纯装饰, 不参与身份);
   - param_entry_find() + entry_type(): 保存时按类型选落盘文本形态
     (uint/int/float/... 只有条目类型知道)。解码侧不看条目表, 落盘文本
     自描述 (见"值文本编解码"), 因此条目清单变化不会污染读取路径。
   批量通路分别走 sink 注入 (恢复) 与 save_begin/save_commit (保存),
   本后端不遍历框架条目、也不直调条目 vtable。 */
#include "param_manager_internal.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

#include "iniparser.h"

/* ================================================================
 *  编译期常量
 * ================================================================ */

/** @brief INI 段名 — 所有参数键值对存放在此段下 */
#define INI_SECTION "params"

/** @brief 键名缓冲区大小 ("params:" + "0x" + 8 位 hex id + '\0') */
#define INI_KEY_MAX 32

/** @brief 参数名缓冲区 (调试名即 C 变量名, 超出则截断) */
#define INI_NAME_MAX 32

/** @brief 字面量缓冲区大小 (hex 形态支持 ≤255 字节的原始数据: 255*2+1=511)
 *
 * 此限制决定单个参数持久化的最大字节数, 具体随落盘形态变化:
 *   - UINT / INT / FLOAT / BOOL / ENUM → 十进制/布尔文本, 几十字节, 完全不受限
 *   - STRING → 明文形态 ≤ (HEX_VAL_MAX-3) 字符 (比 hex 形态大一倍);
 *              含控制字符而退化为 hex 时, 仍受限于此值的一半左右
 *   - BLOB   → hex 形态, 受限于此值的一半 (≤255 字节)
 *
 * 如需支持更大的 BLOB/STRING，将此值改为 2*N+1 (N 为目标最大字节数)
 * 或改用动态分配。 */
#define HEX_VAL_MAX 512

/** @brief 类型标签最大长度 (含 '\\0'), 最长者是 "string" */
#define INI_TAG_MAX 8

/** @brief 完整值缓冲区大小: "<name>:<tag>:<literal>" */
#define INI_VAL_MAX (INI_NAME_MAX + 1 + INI_TAG_MAX + 1 + HEX_VAL_MAX)

/**
 * @name 值文本标签
 *
 * 值 = "<name>:<tag>:<literal>", tag 直接采用 param_type_t 的小写名,
 * "hex" 是"原始字节流"的兜底标签 (类型未知 / 无法文本化时)。
 * 编码侧按条目类型选标签 (类型仅用于选格式), 解码侧只看标签、不回查
 * 条目表 —— 这样 storage 层不依赖"当前固件的条目清单", 落盘文本自描述。
 * @{
 */
#define INI_TAG_UINT   "uint"
#define INI_TAG_INT    "int"
#define INI_TAG_FLOAT  "float"
#define INI_TAG_BOOL   "bool"
#define INI_TAG_ENUM   "enum"
#define INI_TAG_STRING "string"
#define INI_TAG_BLOB   "blob"
#define INI_TAG_HEX    "hex"
/** @} */

/** @brief param_boot 文件名 (存储启动分区索引) */
#define BOOT_FILE "param_boot"

/** @brief param_boot 文件内容的文本表示最大长度 */
#define BOOT_VAL_MAX 4

/**
 * @brief INI 存储上下文 — 每个物理分区对应一个实例
 *
 * 实例通过 g_ctx[MAX_INSTANCES] 静态池管理，按 part_name 查找/创建。
 */
typedef struct {
    bool used;                /**< 此槽位是否已被分配 */
    char base_dir[128];       /**< 持久化根目录 (如 "/data/params") */
    char part_name[24];       /**< 分区名 (如 "param_user0") */
    char ini_path[256];       /**< 完整的 INI 文件路径 */
    dictionary *dict;         /**< iniparser 字典 (内存驻留) */
    bool dict_ready;          /**< 字典是否已加载 */
    bool defer_dump;          /**< 批量保存中: save 只改字典, 不落盘 (单线程前提) */
    param_storage_drv_t drv;  /**< 对外暴露的存储驱动接口 */
} iniparser_ctx_t;

/** @brief 全局存储实例池（静态分配） */
static iniparser_ctx_t g_ctx[MAX_INSTANCES];

/* 分区索引→分区名 的映射与 MAX_INSTANCES 断言由 param_storage_common.h 提供 */

/* ================================================================
 *  hex 编解码
 * ================================================================ */

/** @brief hex 字符→半字节, 非法字符返回 -1 */
static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'A' && c <= 'F') return (int)(c - 'A' + 10);
    if (c >= 'a' && c <= 'f') return (int)(c - 'a' + 10);
    return -1;
}

/**
 * @brief 字节数组→hex 字符串 (大写)
 *
 * @param data  输入字节数组
 * @param len   字节数
 * @param out   输出缓冲区 (至少 len*2+1 字节)
 */
static void hex_encode(const uint8_t *data, uint16_t len, char *out)
{
    static const char hex_chars[] = "0123456789ABCDEF";
    for (uint16_t i = 0; i < len; i++) {
        out[i * 2]     = hex_chars[data[i] >> 4];
        out[i * 2 + 1] = hex_chars[data[i] & 0x0F];
    }
    out[len * 2] = '\0';
}

/**
 * @brief hex 字符串→字节数组
 *
 * @param hex     hex 字符串 (偶数长度)
 * @param data    输出缓冲区
 * @param max_len 输出缓冲区容量
 * @param out_len [out] 解码字节数 (可 NULL)
 * @return PARAM_OK 成功; PARAM_ERR_OUT_OF_RANGE 存储值损坏 (奇数长度 /
 *         非 hex 字符) 或长度超出 max_len; PARAM_ERR_INVALID_ID hex 为 NULL
 */
static int hex_decode(const char *hex, uint8_t *data, uint16_t max_len, uint16_t *out_len)
{
    if (!hex) return PARAM_ERR_INVALID_ID;
    size_t hex_len = strlen(hex);
    if (hex_len % 2 != 0) return PARAM_ERR_OUT_OF_RANGE;
    /* 拒绝截断: 存储值超过缓冲区容量视为数据损坏 */
    if (hex_len / 2 > (size_t)max_len) return PARAM_ERR_OUT_OF_RANGE;

    uint16_t byte_len = (uint16_t)(hex_len / 2);
    for (uint16_t i = 0; i < byte_len; i++) {
        int hi = hex_nibble(hex[i * 2]);
        int lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return PARAM_ERR_OUT_OF_RANGE;
        data[i] = (uint8_t)((hi << 4) | lo);
    }
    if (out_len) *out_len = byte_len;
    return PARAM_OK;
}

/* ================================================================
 *  值文本编解码 — 把 ini 从"hex 转储"变成 cat 可读的文本
 *
 *  值格式: "<name>:<tag>:<literal>", 例:
 *     g_boot_index:uint:3        sensitivity:float:0.75
 *     device_name:string:"CAM-01"   calibration:blob:0A1B2C3D
 *
 *  编码侧按条目类型 (param_entry_find → entry_type) 选标签与字面量形态;
 *  解码侧只看标签, 不回查条目表。格式损坏/标签不识/超容量一律上报
 *  PARAM_ERR_OUT_OF_RANGE, 而不是把错类型的字节硬塞进缓存。
 *
 *  兼容: 无标签的旧形态 "<name>:<hex>" 仍按 hex 字节流解码。
 * ================================================================ */

/** @brief 无符号文本解析 (十进制或 0x 十六进制; 整串必须消费完) */
static bool parse_u32(const char *s, uint32_t *out)
{
    if (!s || *s == '\0' || *s == '-') return false; /* strtoul 会静默取负 */
    char *end = NULL;
    errno = 0;
    unsigned long v = strtoul(s, &end, 0);
    if (errno == ERANGE || end == s || *end != '\0') return false;
    if (v > 0xFFFFFFFFul) return false;
    *out = (uint32_t)v;
    return true;
}

/** @brief 有符号文本解析 (十进制或 0x 十六进制; 整串必须消费完) */
static bool parse_i32(const char *s, int32_t *out)
{
    if (!s || *s == '\0') return false;
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 0);
    if (errno == ERANGE || end == s || *end != '\0') return false;
    if (v < (long)INT32_MIN || v > (long)INT32_MAX) return false;
    *out = (int32_t)v;
    return true;
}

/** @brief bool 文本解析: true/false (大写亦可) 或 0/1/0x1 */
static bool parse_bool_text(const char *s, bool *out)
{
    if (strcmp(s, "true") == 0 || strcmp(s, "TRUE") == 0) { *out = true;  return true; }
    if (strcmp(s, "false") == 0 || strcmp(s, "FALSE") == 0) { *out = false; return true; }

    uint32_t v;
    if (!parse_u32(s, &v)) return false;
    *out = (v != 0);
    return true;
}

/** @brief 字符串能否按明文落盘 — 控制字符 (含 '\n') 会破坏 INI 行结构 */
static bool str_is_plain(const uint8_t *s, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++) {
        if (s[i] < 0x20u || s[i] == 0x7Fu) return false;
    }
    return true;
}

/** @brief 标签比较: tag 区间为 [tag, end) */
static bool tag_is(const char *tag, const char *end, const char *name)
{
    size_t n = strlen(name);
    return (size_t)(end - tag) == n && strncmp(tag, name, n) == 0;
}

/** @brief 把值类型的 4 字节位型回填到 data, 其余字节清零 (与保存侧布局同源) */
static void store_bits32(uint8_t *data, uint16_t len, uint32_t bits)
{
    memset(data, 0, len);
    memcpy(data, &bits, sizeof(bits));
}

/**
 * @brief 按条目类型把原始字节编码为标签 + 字面量
 *
 * @param e        参数条目 (类型的唯一来源); NULL = 按原始字节流处理
 * @param data     原始字节
 * @param len      字节数
 * @param tag      [out] 标签 (静态字符串, 不解引用 data)
 * @param lit      [out] 字面量缓冲区
 * @param lit_size 缓冲区容量
 * @return true 编码成功; false 无法文本化 (调用方退回 hex 兜底)
 */
static bool encode_literal(param_entry_t *e, const uint8_t *data, uint16_t len,
                           const char **tag, char *lit, size_t lit_size)
{
    if (!e || !data || len == 0) return false;

    switch (entry_type(e)) {
    case PARAM_TYPE_UINT: {
        if (len < 4) return false;
        uint32_t v;
        memcpy(&v, data, sizeof(v));
        snprintf(lit, lit_size, "%u", (unsigned)v);
        *tag = INI_TAG_UINT;
        return true;
    }
    case PARAM_TYPE_ENUM: {
        /* ENUM 的缓存按 int32 定义 (PARAM_ENTRY_ENUM 用 .i32, 校验也按
           enum_values[i] 的有符号比较), 与 param_dump 的 "ENUM=%ld" 对齐:
           负枚举值显示成 -1 而不是 4294967295 */
        if (len < 4) return false;
        int32_t v;
        memcpy(&v, data, sizeof(v));
        snprintf(lit, lit_size, "%d", (int)v);
        *tag = INI_TAG_ENUM;
        return true;
    }
    case PARAM_TYPE_INT: {
        if (len < 4) return false;
        int32_t v;
        memcpy(&v, data, sizeof(v));
        snprintf(lit, lit_size, "%d", (int)v);
        *tag = INI_TAG_INT;
        return true;
    }
    case PARAM_TYPE_FLOAT: {
        if (len < 4) return false;
        float v;
        memcpy(&v, data, sizeof(v));
        /* 回读自校验: 目标 libc 若裁掉 printf 浮点 (如 newlib-nano) 或精度
           不足以还原原值, 就退回 hex —— 宁可不可读, 不可写坏。 */
        snprintf(lit, lit_size, "%.9g", (double)v);
        char *end = NULL;
        float back = (float)strtod(lit, &end);
        uint32_t raw, got;
        memcpy(&raw, &v, sizeof(raw));
        memcpy(&got, &back, sizeof(got));
        if (end == lit || *end != '\0' || raw != got) return false;
        *tag = INI_TAG_FLOAT;
        return true;
    }
    case PARAM_TYPE_BOOL:
        /* bool 为 1 字节; param_value_t 联合体的其余字节是填充, 只取首字节。
           只有规范的 0/1 才写成 true/false: 越界字节 (raw 写入留下的脏值)
           退回 hex, 否则"存一次就把 2 改成 1", 违反无损落盘 */
        if (data[0] > 1) return false;
        snprintf(lit, lit_size, "%s", data[0] ? "true" : "false");
        *tag = INI_TAG_BOOL;
        return true;
    case PARAM_TYPE_STRING: {
        /* 字符串以首个 '\0' 结尾 (与 string_save/string_load 同语义);
           尾部零填充不必落盘, 读出侧会补零 */
        const uint8_t *nul = (const uint8_t *)memchr(data, '\0', len);
        uint16_t n = nul ? (uint16_t)(nul - data) : len;
        *tag = INI_TAG_STRING;

        if (str_is_plain(data, n) && (size_t)n + 3u <= lit_size) {
            lit[0] = '"';                    /* 明文: "内容" */
            memcpy(lit + 1, data, n);
            lit[n + 1] = '"';
            lit[n + 2] = '\0';
            return true;
        }

        /* 含控制字符 ('\n' 会破坏 INI 行结构) → 退化为 hex 字节流,
           仍算 string 类型: 只编码 内容+'\0' 这么长, 与明文形态同源 */
        uint16_t bytes = nul ? (uint16_t)(n + 1u) : n;
        if ((uint32_t)bytes * 2u + 3u > lit_size) return false;
        lit[0] = '0';
        lit[1] = 'x';
        hex_encode(data, bytes, lit + 2);
        return true;
    }
    case PARAM_TYPE_BLOB:
        if ((uint32_t)len * 2u + 1u > lit_size) return false;
        hex_encode(data, len, lit);
        *tag = INI_TAG_BLOB;
        return true;
    default:
        return false;  /* EXEC / 未知类型: 不落盘或按原始字节流 */
    }
}

/**
 * @brief 解码 "<name>:<tag>:<literal>" → 原始字节
 *
 * @param val     字典中的值文本
 * @param data    输出缓冲区
 * @param len     输出缓冲区容量 (= 条目数据区字节数; STRING 为 max_len+1)
 * @param out_len [out] 写出的字节数 (可 NULL)
 * @return PARAM_OK 成功; PARAM_ERR_OUT_OF_RANGE 格式损坏 / 超容量 / 标签不识;
 *         PARAM_ERR_INVALID_ID val 为 NULL
 */
static int decode_value(const char *val, uint8_t *data, uint16_t len, uint16_t *out_len)
{
    if (!val) return PARAM_ERR_INVALID_ID;

    const char *sep = strchr(val, ':');
    if (!sep || sep[1] == '\0') return PARAM_ERR_OUT_OF_RANGE;

    const char *body = sep + 1;             /* "<tag>:<literal>" */
    const char *tag_end = strchr(body, ':');
    if (!tag_end)                           /* 旧形态 "<name>:<hex>" */
        return hex_decode(body, data, len, out_len);

    const char *lit = tag_end + 1;

    if (tag_is(body, tag_end, INI_TAG_HEX) || tag_is(body, tag_end, INI_TAG_BLOB))
        return hex_decode(lit, data, len, out_len);

    if (tag_is(body, tag_end, INI_TAG_STRING)) {
        uint16_t n;
        if (lit[0] == '"') {
            /* 取最后一对引号: 字符串内容本身可以含 '"' */
            const char *close = strrchr(lit, '"');
            if (!close || close == lit) return PARAM_ERR_OUT_OF_RANGE;
            n = (uint16_t)(close - (lit + 1));
            if ((uint32_t)n + 1u > (uint32_t)len) return PARAM_ERR_OUT_OF_RANGE;
            memmove(data, lit + 1, n);
        } else if (lit[0] == '0' && (lit[1] == 'x' || lit[1] == 'X')) {
            /* 含控制字符时的退化形态: string:0x<hex> */
            return hex_decode(lit + 2, data, len, out_len);
        } else {
            /* 手写明文 (未加引号): 原样视作字符串内容 */
            n = (uint16_t)strlen(lit);
            if ((uint32_t)n + 1u > (uint32_t)len) return PARAM_ERR_OUT_OF_RANGE;
            memcpy(data, lit, n);
        }
        data[n] = '\0';                     /* 容量已扣除结尾 '\0' 的 1 字节 */
        if (out_len) *out_len = n;
        return PARAM_OK;
    }

    if (tag_is(body, tag_end, INI_TAG_UINT)) {
        uint32_t v;
        if (len < 4 || !parse_u32(lit, &v)) return PARAM_ERR_OUT_OF_RANGE;
        store_bits32(data, len, v);
        if (out_len) *out_len = 4;
        return PARAM_OK;
    }
    if (tag_is(body, tag_end, INI_TAG_ENUM)) {
        /* 按有符号解析 (与保存侧一致), 同时容忍手写的无符号/0x 形式 */
        if (len < 4) return PARAM_ERR_OUT_OF_RANGE;
        uint32_t bits;
        int32_t si;
        if (parse_i32(lit, &si))
            memcpy(&bits, &si, sizeof(bits));
        else if (!parse_u32(lit, &bits))
            return PARAM_ERR_OUT_OF_RANGE;
        store_bits32(data, len, bits);
        if (out_len) *out_len = 4;
        return PARAM_OK;
    }
    if (tag_is(body, tag_end, INI_TAG_INT)) {
        int32_t v;
        if (len < 4 || !parse_i32(lit, &v)) return PARAM_ERR_OUT_OF_RANGE;
        uint32_t bits;
        memcpy(&bits, &v, sizeof(bits));
        store_bits32(data, len, bits);
        if (out_len) *out_len = 4;
        return PARAM_OK;
    }
    if (tag_is(body, tag_end, INI_TAG_FLOAT)) {
        if (len < 4) return PARAM_ERR_OUT_OF_RANGE;
        char *end = NULL;
        double d = strtod(lit, &end);
        if (end == lit || *end != '\0') return PARAM_ERR_OUT_OF_RANGE;
        float v = (float)d;
        uint32_t bits;
        memcpy(&bits, &v, sizeof(bits));
        store_bits32(data, len, bits);
        if (out_len) *out_len = 4;
        return PARAM_OK;
    }
    if (tag_is(body, tag_end, INI_TAG_BOOL)) {
        bool b;
        if (!parse_bool_text(lit, &b)) return PARAM_ERR_OUT_OF_RANGE;
        memset(data, 0, len);
        data[0] = b ? 1u : 0u;
        if (out_len) *out_len = 1;
        return PARAM_OK;
    }

    return PARAM_ERR_OUT_OF_RANGE;          /* 未知标签: 视为记录损坏 */
}

/* ================================================================
 *  文件工具
 * ================================================================ */

/**
 * @brief 确保目录存在 (递归 mkdir -p)
 */
static int mkdir_p(const char *path)
{
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

/**
 * @brief 原子写入文件: 写临时文件 → rename
 *
 * @param path  目标文件路径
 * @param data  数据缓冲区
 * @param len   数据长度
 * @return 0 成功, -1 失败
 */
static int atomic_write(const char *path, const uint8_t *data, uint16_t len)
{
    char tmp[288];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    /* O_EXCL 防止多进程竞态写入同一临时文件 */
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_TRUNC, 0644);
    if (fd < 0) return -1;

    ssize_t written = write(fd, data, len);
    close(fd);

    if (written != len) {
        unlink(tmp);
        return -1;
    }

    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }

    return 0;
}

/**
 * @brief 原子写入 INI 文件: iniparser_dump_ini → 临时文件 → rename
 */
static int atomic_dump_ini(iniparser_ctx_t *c)
{
    char tmp[288];
    snprintf(tmp, sizeof(tmp), "%s.tmp", c->ini_path);

    FILE *f = fopen(tmp, "w");
    if (!f) return -1;

    iniparser_dump_ini(c->dict, f);
    fclose(f);

    if (rename(tmp, c->ini_path) != 0) {
        unlink(tmp);
        return -1;
    }

    return 0;
}

/* ================================================================
 *  键名 / 值构建
 * ================================================================ */

/**
 * @brief 构建键名 "params:0x<8 位 hex param_id>"
 *
 * param_id 的完整 32 位形式: 高 16 位 module_id | 低 16 位 local_id。
 * 不使用 "p" 前缀: "0x" 已足以区分命名空间, 同时便于人工对照 module_ids.h。
 * 键大小写不敏感 —— iniparser 内部统一转小写, 落盘形如 "params:0x00020000"。
 */
static void make_key(uint32_t param_id, char *key, size_t key_size)
{
    snprintf(key, key_size, INI_SECTION ":0x%08X", param_id);
}

/**
 * @brief 构建值 "<参数名>:<类型标签>:<字面量>" — 参数名仅作可读标注, 不参与身份
 *
 * @note 名称为空或条目未注册时用 "?" 占位 (PARAM_DEBUG_NAME_ENABLE=0 时
 *       param_entry_name 本身就返回 "?")。调试名是 C 变量名, 只含
 *       [A-Za-z0-9_], 不会与分隔符 ':' 冲突。
 */
static void make_value(uint32_t param_id, const char *tag, const char *lit,
                       char *out, size_t out_size)
{
    const param_entry_t *e = param_entry_find(param_id);
    const char *src = "?";

    if (e) {
        const char *nm = param_entry_name(e);
        if (nm && nm[0])
            src = nm;
    }

    /* 先独立截断名称: 直接把长名称拼进 out 会挤掉值部分, 静默损坏值。
       截断后总长上限 = (INI_NAME_MAX-1) + 1 + (INI_TAG_MAX-1) + 1
                      + strlen(lit) + 1, 恒 < INI_VAL_MAX (lit 恒短于 HEX_VAL_MAX) */
    char name[INI_NAME_MAX];
    snprintf(name, sizeof(name), "%s", src);

    snprintf(out, out_size, "%s:%s:%s", name, tag, lit);
}

/* ================================================================
 *  基础操作: 加载/保存字典
 * ================================================================ */

/**
 * @brief 加载 INI 文件到字典 (若文件不存在则创建空字典)
 */
static int load_dict(iniparser_ctx_t *c)
{
    if (c->dict) {
        iniparser_freedict(c->dict);
        c->dict = NULL;
    }

    c->dict = iniparser_load(c->ini_path);
    if (!c->dict) {
        /* 文件不存在或解析失败 → 空字典 */
        c->dict = dictionary_new(0);
        if (!c->dict) return -1;
    }

    /* 确保 [params] 段存在: iniparser_dump_ini 按段组织输出需要段键 */
    if (!iniparser_find_entry(c->dict, INI_SECTION)) {
        iniparser_set(c->dict, INI_SECTION, NULL);
    }

    c->dict_ready = true;
    return 0;
}

/**
 * @brief 检查字典是否为空 (无任何键值对)
 */
static bool dict_is_empty(iniparser_ctx_t *c)
{
    if (!c || !c->dict_ready || !c->dict) return true;

    /* 字典中仅存在 section 键 (无实际参数键) 视为空 */
    int nsec = iniparser_getnsec(c->dict);
    if (nsec < 0) return true;
    return ((int)c->dict->n <= nsec);
}

/* ================================================================
 *  存储驱动回调实现
 * ================================================================ */

/**
 * @brief 加载: 从字典中读取 hex 值并解码为字节数组
 *
 * @return PARAM_OK 成功; PARAM_ERR_NOT_FOUND 键不存在;
 *         PARAM_ERR_OUT_OF_RANGE 存储值损坏/超出条目容量; PARAM_ERR_INVALID_ID 参数非法
 */
static int iniparser_load_cb(void *ctx, uint32_t param_id,
                              uint8_t *data, uint16_t len, uint16_t *read_len)
{
    iniparser_ctx_t *c = (iniparser_ctx_t *)ctx;
    if (!c || !c->dict_ready || !c->dict || !data || len == 0)
        return PARAM_ERR_INVALID_ID;

    char key[INI_KEY_MAX];
    make_key(param_id, key, sizeof(key));

    const char *val = iniparser_getstring(c->dict, key, NULL);
    if (!val) return PARAM_ERR_NOT_FOUND;

    /* 记录存在即代表"有值": 交由值文本解码器还原字节流 (新旧格式统一入口)。
       损坏/超长记录上报明确的错误码, 不冒充"键不存在" */
    return decode_value(val, data, len, read_len);
}

/**
 * @brief 保存: 原始字节 → 类型化文本 → 更新字典 → 全量重写 INI 文件
 *
 * @return 0 成功, -1 失败
 */
static int iniparser_save_cb(void *ctx, uint32_t param_id,
                              const uint8_t *data, uint16_t len)
{
    iniparser_ctx_t *c = (iniparser_ctx_t *)ctx;
    if (!c || !c->dict_ready || !c->dict || !data || len == 0)
        return -1;

    /* 1. 按条目类型选落盘形态 (uint/int/float/bool/enum/string/blob);
          类型未知 / 长度不足 / 超长 / EXEC 等无法文本化的情况退回 hex
          原始字节流, 保证任何字节都能存下来 */
    char lit[HEX_VAL_MAX];
    const char *tag = INI_TAG_HEX;

    if (!encode_literal(param_entry_find(param_id), data, len, &tag, lit, sizeof(lit))) {
        /* hex 兜底也要检查编码后是否超出缓冲区 */
        if ((uint32_t)len * 2u + 1u > HEX_VAL_MAX)
            return -1;
        hex_encode(data, len, lit);
        tag = INI_TAG_HEX;
    }

    /* 2. 组装成 "<参数名>:<tag>:<字面量>" (名称仅供人读) */
    char val[INI_VAL_MAX];
    make_value(param_id, tag, lit, val, sizeof(val));

    /* 3. 更新字典 */
    char key[INI_KEY_MAX];
    make_key(param_id, key, sizeof(key));
    if (iniparser_set(c->dict, key, val) != 0)
        return -1;

    /* 4. 全量写入 INI 文件 (原子); 批量模式 (defer_dump) 下仅改字典, 由调用方落盘一次 */
    return c->defer_dump ? 0 : atomic_dump_ini(c);
}

/**
 * @brief 批量保存事务开始 — 打开"只改字典不落盘"的批量模式
 *
 * 框架随后逐条调用 save (各条目的 PERSIST 过滤由 param_data_ops 层完成),
 * 最后调用 iniparser_save_commit_cb 一次性落盘。本后端不需要、也不得
 * 自己遍历框架的条目 —— 批量语义只由本后端的 defer_dump 表达。
 *
 * @return 0 成功; -1 字典未就绪
 */
static int iniparser_save_begin_cb(void *ctx)
{
    iniparser_ctx_t *c = (iniparser_ctx_t *)ctx;
    if (!c || !c->dict_ready || !c->dict)
        return -1;

    c->defer_dump = true;
    return 0;
}

/**
 * @brief 批量保存事务提交 — 一次性落盘并关闭批量模式
 *
 * 批量模式下逐条 save 只改字典, 由本回调一次性落盘 —— 否则每条 PERSIST
 * 参数都要全量重写一次文件, 开销随参数数线性叠加。
 *
 * @note 框架在 SYS_LOCK_PARAM 之外调用本回调, 文件 I/O 不阻塞参数访问。
 * @note 先关批量模式再落盘: 即使落盘失败, 后端也不会滞留在 defer_dump 状态
 *       (否则后续 param_save_one 会只改字典、永不落盘)。
 * @return 0 成功; -1 字典未就绪或写入失败
 */
static int iniparser_save_commit_cb(void *ctx)
{
    iniparser_ctx_t *c = (iniparser_ctx_t *)ctx;
    if (!c || !c->dict_ready || !c->dict)
        return -1;

    c->defer_dump = false;
    return atomic_dump_ini(c);
}

/**
 * @brief 删除单个参数: 从字典移除 → 重写 INI
 */
static int iniparser_delete_cb(void *ctx, uint32_t param_id)
{
    iniparser_ctx_t *c = (iniparser_ctx_t *)ctx;
    if (!c || !c->dict_ready || !c->dict) return -1;

    char key[INI_KEY_MAX];
    make_key(param_id, key, sizeof(key));
    iniparser_unset(c->dict, key);

    return atomic_dump_ini(c);
}

/**
 * @brief 擦除全部: 释放字典 → 重建空字典 → 写空 INI
 */
static int iniparser_erase_all_cb(void *ctx)
{
    iniparser_ctx_t *c = (iniparser_ctx_t *)ctx;
    if (!c || !c->dict_ready || !c->dict) return -1;

    iniparser_freedict(c->dict);
    c->dict = dictionary_new(0);
    if (!c->dict) return -1;

    /* 确保 [params] 段存在 */
    iniparser_set(c->dict, INI_SECTION, NULL);

    return atomic_dump_ini(c);
}

/**
 * @brief 去初始化: 释放字典
 */
static int iniparser_deinit_cb(void *ctx)
{
    iniparser_ctx_t *c = (iniparser_ctx_t *)ctx;
    if (!c) return -1;

    if (c->dict) {
        iniparser_freedict(c->dict);
        c->dict = NULL;
    }
    c->dict_ready = false;
    return 0;
}

/* ================================================================
 *  分区管理 (A/B 切换)
 * ================================================================ */

/**
 * @brief 读取启动分区索引 (从 <base_dir>/param_boot 文件)
 *
 * @param base_dir 持久化根目录
 * @param index    [out] 分区索引
 * @return 0 成功, -1 失败 (文件不存在/格式错误等)
 */
static int read_boot_index(const char *base_dir, uint8_t *index)
{
    if (!base_dir || !index) return -1;

    char boot_path[288];
    snprintf(boot_path, sizeof(boot_path), "%s/%s", base_dir, BOOT_FILE);

    int fd = open(boot_path, O_RDONLY);
    if (fd < 0) return -1;

    char buf[BOOT_VAL_MAX];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (n <= 0) return -1;
    buf[n] = '\0';

    char *endptr = NULL;
    long val = strtol(buf, &endptr, 10);
    /* 拒绝非数字内容或残留字符 */
    if (endptr == buf || *endptr != '\0')
        return -1;
    if (val < 0 || val >= PARAM_PARTITION_COUNT)
        return -1;

    *index = (uint8_t)val;
    return 0;
}

/**
 * @brief 构建 param_boot 文件路径
 */
static void make_boot_path(iniparser_ctx_t *c, char *path, size_t size)
{
    snprintf(path, size, "%s/%s", c->base_dir, BOOT_FILE);
}

/**
 * @brief 读取启动分区索引 (从 param_boot 文件)
 *
 * @return 0 成功, -1 失败 (文件不存在等)
 */
static int iniparser_get_active_partition_cb(void *ctx, uint8_t *index)
{
    iniparser_ctx_t *c = (iniparser_ctx_t *)ctx;
    if (!c || !index) return -1;
    return read_boot_index(c->base_dir, index);
}

/**
 * @brief 写入启动分区索引 (下次启动生效)
 */
static int iniparser_set_active_partition_cb(void *ctx, uint8_t index)
{
    iniparser_ctx_t *c = (iniparser_ctx_t *)ctx;
    if (!c) return -1;

    char boot_path[288];
    make_boot_path(c, boot_path, sizeof(boot_path));

    char buf[BOOT_VAL_MAX];
    int len = snprintf(buf, sizeof(buf), "%u", index);

    return atomic_write(boot_path, (const uint8_t *)buf, (uint16_t)len);
}

/**
 * @brief 按索引获取分区驱动 — 单例语义
 *
 * 委托 param_storage_iniparser_get_driver 创建/获取对应分区驱动。
 */
static const param_storage_drv_t *iniparser_get_partition_cb(void *ctx, uint8_t index)
{
    iniparser_ctx_t *c = (iniparser_ctx_t *)ctx;
    if (!c) return NULL;

    return param_storage_iniparser_get_driver(c->base_dir, param_partition_name(index));
}

/* ================================================================
 *  工厂方法
 * ================================================================ */

/**
 * @brief 创建默认存储驱动（启动时调用，智能分区选择）
 *
 * @details
 * 启动流程:
 *   1. 确保 base_dir 存在 (mkdir -p)
 *   2. 读取 param_boot 获取启动分区索引
 *   3. 初始化目标分区 INI 文件
 *   4. 空分区检测: 字典为空且 boot_index 在用户分区范围 → 回退 factory
 *   5. 写入 boot_index=PARAM_PARTITION_FACTORY (回退时)
 *
 * @param base_dir 持久化目录路径 (如 "/data/params")
 * @return 已初始化的存储驱动 (最坏返回 factory；base_dir 为 NULL 或池耗尽返回 NULL)
 */
const param_storage_drv_t *param_storage_iniparser_create(const char *base_dir)
{
    if (!base_dir) return NULL;

    /* 阶段 1: 确保目录存在 */
    mkdir_p(base_dir);

    /* 阶段 2: 读取启动分区索引 */
    uint8_t boot_index = PARAM_PARTITION_FACTORY;
    (void)read_boot_index(base_dir, &boot_index);

    /* 阶段 3: 索引 → 分区名 (越界/未填充自动回退 factory) */
    const char *target = param_partition_name(boot_index);

    /* 阶段 4: 初始化目标分区，空则回退 factory */
    const param_storage_drv_t *drv =
        param_storage_iniparser_get_driver(base_dir, target);
    if (!drv) {
        drv = param_storage_iniparser_get_driver(
            base_dir, param_partition_name(PARAM_PARTITION_FACTORY));
        return drv;
    }

    iniparser_ctx_t *c = (iniparser_ctx_t *)drv->ctx;
    if (boot_index >= PARAM_PARTITION_USER_MIN && boot_index <= PARAM_PARTITION_USER_MAX
        && dict_is_empty(c)) {
        /* 用户分区为空 → 回退 factory */
        drv->deinit(drv->ctx);
        drv = param_storage_iniparser_get_driver(
            base_dir, param_partition_name(PARAM_PARTITION_FACTORY));
        /* 写入 boot_index=factory */
        char buf[BOOT_VAL_MAX];
        int len = snprintf(buf, sizeof(buf), "%u", PARAM_PARTITION_FACTORY);
        char bp[288];
        snprintf(bp, sizeof(bp), "%s/%s", base_dir, BOOT_FILE);
        atomic_write(bp, (const uint8_t *)buf, (uint16_t)len);
    }

    return drv;
}

/**
 * @brief 获取 INI 持久化后端驱动（工厂模式，按分区名创建独立实例）
 *
 * @details
 * "查找→创建"两阶段工厂模式:
 *   1. 查找已有: 遍历 g_ctx[] 查找 part_name 匹配的槽位
 *   2. 创建新实例: 分配空闲槽位，构建 INI 路径，加载字典
 *   3. 池耗尽: 返回 NULL
 *
 * @param base_dir  持久化根目录 (如 "/data/params")
 * @param part_name 分区名 (如 "param_user0")
 * @return 驱动句柄（静态分配），同一分区名返回同一实例
 */
const param_storage_drv_t *param_storage_iniparser_get_driver(const char *base_dir,
                                                               const char *part_name)
{
    if (!base_dir || !part_name) return NULL;

    /* 阶段 1: 查找已有实例 */
    for (int i = 0; i < MAX_INSTANCES; i++) {
        if (g_ctx[i].used &&
            strncmp(g_ctx[i].part_name, part_name, sizeof(g_ctx[i].part_name)) == 0 &&
            strncmp(g_ctx[i].base_dir, base_dir, sizeof(g_ctx[i].base_dir)) == 0) {
            /* 惰性重加载: 条件为 dict_ready 为假 (实例被 deinit 后即如此) */
            if (!g_ctx[i].dict_ready)
                load_dict(&g_ctx[i]);
            return &g_ctx[i].drv;
        }
    }

    /* 阶段 2: 分配新槽位并初始化 */
    for (int i = 0; i < MAX_INSTANCES; i++) {
        if (!g_ctx[i].used) {
            iniparser_ctx_t *c = &g_ctx[i];
            memset(c, 0, sizeof(*c));

            strncpy(c->base_dir, base_dir, sizeof(c->base_dir) - 1);
            c->base_dir[sizeof(c->base_dir) - 1] = '\0';
            strncpy(c->part_name, part_name, sizeof(c->part_name) - 1);
            c->part_name[sizeof(c->part_name) - 1] = '\0';
            snprintf(c->ini_path, sizeof(c->ini_path),
                     "%s/%s.ini", c->base_dir, c->part_name);

            /* 确保目录存在 */
            mkdir_p(c->base_dir);

            /* 绑定存储驱动回调 */
            c->drv.ctx = c;
            c->drv.load = iniparser_load_cb;
            c->drv.save = iniparser_save_cb;
            c->drv.save_begin = iniparser_save_begin_cb;
            c->drv.save_commit = iniparser_save_commit_cb;
            c->drv.delete = iniparser_delete_cb;
            c->drv.erase_all = iniparser_erase_all_cb;
            c->drv.deinit = iniparser_deinit_cb;
            c->drv.get_active_partition = iniparser_get_active_partition_cb;
            c->drv.set_active_partition = iniparser_set_active_partition_cb;
            c->drv.get_partition = iniparser_get_partition_cb;
            c->used = true;

            /* 加载 INI 文件到字典 */
            if (load_dict(c) != 0) {
                c->used = false;
                return NULL;
            }

            return &c->drv;
        }
    }

    /* 阶段 3: 池耗尽 */
    return NULL;
}