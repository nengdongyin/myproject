/**
 * @file param_storage_flashdb.c
 * @brief 基于 FlashDB 的持久化存储后端实现
 *
 * @details
 * 本文件实现 param_storage_drv_t 接口的 FlashDB 适配层。
 * 支持两种编译模式:
 *   - USE_FLASHDB=1: 完整 FlashDB 后端 — 多实例工厂、分区选择、
 *     启动分区回退、空分区检测、KV 读写擦除
 *   - USE_FLASHDB=0: 空操作 stub — 所有操作返回 -1，用于编译验证
 *
 * FlashDB 模式下采用工厂模式管理多存储实例:
 *   - g_ctx[MAX_INSTANCES] 静态池，按分区名查找/创建
 *   - 同一分区名返回同一实例（幂等）
 *   - param_storage_flashdb_create() 实现智能启动分区选择:
 *     读取 param_boot 裸分区 → 选择目标用户分区 → 空则回退 factory
 *
 * @see param_storage_drv_t 接口定义
 * @see param_storage_flashdb.h 公开 API
 */

#include "param_storage_flashdb.h"
#include <stdio.h>
#include <string.h>

#if !USE_FLASHDB

/* ================================================================
 *  USE_FLASHDB=0: 空操作 stub — 所有持久化操作返回 -1
 * ================================================================ */

/** @brief stub: 加载操作始终返回"不存在" */
static int stub_load(void *ctx, uint32_t id, uint8_t *d, uint16_t l)
{
    (void)ctx;
    (void)id;
    (void)d;
    (void)l;
    return -1;
}
/** @brief stub: 保存操作始终返回 -1 */
static int stub_save(void *ctx, uint32_t id, const uint8_t *d, uint16_t l)
{
    (void)ctx;
    (void)id;
    (void)d;
    (void)l;
    return -1;
}
/** @brief stub: 擦除全部 — 无操作 */
static int stub_erase_all(void *ctx) { (void)ctx; return 0; }
/** @brief stub: 删除单个 — 无操作 */
static int stub_delete(void *ctx, uint32_t id) { (void)ctx; (void)id; return 0; }
/** @brief stub: 去初始化 — 无操作 */
static int stub_deinit(void *ctx)
{
    (void)ctx;
    return 0;
}

/** @brief stub 模式下的全局驱动实例（所有分区返回同一实例） */
static param_storage_drv_t g_stub_drv = {
    .ctx = NULL,
    .load = stub_load,
    .save = stub_save,
    .delete = stub_delete,
    .erase_all = stub_erase_all,
    .deinit = stub_deinit,
};

/**
 * @brief stub 模式: 忽略分区名，返回全局空操作驱动
 *
 * @param part_name 分区名（忽略）
 * @return 全局空操作驱动
 */
const param_storage_drv_t *param_storage_flashdb_get_driver(const char *part_name)
{
    (void)part_name;
    return &g_stub_drv;
}

#else

/* ================================================================
 *  USE_FLASHDB=1: 完整 FlashDB 后端实现
 * ================================================================ */

#include "flashdb.h"

/**
 * @brief FlashDB 存储上下文 — 每个物理分区对应一个实例
 *
 * 实例通过 g_ctx[MAX_INSTANCES] 静态池管理，按 part_name 查找/创建。
 * 同一分区名多次请求返回同一实例（工厂模式 + 幂等）。
 */
typedef struct
{
    bool used;               /**< 此槽位是否已被分配 */
    struct fdb_kvdb kvdb;    /**< FlashDB KVDB 句柄 */
    bool kvdb_ready;         /**< KVDB 是否已初始化完成 */
    char part_name[24];      /**< FAL 分区名（如 "param_user0"） */
    param_storage_drv_t drv; /**< 对外暴露的存储驱动接口 */
} flashdb_ctx_t;

/** @brief 全局存储实例池（静态分配，编译期确定上限） */
static flashdb_ctx_t g_ctx[MAX_INSTANCES];
/** @brief FAL (Flash Abstraction Layer) 是否已初始化 */
static bool g_fal_inited = false;

/**
 * @brief 初始化 FlashDB KVDB 实例
 *
 * 设置扇区大小，调用 fdb_kvdb_init 初始化指定分区的 KV 数据库。
 *
 * @param c FlashDB 上下文指针
 * @return 0 成功, -1 初始化失败
 */
