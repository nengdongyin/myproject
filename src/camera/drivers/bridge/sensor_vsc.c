/**
 * @file    sensor_vsc.c
 * @brief   图像传感器 VSC 适配器 — 桥接 ISC (Image Sensor Controller) 框架。
 *
 * 通过 isc_bridge_init() 初始化 ISC，调用 isc_open/isc_try_fmt/isc_set_fmt。
 */

#include "sensor_vsc.h"
#include "isc_bridge.h"
#include "isc.h"
#include "vsc_prop_ids.h"
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════
 *  格式转换 helper: vsc_mbus_fmt_t ↔ isc_fmt_t
 * ═══════════════════════════════════════════════════════════════════════ */

static void vsc_to_isc(const vsc_mbus_fmt_t *v, isc_fmt_t *i)
{
    memset(i, 0, sizeof(*i));
    i->width         = v->width;
    i->height        = v->height;
    i->pixel_format  = v->pixel_format;
    i->frame_rate_num = v->frame_rate_num;
    i->frame_rate_den = v->frame_rate_den;
    i->bit_depth     = v->bit_depth;
}

static void isc_to_vsc(const isc_fmt_t *i, vsc_mbus_fmt_t *v)
{
    v->width         = i->width;
    v->height        = i->height;
    v->pixel_format  = i->pixel_format;
    v->frame_rate_num = i->frame_rate_num;
    v->frame_rate_den = i->frame_rate_den;
    v->bit_depth     = i->bit_depth;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  私有上下文
 * ═══════════════════════════════════════════════════════════════════════ */

#define SENSOR_MAX_INSTANCES 2

typedef struct {
    bool     in_use;
    isc_dev_t *isc_dev;      /* ISC 设备句柄 */
    char     model[32];
} sensor_ctx_t;

static sensor_ctx_t g_pool[SENSOR_MAX_INSTANCES];

void sensor_vsc_reset(void)
{
    /* close any open ISC devices */
    for (int i = 0; i < SENSOR_MAX_INSTANCES; i++) {
        if (g_pool[i].in_use && g_pool[i].isc_dev) {
            isc_close(g_pool[i].isc_dev);
        }
    }
    memset(g_pool, 0, sizeof(g_pool));
}

/* ═══════════════════════════════════════════════════════════════════════
 *  ops.init()
 * ═══════════════════════════════════════════════════════════════════════ */

static int sensor_vsc_init(void **drv_ctx, uint32_t base_addr,
                           const vsc_override_t *overrides,
                           uint8_t num_over)
{
    (void)base_addr;

    /* 确保 ISC 框架已初始化 */
    int rc = isc_bridge_init();
    if (rc != 0) return VSC_ERR_CANNOT_AUTO_BRIDGE;

    sensor_ctx_t *ctx = NULL;
    for (int i = 0; i < SENSOR_MAX_INSTANCES; i++)
        if (!g_pool[i].in_use) { ctx = &g_pool[i]; ctx->in_use = true; break; }
    if (!ctx) return VSC_ERR_TOPOLOGY_BROKEN;

    strcpy(ctx->model, "sensor_imx477");
    for (uint8_t i = 0; i < num_over; i++) {
        if (strcmp(overrides[i].key, "model") == 0)
            strncpy(ctx->model, (const char *)(uintptr_t)overrides[i].value, 31);
    }

    /* 打开传感器（ISC 可用时） */
    rc = isc_open(ctx->model, &ctx->isc_dev);
    if (rc != ISC_OK) {
        /* ISC 不可用时使用内置格式表 (测试环境回退) */
        ctx->isc_dev = NULL;
        rc = VSC_OK;
    }

    *drv_ctx = ctx;
    return VSC_OK;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  ops.try_fmt_source()
 * ═══════════════════════════════════════════════════════════════════════ */

static int sensor_vsc_try_fmt_source(void *drv_ctx,
                                     const vsc_mbus_fmt_t *intent,
                                     vsc_mbus_fmt_t *source_fmt)
{
    sensor_ctx_t *ctx = (sensor_ctx_t *)drv_ctx;

    if (ctx->isc_dev) {
        isc_fmt_t isc_in;
        vsc_to_isc(intent, &isc_in);
        int rc = isc_try_fmt(ctx->isc_dev, &isc_in);
        if (rc != ISC_OK) return VSC_ERR_PROPAGATION_SOURCE;
        isc_to_vsc(&isc_in, source_fmt);
    } else {
        /* 回退：内置格式表 */
        *source_fmt = *intent;
        uint32_t supported[] = { VSC_FMT_RAW8, VSC_FMT_RAW10, VSC_FMT_RAW12, 0 };
        bool ok = false;
        for (int i = 0; supported[i]; i++)
            if (supported[i] == intent->pixel_format) { ok = true; break; }
        if (!ok) source_fmt->pixel_format = supported[0];
        if (source_fmt->width  > 4056) source_fmt->width  = 4056;
        if (source_fmt->height > 3040) source_fmt->height = 3040;
    }
    return VSC_OK;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  ops.commit_fmt()
 * ═══════════════════════════════════════════════════════════════════════ */

static int sensor_vsc_commit_fmt(void *drv_ctx,
                                 const vsc_mbus_fmt_t *final_fmt)
{
    sensor_ctx_t *ctx = (sensor_ctx_t *)drv_ctx;
    if (ctx->isc_dev) {
        isc_fmt_t isc_fmt;
        vsc_to_isc(final_fmt, &isc_fmt);
        int rc = isc_set_fmt(ctx->isc_dev, &isc_fmt);
        return (rc == ISC_OK) ? VSC_OK : VSC_ERR_COMMIT_FAILED;
    }
    (void)final_fmt;
    return VSC_OK;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Transform template（供 Phase 1 可行性检查使用）
 * ═══════════════════════════════════════════════════════════════════════ */

static const vsc_fmt_transform_desc_t s_sensor_template = {
    .type = VSC_TRANSFORM_CROP,
    .params.crop = { .min_w = 1, .min_h = 1, .max_w = 4056, .max_h = 3040 },
};

/* ═══════════════════════════════════════════════════════════════════════
 *  Driver 实例
 * ═══════════════════════════════════════════════════════════════════════ */

const vsc_driver_t sensor_imx477_vsc_driver = {
    .name         = "sensor_imx477",
    .driver_id    = VSC_DRIVER_ID_SENSOR_IMX477,
    .capabilities = VSC_CAP_SENSOR | VSC_CAP_EXPOSURE_CTRL,
    .transform_template = &s_sensor_template,
    .ops = {
        .init           = sensor_vsc_init,
        .try_fmt_sink   = NULL,
        .try_fmt_source = sensor_vsc_try_fmt_source,
        .commit_fmt     = sensor_vsc_commit_fmt,
    },
};
