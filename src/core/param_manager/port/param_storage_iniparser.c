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
 * 每个参数以 hex 字符串存储在 [params] 段下:
 *   p<param_id> = <hex bytes>
 *
 * 每次 save() 全量重写 INI 文件，通过写临时文件 + rename 保证原子性。
 *
 * @attention 全局实例池 g_ctx[] 无锁保护，仅支持单线程使用。
 *
 * @see param_storage_drv_t 接口定义
 * @see param_storage_iniparser.h 公开 API
 */

#include "param_storage_iniparser.h"

#if !USE_INIPARSER

/* ================================================================
 *  USE_INIPARSER=0: 空操作 stub
 * ================================================================ */

static int stub_load(void *ctx, uint32_t id, uint8_t *d, uint16_t l)
{ (void)ctx; (void)id; (void)d; (void)l; return -1; }
static int stub_save(void *ctx, uint32_t id, const uint8_t *d, uint16_t l)
{ (void)ctx; (void)id; (void)d; (void)l; return -1; }
static int stub_erase_all(void *ctx) { (void)ctx; return 0; }
static int stub_delete(void *ctx, uint32_t id) { (void)ctx; (void)id; return 0; }
static int stub_deinit(void *ctx) { (void)ctx; return 0; }
static int stub_get_active_partition(void *ctx, uint8_t *idx)
{ (void)ctx; (void)idx; return -1; }
static int stub_set_active_partition(void *ctx, uint8_t idx)
{ (void)ctx; (void)idx; return -1; }
static const param_storage_drv_t *stub_get_partition(void *ctx, uint8_t idx)
{ (void)ctx; (void)idx; return NULL; }

static param_storage_drv_t g_stub_drv = {
    .ctx = NULL,
    .load = stub_load, .save = stub_save,
    .delete = stub_delete, .erase_all = stub_erase_all, .deinit = stub_deinit,
    .get_active_partition = stub_get_active_partition,
    .set_active_partition = stub_set_active_partition,
    .get_partition = stub_get_partition,
};

const param_storage_drv_t *param_storage_iniparser_create(const char *base_dir)
{ (void)base_dir; return &g_stub_drv; }

const param_storage_drv_t *param_storage_iniparser_get_driver(const char *base_dir,
                                                               const char *part_name)
{ (void)base_dir; (void)part_name; return &g_stub_drv; }

#else

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

/** @brief 键名缓冲区大小 ("params:p" + id 十进制 + '\0') */
#define INI_KEY_MAX 48

/** @brief hex 值缓冲区大小 (支持 ≤255 字节的原始数据: 255*2+1=511)
 *
 * 此限制决定单个参数持久化的最大字节数:
 *   - FLOAT / UINT / INT / BOOL / ENUM / EXEC  → sizeof(param_value_t) = 4~8 字节, 完全不受限
 *   - STRING → max_len+1 字节, 受限于此值的一半 (≤255 字节)
 *   - BLOB   → blob_size 字节, 受限于此值的一半 (≤255 字节)
 *
 * 如需支持更大的 BLOB/STRING，将此值改为 2*N+1 (N 为目标最大字节数)
 * 或改用动态分配。 */
#define HEX_VAL_MAX 512

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
    param_storage_drv_t drv;  /**< 对外暴露的存储驱动接口 */
} iniparser_ctx_t;

/** @brief 全局存储实例池（静态分配） */
static iniparser_ctx_t g_ctx[MAX_INSTANCES];

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(MAX_INSTANCES >= PARAM_PARTITION_COUNT,
               "MAX_INSTANCES must be >= PARAM_PARTITION_COUNT");
#endif

/**
 * @brief 分区索引→分区名映射表 (与 flashdb 后端一致)
 *
 * 索引: 0=factory, 1~PARAM_PARTITION_USER_MAX=用户分区。
 */
static const char *const g_partition_names[PARAM_PARTITION_COUNT] = {
    [PARAM_PARTITION_FACTORY] = "param_factory",
    [1] = "param_user0",
    [2] = "param_user1",
    [3] = "param_user2",
    [4] = "param_user3",
};

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
 * @return 解码的字节数 (>0), -1 格式错误
 */