static int flashdb_init_kvdb(flashdb_ctx_t *c)
{
    uint32_t sec_size = FDB_SECTOR_SIZE;
    fdb_kvdb_control(&c->kvdb, FDB_KVDB_CTRL_SET_SEC_SIZE, &sec_size);

    fdb_err_t result = fdb_kvdb_init(&c->kvdb, FDB_KVDB_NAME,
                                     c->part_name, NULL, NULL);
    if (result != FDB_NO_ERR)
        return -1;

    c->kvdb_ready = true;
    return 0;
}

/**
 * @brief 检测 KVDB 分区是否为空（无任何键值对）
 *
 * 通过迭代器探测分区内是否有数据。
 * 用于启动时的空分区检测 → 回退到 factory 分区。
 *
 * @param c FlashDB 上下文指针
 * @return true 分区为空或未初始化，false 分区有数据
 */
static bool kvdb_is_empty(flashdb_ctx_t *c)
{
    if (!c || !c->kvdb_ready)
        return true;

    struct fdb_kv_iterator itr;
    fdb_kv_iterator_init(&c->kvdb, &itr);
    return !fdb_kv_iterate(&c->kvdb, &itr);
}

/**
 * @brief 读取启动分区索引（从 "param_boot" 裸分区）
 *
 * @param ctx   存储上下文（本实现忽略）
 * @param index [out] 启动索引 (0~3=用户分区, 4=factory, 0xFF=无效)
 * @return 0 成功, -1 失败
 */
static int flashdb_get_active_partition(void *ctx, uint8_t *index)
{
    (void)ctx;
    if (!index)
        return -1;

    const struct fal_partition *part = fal_partition_find("param_boot");
    if (!part)
        return -1;

    uint8_t val = 0xFF;
    if (fal_partition_read(part, 0, &val, 1) < 0)
        return -1;

    *index = (val < 4) ? val : 4;
    return 0;
}

/**
 * @brief 写入启动分区索引（下次启动生效）
 *
 * 擦除整个 param_boot 分区，写入 1 字节索引值。
 *
 * @param ctx   存储上下文（本实现忽略）
 * @param index 启动索引 (0~4)
 * @return 0 成功, -1 失败
 */
static int flashdb_set_active_partition(void *ctx, uint8_t index)
{
    (void)ctx;

    const struct fal_partition *part = fal_partition_find("param_boot");
    if (!part)
        return -1;

    if (fal_partition_erase(part, 0, part->len) < 0)
        return -1;

    if (fal_partition_write(part, 0, &index, 1) < 0)
        return -1;

    return 0;
}

/**
 * @brief 按索引获取分区驱动 — 单例语义 (运行时分区切换用)
 *
 * 索引映射: 0→param_user0, 1→param_user1, 2→param_user2,
 *           3→param_user3, 其他→param_factory。
 * 底层 param_storage_flashdb_get_driver 保证同一分区名返回同一实例。
 * 运行时切换允许目标分区为空 — 由调用者决定是否 param_load_all。
 *
 * @param ctx   存储上下文（本实现忽略）
 * @param index 分区索引
 * @return 驱动指针，池耗尽时返回 NULL
 */
static const param_storage_drv_t *flashdb_get_partition(void *ctx, uint8_t index)
{
    (void)ctx;

    static const char *names[] = {
        "param_user0",
        "param_user1",
        "param_user2",
        "param_user3",
        "param_factory",
    };
    const char *name = (index < 5) ? names[index] : "param_factory";
    return param_storage_flashdb_get_driver(name);
}

/**
 * @brief FlashDB 加载: 通过键名 "p<param_id>" 读取 Blob 数据
 *
 * @param ctx       FlashDB 上下文
 * @param param_id  参数 ID
 * @param data      输出缓冲区
 * @param len       缓冲区长度
 * @return 0 成功, -1 键不存在或读取失败
 */
static int flashdb_load(void *ctx, uint32_t param_id,
                        uint8_t *data, uint16_t len)
{
    flashdb_ctx_t *c = (flashdb_ctx_t *)ctx;
    if (!c || !c->kvdb_ready || !data || len == 0)
        return -1;

    char key[16];
    snprintf(key, sizeof(key), "p%lu", (unsigned long)param_id);

    struct fdb_blob blob;
    size_t read_len = fdb_kv_get_blob(&c->kvdb, key,
                                      fdb_blob_make(&blob, data, len));
    return (read_len > 0) ? 0 : -1;
}

