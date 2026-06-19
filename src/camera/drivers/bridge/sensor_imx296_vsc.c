/**
 * @file    sensor_imx296_vsc.c
 * @brief   IMX296 传感器 VSC 适配器 — 桥接 ISC 框架
 *
 * 基于 sensor_vsc.c 模板，针对 IMX296 (1440×1080, RAW8/RAW10)。
 */

#include "sensor_imx296_vsc.h"
#include "isc_bridge.h"
#include "isc.h"
#include "vsc_driver_ids.h"
#include <string.h>

static void vsc_to_isc(const vsc_mbus_fmt_t *v, isc_fmt_t *i)
{
    memset(i, 0, sizeof(*i));
    i->crop_left   = v->spatial.offsetx;
    i->crop_top    = v->spatial.offsety;
    i->crop_width  = v->spatial.width;
    i->crop_height = v->spatial.height;
    i->bin_x       = v->spatial.bin_x;
    i->bin_y       = v->spatial.bin_y;
    i->dec_x       = v->spatial.dec_x;
    i->dec_y       = v->spatial.dec_y;
    i->pixel_format   = v->spatial.pixel_format;
    i->frame_rate_num = v->spatial.frame_rate_num;
    i->frame_rate_den = v->spatial.frame_rate_den;
    i->bit_depth      = v->spatial.bit_depth;
}

static void isc_to_vsc(const isc_fmt_t *i, vsc_mbus_fmt_t *v)
{
    v->spatial.width          = i->width;
    v->spatial.height         = i->height;
    v->spatial.pixel_format   = i->pixel_format;
    v->spatial.frame_rate_num = i->frame_rate_num;
    v->spatial.frame_rate_den = i->frame_rate_den;
    v->spatial.bit_depth      = i->bit_depth;
    v->spatial.bin_x   = (i->bin_x    == v->spatial.bin_x)   ? 1 : v->spatial.bin_x;
    v->spatial.bin_y   = (i->bin_y    == v->spatial.bin_y)   ? 1 : v->spatial.bin_y;
    v->spatial.dec_x   = (i->dec_x    == v->spatial.dec_x)   ? 1 : v->spatial.dec_x;
    v->spatial.dec_y   = (i->dec_y    == v->spatial.dec_y)   ? 1 : v->spatial.dec_y;
    v->spatial.offsetx = (i->crop_left == v->spatial.offsetx) ? 0 : v->spatial.offsetx;
    v->spatial.offsety = (i->crop_top  == v->spatial.offsety) ? 0 : v->spatial.offsety;
}

static int imx296_vsc_init(void *inst)
{
    sensor_imx296_vsc_inst_t *ctx = (sensor_imx296_vsc_inst_t *)inst;

    int rc = isc_bridge_init();
    if (rc != 0) return VSC_ERR_CANNOT_AUTO_BRIDGE;

    rc = isc_open("imx296", &ctx->isc_dev);
    if (rc != ISC_OK) {
        ctx->isc_dev = NULL;
        rc = VSC_OK;
    }
    return VSC_OK;
}

static int imx296_vsc_try_fmt_source(void *drv_ctx,
                                     const vsc_mbus_fmt_t *intent,
                                     vsc_mbus_fmt_t *source_fmt)
{
    sensor_imx296_vsc_inst_t *ctx = (sensor_imx296_vsc_inst_t *)drv_ctx;

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
            if (supported[i] == intent->spatial.pixel_format) { ok = true; break; }
        if (!ok) source_fmt->spatial.pixel_format = supported[0];
        if (source_fmt->spatial.width  > 1440) source_fmt->spatial.width  = 1440;
        if (source_fmt->spatial.height > 1080) source_fmt->spatial.height = 1080;
    }
    return VSC_OK;
}

static int imx296_vsc_commit_fmt(void *drv_ctx,
                                 const vsc_mbus_fmt_t *final_fmt)
{
    sensor_imx296_vsc_inst_t *ctx = (sensor_imx296_vsc_inst_t *)drv_ctx;
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
        .get_timing_req = NULL,
        .query_cap      = NULL,
        .set_ctrl       = NULL,
        .get_ctrl       = NULL,
    },
};
