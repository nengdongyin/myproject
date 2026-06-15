/**
 * @file param_storage_mkv.c
 * @brief 基于极简 KV (mkv) 的持久化存储后端实现
 *
 * 实现 param_storage_drv_t 接口的 mkv 适配层。
 * 多实例工厂模式: g_ctx[MAX_INSTANCES] 静态池，按分区名查找/创建。
 * 同一分区名返回同一实例（幂等）。
 */

#include "param_storage_mkv.h"
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

#define FAL_BOOT_PART "param_boot"

static const char *const g_partition_names[PARAM_PARTITION_COUNT] = {
    [PARAM_PARTITION_FACTORY] = "param_factory",
    [1] = "param_user0",
    [2] = "param_user1",
    [3] = "param_user2",
    [4] = "param_user3",
};

/* ═══════════════════════════════════════ 存储回调 ═══════════════ */

static int mkv_storage_load(void *ctx, uint32_t id, uint8_t *data, uint16_t len)
{
    mkv_storage_ctx_t *c = (mkv_storage_ctx_t *)ctx;
    if (!c || !c->used || !data || len == 0) return -1;
    return mkv_get(&c->kv, id, data, len);
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

static int mkv_storage_get_active_partition(void *ctx, uint8_t *index)
{
    (void)ctx;
    if (!index) return -1;
    const struct fal_partition *part = fal_partition_find(FAL_BOOT_PART);
    if (!part) return -1;
    uint8_t val = 0xFF;
    if (fal_partition_read(part, 0, &val, 1) < 0) return -1;
    *index = (val < PARAM_PARTITION_COUNT) ? val : PARAM_PARTITION_FACTORY;
    return 0;
}

static int mkv_storage_set_active_partition(void *ctx, uint8_t index)
{
    (void)ctx;
    const struct fal_partition *part = fal_partition_find(FAL_BOOT_PART);
    if (!part) return -1;
    if (fal_partition_erase(part, 0, part->len) < 0) return -1;
    return fal_partition_write(part, 0, &index, 1);
}

static const param_storage_drv_t *mkv_storage_get_partition(void *ctx, uint8_t index)
{
    (void)ctx;
    const char *name;
    if (index < PARAM_PARTITION_COUNT && g_partition_names[index])
        name = g_partition_names[index];
    else
        name = g_partition_names[PARAM_PARTITION_FACTORY];
    return param_storage_mkv_get_driver(name);
}

/* ═══════════════════════════════════════ 公开 API ═══════════════ */

const param_storage_drv_t *param_storage_mkv_create(void)
{
    uint8_t boot_index = 0xFF;
    const struct fal_partition *boot = fal_partition_find(FAL_BOOT_PART);
    if (boot)
        fal_partition_read(boot, 0, &boot_index, 1);

    const char *target;
    if (boot_index >= PARAM_PARTITION_USER_MIN && boot_index <= PARAM_PARTITION_USER_MAX)
        target = g_partition_names[boot_index];
    else
        target = g_partition_names[PARAM_PARTITION_FACTORY];

    const param_storage_drv_t *drv = param_storage_mkv_get_driver(target);
    if (!drv)
        drv = param_storage_mkv_get_driver("param_factory");
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
            c->drv.save = mkv_storage_save;
            c->drv.delete = mkv_storage_delete;
            c->drv.erase_all = mkv_storage_erase_all;
            c->drv.deinit = mkv_storage_deinit;
            c->drv.get_active_partition = mkv_storage_get_active_partition;
            c->drv.set_active_partition = mkv_storage_set_active_partition;
            c->drv.get_partition = mkv_storage_get_partition;
            c->used = true;

            return &c->drv;
        }
    }

    return NULL;
}