/**
 * @brief FlashDB 保存: 以键名 "p<param_id>" 写入 Blob 数据
 *
 * @param ctx       FlashDB 上下文
 * @param param_id  参数 ID
 * @param data      数据缓冲区
 * @param len       数据长度
 * @return 0 成功, -1 写入失败
 */
static int flashdb_save(void *ctx, uint32_t param_id,
                        const uint8_t *data, uint16_t len)
{
    flashdb_ctx_t *c = (flashdb_ctx_t *)ctx;
    if (!c || !c->kvdb_ready || !data || len == 0)
        return -1;

    char key[16];
    snprintf(key, sizeof(key), "p%lu", (unsigned long)param_id);

    struct fdb_blob blob;
    fdb_err_t result = fdb_kv_set_blob(&c->kvdb, key,
                                       fdb_blob_make(&blob, data, len));
    return (result == FDB_NO_ERR) ? 0 : -1;
}

/**
 * @brief FlashDB 删除: 删除键 "p<param_id>"
 *
 * @param ctx       FlashDB 上下文
 * @param param_id  参数 ID
 * @return 0 成功, -1 失败
 */
static int flashdb_delete(void *ctx, uint32_t param_id)
{
    flashdb_ctx_t *c = (flashdb_ctx_t *)ctx;
    if (!c || !c->kvdb_ready)
        return -1;

    char key[16];
    snprintf(key, sizeof(key), "p%lu", (unsigned long)param_id);
    fdb_err_t result = fdb_kv_del(&c->kvdb, key);
    return (result == FDB_NO_ERR) ? 0 : -1;
}

/**
 * @brief FlashDB 全量擦除: 反初始化后重新初始化 KVDB
 *
 * 采用"销毁→重建"策略而非逐键删除，效率远高于迭代删除。
 *
 * @param ctx FlashDB 上下文
 * @return 0 成功, -1 失败
 */
static int flashdb_erase_all(void *ctx)
{
    flashdb_ctx_t *c = (flashdb_ctx_t *)ctx;
    if (!c || !c->kvdb_ready)
        return -1;

    fdb_kvdb_deinit(&c->kvdb);
    c->kvdb_ready = false;

    return flashdb_init_kvdb(c);
}

/**
 * @brief FlashDB 去初始化: 反初始化 KVDB 并标记未就绪
 *
 * @param ctx FlashDB 上下文
 * @return 0 成功, -1 失败
 */
static int flashdb_deinit(void *ctx)
{
    flashdb_ctx_t *c = (flashdb_ctx_t *)ctx;
    if (!c)
        return -1;
    if (c->kvdb_ready)
    {
        fdb_kvdb_deinit(&c->kvdb);
        c->kvdb_ready = false;
    }
    return 0;
}

/**
 * @brief 创建默认存储驱动（启动时调用，智能分区选择）
 *
 * @details
 * 启动流程分 4 个阶段，任意阶段失败均回退到 param_factory:
 *
 *   1. FAL 初始化: 若尚未初始化则调用 fal_init()（惰性初始化，仅首次）
 *   2. 读取启动索引: 从 "param_boot" 裸分区读取 1 字节 boot_index
 *      - 0~3 → 映射到 param_user0~param_user3
 *      - 其他 → 直接使用 param_factory
 *   3. 初始化目标分区: 通过工厂获取对应分区驱动（触发 KVDB 初始化）
 *      - 驱动获取失败 → 回退 param_factory
 *   4. 空分区检测: boot_index<4 且目标分区为空时
 *      - 反初始化目标，切换到 param_factory，写入 boot_index=4
 *      - 确保新出厂设备首次启动不会读到空用户分区
 *
 * 设计意图: 通过启动分区索引实现简单的 A/B 分区切换能力。
 * 固件升级时写入目标 boot_index，下次启动自动切换到新分区。
 *
 * @return 已初始化的存储驱动（保证非 NULL，最坏返回 factory 驱动）
 */
