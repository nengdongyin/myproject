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
 * 值由"参数名"与"hex 字节流"组成 (参数名仅作可读标注, 不参与身份):
 *   0x00020000 = g_boot_index:03000000
 * 高 16 位 = module_id, 低 16 位 = local_id, 便于对照 module_ids.h。
 * 键与值格式均为本后端第一版定义, 无历史格式包袱。
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
/* 值是"名称:hex"形式, 名称经 param_entry_name() 取自条目本身 (调试名),
   这是本后端对框架内部 API 的唯一依赖 (纯装饰用途, 不参与身份)。
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

/** @brief 完整值缓冲区大小: "<name>:<hex>" (名称 + ':' + hex 部分) */
#define INI_VAL_MAX (INI_NAME_MAX + 1 + HEX_VAL_MAX)

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
 * @brief 构建值 "<参数名>:<hex>" — 参数名仅作可读标注, 不参与身份
 *
 * @note 名称为空或条目未注册时用 "?" 占位 (PARAM_DEBUG_NAME_ENABLE=0 时
 *       param_entry_name 本身就返回 "?")。调试名是 C 变量名, 只含
 *       [A-Za-z0-9_], 不会与分隔符 ':' 冲突。
 */
static void make_value(uint32_t param_id, const char *hex, char *out, size_t out_size)
{
    const param_entry_t *e = param_entry_find(param_id);
    const char *src = "?";

    if (e) {
        const char *nm = param_entry_name(e);
        if (nm && nm[0])
            src = nm;
    }

    /* 先独立截断名称: 直接把长名称拼进 out 会挤掉 hex 部分, 静默损坏值。
       截断后总长上限 = (INI_NAME_MAX-1) + 1 + strlen(hex) + 1, 恒 < INI_VAL_MAX */
    char name[INI_NAME_MAX];
    snprintf(name, sizeof(name), "%s", src);

    snprintf(out, out_size, "%s:%s", name, hex);
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

    /* 值格式固定为 "<参数名>:<hex>"; 缺分隔符或 hex 为空均属记录损坏 */
    const char *sep = strchr(val, ':');
    if (!sep || sep[1] == '\0') return PARAM_ERR_OUT_OF_RANGE;

    /* 损坏/超长记录上报明确的错误码, 不冒充"键不存在" */
    return hex_decode(sep + 1, data, len, read_len);
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

    /* 1. hex 编码, 并组装成 "<参数名>:<hex>" (名称仅供人读) */
    char hex_val[HEX_VAL_MAX];
    hex_encode(data, len, hex_val);

    char val[INI_VAL_MAX];
    make_value(param_id, hex_val, val, sizeof(val));

    /* 2. 更新字典 */
    char key[INI_KEY_MAX];
    make_key(param_id, key, sizeof(key));
    if (iniparser_set(c->dict, key, val) != 0)
        return -1;

    /* 3. 全量写入 INI 文件 (原子); 批量模式 (defer_dump) 下仅改字典, 由调用方落盘一次 */
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