static int hex_decode(const char *hex, uint8_t *data, uint16_t max_len)
{
    if (!hex) return -1;
    uint16_t hex_len = (uint16_t)strlen(hex);
    if (hex_len % 2 != 0) return -1;
    uint16_t byte_len = hex_len / 2;
    /* 拒绝截断: 存储值超过缓冲区容量视为数据损坏 */
    if (byte_len > max_len) return -1;
    for (uint16_t i = 0; i < byte_len; i++) {
        int hi = hex_nibble(hex[i * 2]);
        int lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        data[i] = (uint8_t)((hi << 4) | lo);
    }
    return (int)byte_len;
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
 *  键名构建
 * ================================================================ */

/**
 * @brief 构建 iniparser 键名 "params:p<id>"
 */
static void make_key(uint32_t param_id, char *key, size_t key_size)
{
    snprintf(key, key_size, INI_SECTION ":p%u", param_id);
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
 * @return 实际读取字节数 (>0), -1 键不存在或解码失败
 */
static int iniparser_load_cb(void *ctx, uint32_t param_id,
                              uint8_t *data, uint16_t len)
{
    iniparser_ctx_t *c = (iniparser_ctx_t *)ctx;
    if (!c || !c->dict_ready || !c->dict || !data || len == 0)
        return -1;

    char key[INI_KEY_MAX];
    make_key(param_id, key, sizeof(key));

    const char *hex = iniparser_getstring(c->dict, key, NULL);
    if (!hex || hex[0] == '\0') return -1;

    return hex_decode(hex, data, len);
}

/**
 * @brief 保存: hex 编码 → 更新字典 → 全量重写 INI 文件
 *
 * @return 0 成功, -1 失败
 */
static int iniparser_save_cb(void *ctx, uint32_t param_id,
                              const uint8_t *data, uint16_t len)
{
    iniparser_ctx_t *c = (iniparser_ctx_t *)ctx;
    if (!c || !c->dict_ready || !c->dict || !data || len == 0)
        return -1;

    /* 检查 hex 编码后是否超出缓冲区 */
    if (((uint32_t)len * 2 + 1) > HEX_VAL_MAX)
        return -1;

    /* 1. hex 编码 */
    char hex_val[HEX_VAL_MAX];
    hex_encode(data, len, hex_val);

    /* 2. 更新字典 */
    char key[INI_KEY_MAX];
    make_key(param_id, key, sizeof(key));
    if (iniparser_set(c->dict, key, hex_val) != 0)
        return -1;

    /* 3. 全量写入 INI 文件 (原子) */
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

    const char *name;
    if (index < PARAM_PARTITION_COUNT && g_partition_names[index])
        name = g_partition_names[index];
    else
        name = g_partition_names[PARAM_PARTITION_FACTORY];

    return param_storage_iniparser_get_driver(c->base_dir, name);
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

    /* 阶段 3: 映射索引到分区名 */
    const char *target;
    if (boot_index >= PARAM_PARTITION_USER_MIN && boot_index <= PARAM_PARTITION_USER_MAX)
        target = g_partition_names[boot_index];
    else
        target = g_partition_names[PARAM_PARTITION_FACTORY];

    /* 阶段 4: 初始化目标分区，空则回退 factory */
    const param_storage_drv_t *drv =
        param_storage_iniparser_get_driver(base_dir, target);
    if (!drv) {
        drv = param_storage_iniparser_get_driver(base_dir,
                                                  g_partition_names[PARAM_PARTITION_FACTORY]);
        return drv;
    }

    iniparser_ctx_t *c = (iniparser_ctx_t *)drv->ctx;
    if (boot_index >= PARAM_PARTITION_USER_MIN && boot_index <= PARAM_PARTITION_USER_MAX
        && dict_is_empty(c)) {
        /* 用户分区为空 → 回退 factory */
        drv->deinit(drv->ctx);
        drv = param_storage_iniparser_get_driver(base_dir,
                                                  g_partition_names[PARAM_PARTITION_FACTORY]);
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
            /* 惰性重加载: 若字典曾被释放则重新加载 */
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

            /* 绑定全部 8 个存储驱动回调 */
            c->drv.ctx = c;
            c->drv.load = iniparser_load_cb;
            c->drv.save = iniparser_save_cb;
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

#endif /* USE_INIPARSER */