const param_storage_drv_t *param_storage_flashdb_create(void)
{
    /* 阶段 1: 惰性 FAL 初始化 */
    if (!g_fal_inited)
    {
        fal_init();
        g_fal_inited = true;
    }

    /* 阶段 2: 读取启动分区索引 */
    uint8_t boot_index = 0xFF;
    const struct fal_partition *boot = fal_partition_find("param_boot");
    if (boot)
        fal_partition_read(boot, 0, &boot_index, 1);

    /* 阶段 3: 映射索引到分区名 */
    static const char *user_parts[] = {
        "param_user0",
        "param_user1",
        "param_user2",
        "param_user3",
    };
    const char *target;
    if (boot_index < 4)
        target = user_parts[boot_index];
    else
        target = "param_factory";

    /* 阶段 4: 初始化目标分区，空则回退 factory */
    const param_storage_drv_t *drv = param_storage_flashdb_get_driver(target);
    if (!drv)
    {
        drv = param_storage_flashdb_get_driver("param_factory");
        return drv;
    }

    if (boot_index < 4 && kvdb_is_empty((flashdb_ctx_t *)drv->ctx))
    {
        drv->deinit(drv->ctx);
        drv = param_storage_flashdb_get_driver("param_factory");
        flashdb_set_active_partition(NULL, 4);
    }

    return drv;
}

/**
 * @brief 获取 FlashDB 持久化后端驱动（工厂模式，按分区名创建独立实例）
 *
 * @details
 * 采用"查找→创建"两阶段工厂模式:
 *
 *   1. 查找已有实例: 遍历 g_ctx[] 查找 used==true 且 part_name 匹配的槽位。
 *      找到后若 kvdb_ready 为 false 则惰性重新初始化。
 *      同一分区名多次调用返回同一实例指针（幂等性保证）。
 *
 *   2. 创建新实例: 若无匹配，分配第一个空闲槽位 (used==false)。
 *      初始化 flashdb_ctx_t 结构体，填充所有 7 个存储驱动回调:
 *        load / save / delete / erase_all / deinit /
 *        get_active_partition / set_active_partition / get_partition
 *      设置 used=true，惰性初始化 FAL，调用 flashdb_init_kvdb。
 *
 *   3. 池耗尽: 所有 MAX_INSTANCES 个槽位均已使用时返回 NULL。
 *
 * 典型用法:
 * @code
 *   drv0 = param_storage_flashdb_get_driver("param_user0");
 *   drv1 = param_storage_flashdb_get_driver("param_bank1");
 *   param_init(drv0);
 *   param_set_storage(drv1);  // 运行时切换分区
 * @endcode
 *
 * @param part_name FAL 分区名 (如 "param_user0")，不能为 NULL
 * @return 驱动句柄（静态分配），同一分区名返回同一实例；池耗尽返回 NULL
 */
const param_storage_drv_t *param_storage_flashdb_get_driver(const char *part_name)
{
    if (!part_name)
        return NULL;

    /* 阶段 1: 查找已有实例 */
    for (int i = 0; i < MAX_INSTANCES; i++)
    {
        if (g_ctx[i].used &&
            strncmp(g_ctx[i].part_name, part_name, sizeof(g_ctx[i].part_name)) == 0)
        {
            /* 惰性重初始化：若 KVDB 曾被 deinit 则重新初始化 */
            if (!g_ctx[i].kvdb_ready)
                flashdb_init_kvdb(&g_ctx[i]);
            return &g_ctx[i].drv;
        }
    }

    /* 阶段 2: 分配新槽位并初始化 */
    for (int i = 0; i < MAX_INSTANCES; i++)
    {
        if (!g_ctx[i].used)
        {
            flashdb_ctx_t *c = &g_ctx[i];
            memset(c, 0, sizeof(*c));
            strncpy(c->part_name, part_name, sizeof(c->part_name) - 1);
            c->part_name[sizeof(c->part_name) - 1] = '\0';
            /* 绑定全部 8 个存储驱动回调 */
            c->drv.ctx = c;
            c->drv.load = flashdb_load;
            c->drv.save = flashdb_save;
            c->drv.delete = flashdb_delete;
            c->drv.erase_all = flashdb_erase_all;
            c->drv.deinit = flashdb_deinit;
            c->drv.get_active_partition = flashdb_get_active_partition;
            c->drv.set_active_partition = flashdb_set_active_partition;
            c->drv.get_partition = flashdb_get_partition;
            c->used = true;

            /* 惰性 FAL 初始化（全局仅一次） */
            if (!g_fal_inited)
            {
                fal_init();
                g_fal_inited = true;
            }

            flashdb_init_kvdb(c);
            return &c->drv;
        }
    }

    /* 阶段 3: 池耗尽 */
    return NULL;
}

#endif
