/**
 * @file    sensor_imx296_vsc.c
 * @brief   IMX296 传感器 VSC 适配器 — 桥接 ISC 框架
 *
 * 基于 sensor_vsc.c 模板，针对 IMX296 (1440×1080, RAW8/RAW10)。
 */

#include "isc_bridge.h"
#include "isc.h"
#include "vsc_types.h"
#include "vsc_prop_ids.h"
#include <string.h>

/* ═══════════════════════════════════════════ 格式转换 ═══════════ */

static void vsc_to_isc(const vsc_mbus_fmt_t *v, isc_fmt_t *i)
{
    memset(i, 0, sizeof(*i));
    i->width          = v->width;
    i->height         = v->height;
    i->pixel_format   = v->pixel_format;
    i->frame_rate_num = v->frame_rate_num;
    i->frame_rate_den = v->frame_rate_den;
    i->bit_depth      = v->bit_depth;
}

static void isc_to_vsc(const isc_fmt_t *i, vsc_mbus_fmt_t *v)
{
    v->width          = i->width;
    v->height         = i->height;
    v->pixel_format   = i->pixel_format;
    v->frame_rate_num = i->frame_rate_num;
    v->frame_rate_den = i->frame_rate_den;
    v->bit_depth      = i->bit_depth;
}

/* ═══════════════════════════════════════════ 私有上下文 ═══════════ */

#define IMX296_MAX_INSTANCES 2

typedef struct {
    bool      in_use;
    isc_dev_t *isc_dev;
} imx296_ctx_t;

static imx296_ctx_t g_pool[IMX296_MAX_INSTANCES];

void sensor_imx296_vsc_reset(void)
{
    for (int i = 0; i < IMX296_MAX_INSTANCES; i++) {
        if (g_pool[i].in_use && g_pool[i].isc_dev)
            isc_close(g_pool[i].isc_dev);
    }
    memset(g_pool, 0, sizeof(g_pool));
}

/* ═══════════════════════════════════════════ ops ═══════════ */

static int imx296_vsc_init(void **drv_ctx, uint32_t base_addr,
                           const vsc_override_t *overrides, uint8_t num_over)
{
    (void)base_addr; (void)overrides; (void)num_over;

    int rc = isc_bridge_init();
    if (rc != 0) return VSC_ERR_CANNOT_AUTO_BRIDGE;

    imx296_ctx_t *ctx = NULL;
    for (int i = 0; i < IMX296_MAX_INSTANCES; i++)
        if (!g_pool[i].in_use) { ctx = &g_pool[i]; ctx->in_use = true; break; }
    if (!ctx) return VSC_ERR_TOPOLOGY_BROKEN;

    rc = isc_open("imx296", &ctx->isc_dev);
    if (rc != ISC_OK) {
        ctx->isc_dev = NULL;
        rc = VSC_OK;
    }

    *drv_ctx = ctx;
    return VSC_OK;
}

static int imx296_vsc_try_fmt_source(void *drv_ctx,
                                     const vsc_mbus_fmt_t *intent,
                                     vsc_mbus_fmt_t *source_fmt)
{
    imx296_ctx_t *ctx = (imx296_ctx_t *)drv_ctx;

    if (ctx->isc_dev) {
        isc_fmt_t isc_in;
        vsc_to_isc(intent, &isc_in);
        int rc = isc_try_fmt(ctx->isc_dev, &isc_in);
        if (rc != ISC_OK) return VSC_ERR_PROPAGATION_SOURCE;
        isc_to_vsc(&isc_in, source_fmt);
    } else {
        *source_fmt = *intent;
        uint32_t supported[] = { VSC_FMT_RAW8, VSC_FMT_RAW10, 0 };
        bool ok = false;
        for (int i = 0; supported[i]; i++)
            if (supported[i] == intent->pixel_format) { ok = true; break; }
        if (!ok) source_fmt->pixel_format = supported[0];
        if (source_fmt->width  > 1440) source_fmt->width  = 1440;
        if (source_fmt->height > 1080) source_fmt->height = 1080;
    }
    return VSC_OK;
}

static int imx296_vsc_commit_fmt(void *drv_ctx,
                                 const vsc_mbus_fmt_t *final_fmt)
{
    imx296_ctx_t *ctx = (imx296_ctx_t *)drv_ctx;
    if (ctx->isc_dev) {
        isc_fmt_t isc_fmt;
        vsc_to_isc(final_fmt, &isc_fmt);
        int rc = isc_set_fmt(ctx->isc_dev, &isc_fmt);
        return (rc == ISC_OK) ? VSC_OK : VSC_ERR_COMMIT_FAILED;
    }
    return VSC_OK;
}

/* ═══════════════════════════════════════════ Transform template ═══════════ */

static const vsc_fmt_transform_desc_t s_imx296_template = {
    .type = VSC_TRANSFORM_CROP,
    .params.crop = { .min_w = 1, .min_h = 1, .max_w = 1440, .max_h = 1080 },
};

/* ═══════════════════════════════════════════ Driver 实例 ═══════════ */

const vsc_driver_t sensor_imx296_vsc_driver = {
    .name         = "sensor_imx296",
    .driver_id    = VSC_DRIVER_ID_SENSOR_IMX296,
    .capabilities = VSC_CAP_SENSOR | VSC_CAP_EXPOSURE_CTRL,
    .transform_template = &s_imx296_template,
    .ops = {
        .init           = imx296_vsc_init,
        .try_fmt_sink   = NULL,
        .try_fmt_source = imx296_vsc_try_fmt_source,
        .commit_fmt     = imx296_vsc_commit_fmt,
    },
};
