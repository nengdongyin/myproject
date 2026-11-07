/**
 * @file param_storage_mkv.c
 * @brief 基于极简 KV (mkv) 的持久化存储后端实现
 *
 * 实现 param_storage_drv_t 接口的 mkv 适配层。
 * 多实例工厂模式: g_ctx[MAX_INSTANCES] 静态池，按分区名查找/创建。
 * 同一分区名返回同一实例（幂等）。
 */

#include "param_storage_mkv.h"
#include "param_storage_common.h"
#include "param_storage_fal.h"
#include "mkv.h"
#include "fal.h"
#include <string.h>

/** @brief mkv 存储上下文 — 每个物理分区对应一个实例 */
typedef struct
{
    bool     used;
    mkv_t    kv;
    char     part_name[24];
    param_storage_drv_t drv;
} mkv_storage_ctx_t;

/** @brief 全局存储实例池 */
static mkv_storage_ctx_t g_ctx[MAX_INSTANCES];

/* 分区索引→分区名 的映射与 MAX_INSTANCES 断言由 param_storage_common.h 提供;
   启动分区索引读写与 FAL 惰性初始化由 param_storage_fal.h 提供 */

/* ═══════════════════════════════════════ 存储回调 ═══════════════ */

static int mkv_storage_load(void *ctx, uint32_t id, uint8_t *data, uint16_t len,
                            uint16_t *read_len)
{
    mkv_storage_ctx_t *c = (mkv_storage_ctx_t *)ctx;
    if (!c || !c->used || !data || len == 0) return PARAM_ERR_INVALID_ID;

    int got = mkv_get(&c->kv, id, data, len);
    if (got > 0)
    {
        if (read_len) *read_len = (uint16_t)got;
        return PARAM_OK;
    }
    /* 0 = 记录不存在或墓碑: 框架保持默认值, 非错误 */
    if (got == 0) return PARAM_ERR_NOT_FOUND;

    /* mkv_get 的 -1 = 内部/参数错误。
       注: mkv_get 内部已按 max_len 截断, 取不到记录原始长度,
       故本后端无法上报 PARAM_ERR_OUT_OF_RANGE (见 drv 契约注释)。 */
    return PARAM_ERR_FLASH_FAIL;
}

/** @brief mkv_scan 回调上下文 — 把记录逐条推给框架注入的 sink */
typedef struct
{
    const param_restore_sink_t *sink; /**< 框架提供的数据出口 (仅在本次调用期间有效) */
    uint16_t rejected;                /**< 未被框架接受的记录数 (饱和于 UINT16_MAX) */
} mkv_restore_ctx_t;

static void mkv_scan_restore_cb(uint32_t id, const uint8_t *data, uint16_t len, void *user)
{
    mkv_restore_ctx_t *rc = (mkv_restore_ctx_t *)user;

    /* 单条被拒不中断扫描: 计数后由 load_all 以 >0 让框架回退逐条 load。
       PARAM_ERR_NOT_FOUND (旧固件遗留的未注册 ID) 属正常情形, 不计入 ——
       否则参数表一旦瘦身, 每次开机都会退化为全量逐条扫描。 */
    int ret = rc->sink->restore(rc->sink->ctx, id, data, len);
    if (ret != PARAM_OK && ret != PARAM_ERR_NOT_FOUND && rc->rejected < UINT16_MAX)
        rc->rejected++;
}

static int mkv_storage_load_all(void *ctx, const param_restore_sink_t *sink)
{
    mkv_storage_ctx_t *c = (mkv_storage_ctx_t *)ctx;
    if (!c || !c->used || !sink || !sink->restore) return -1;

    mkv_restore_ctx_t rc = { .sink = sink, .rejected = 0 };
    int ret = mkv_scan(&c->kv, mkv_scan_restore_cb, &rc);
    if (ret != 0) return ret;  /* -1 参数/内部错误; 1 扫描不完整 */

    /* 有记录未被接受 (容量不符/数据损坏) → 返回 >0 让 param_load_all 回退
       逐条 load: 那条路径按条目真实容量读, 能区分"不存在"与"读失败",
       不会把损坏记录静默退化为默认值 */
    return rc.rejected ? 1 : 0;
}

static int mkv_storage_save(void *ctx, uint32_t id, const uint8_t *data, uint16_t len)
{
    mkv_storage_ctx_t *c = (mkv_storage_ctx_t *)ctx;
    if (!c || !c->used || !data || len == 0) return -1;
    return mkv_set(&c->kv, id, data, len);
}

static int mkv_storage_delete(void *ctx, uint32_t id)
{
    mkv_storage_ctx_t *c = (mkv_storage_ctx_t *)ctx;
    if (!c || !c->used) return -1;
    return mkv_del(&c->kv, id);
}

static int mkv_storage_erase_all(void *ctx)
{
    mkv_storage_ctx_t *c = (mkv_storage_ctx_t *)ctx;
    if (!c || !c->used) return -1;
    return mkv_erase_all(&c->kv);
}

static int mkv_storage_deinit(void *ctx)
{
    mkv_storage_ctx_t *c = (mkv_storage_ctx_t *)ctx;
    if (!c) return -1;
    mkv_deinit(&c->kv);
    return 0;
}

static const param_storage_drv_t *mkv_storage_get_partition(void *ctx, uint8_t index)
{
    (void)ctx;
    return param_storage_mkv_get_driver(param_partition_name(index));
}

/* ═══════════════════════════════════════ 公开 API ═══════════════ */

const param_storage_drv_t *param_storage_mkv_create(void)
{
    param_fal_ensure_init();

    /* 读取启动分区索引 (失败则保持 0xFF → 走 factory) */
    uint8_t boot_index = 0xFF;
    (void)param_boot_index_get(NULL, &boot_index);

    const char *target = param_partition_name(boot_index);

    const param_storage_drv_t *drv = param_storage_mkv_get_driver(target);
    if (!drv)
        drv = param_storage_mkv_get_driver(param_partition_name(PARAM_PARTITION_FACTORY));
    return drv;
}

const param_storage_drv_t *param_storage_mkv_get_driver(const char *part_name)
{
    if (!part_name) return NULL;

    for (int i = 0; i < MAX_INSTANCES; i++)
    {
        if (g_ctx[i].used &&
            strncmp(g_ctx[i].part_name, part_name, sizeof(g_ctx[i].part_name)) == 0)
            return &g_ctx[i].drv;
    }

    for (int i = 0; i < MAX_INSTANCES; i++)
    {
        if (!g_ctx[i].used)
        {
            mkv_storage_ctx_t *c = &g_ctx[i];
            memset(c, 0, sizeof(*c));

            if (mkv_init(&c->kv, part_name) < 0)
                return NULL;

            strncpy(c->part_name, part_name, sizeof(c->part_name) - 1);
            c->part_name[sizeof(c->part_name) - 1] = '\0';

            c->drv.ctx = c;
            c->drv.load = mkv_storage_load;
            c->drv.load_all = mkv_storage_load_all;
            c->drv.save = mkv_storage_save;
            c->drv.delete = mkv_storage_delete;
            c->drv.erase_all = mkv_storage_erase_all;
            c->drv.deinit = mkv_storage_deinit;
            c->drv.get_active_partition = param_boot_index_get;
            c->drv.set_active_partition = param_boot_index_set;
            c->drv.get_partition = mkv_storage_get_partition;
            c->used = true;

            return &c->drv;
        }
    }

    return NULL;
}
