/**
 * @file    crop_vsc.c
 * @brief   Crop IP 的 VSC 适配器实现。
 *
 * 实例（crop_vsc_inst_t）由应用层编译期静态分配，
 * base_addr 和出厂默认值已在实例中预设。
 */

#include "crop_vsc.h"
#include "vsc_driver_ids.h"
#include <string.h>

static int crop_vsc_init(void *inst)
{
    crop_vsc_inst_t *ctx = (crop_vsc_inst_t *)inst;
    (void)ctx;
    return VSC_OK;
}

static int crop_vsc_try_fmt_sink(void *drv_ctx,
                                 const vsc_mbus_fmt_t *proposed,
                                 vsc_mbus_fmt_t *clamped)
{
    crop_vsc_inst_t *ctx = (crop_vsc_inst_t *)drv_ctx;
    uint32_t max_w, max_h;
    crop_get_limits(&ctx->hw, &max_w, &max_h);

    *clamped = *proposed;
    if (clamped->spatial.width  > max_w) clamped->spatial.width  = max_w;
    if (clamped->spatial.height > max_h) clamped->spatial.height = max_h;
    return VSC_OK;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  ops.try_fmt_source()
 * ═══════════════════════════════════════════════════════════════════════ */

static int crop_vsc_try_fmt_source(void *drv_ctx,
                                   const vsc_mbus_fmt_t *sink_fmt,
                                   vsc_mbus_fmt_t *source_fmt)
{
    crop_vsc_inst_t *ctx = (crop_vsc_inst_t *)drv_ctx;
    uint32_t max_w, max_h;
    crop_get_limits(&ctx->hw, &max_w, &max_h);

    /* source_fmt 由框架预填（Phase 0 写 intent，Phase A 已乘待认领缩放因子）。
     * 直接读作裁剪目标，无需理解 bin/dec 语义。 */
    uint32_t target_w = source_fmt->spatial.width;
    uint32_t target_h = source_fmt->spatial.height;

    *source_fmt = *sink_fmt;

    /* ── 认领 crop offset（若 sink 携带）─── */
    if (sink_fmt->spatial.offsetx > 0 || sink_fmt->spatial.offsety > 0)
    {
        uint32_t needed_w = sink_fmt->spatial.offsetx + target_w;
        uint32_t needed_h = sink_fmt->spatial.offsety + target_h;

        if (needed_w > max_w || needed_h > max_h)
            return VSC_ERR_UNREACHABLE;

        if (target_w > 0 && target_w < source_fmt->spatial.width)
            source_fmt->spatial.width  = target_w;
        if (target_h > 0 && target_h < source_fmt->spatial.height)
            source_fmt->spatial.height = target_h;

        source_fmt->spatial.offsetx = 0;
        source_fmt->spatial.offsety = 0;
    }
    else
    {
        if (target_w > 0 && target_w < source_fmt->spatial.width)
            source_fmt->spatial.width  = target_w;
        if (target_h > 0 && target_h < source_fmt->spatial.height)
            source_fmt->spatial.height = target_h;

        if (source_fmt->spatial.width  > max_w) source_fmt->spatial.width  = max_w;
        if (source_fmt->spatial.height > max_h) source_fmt->spatial.height = max_h;
    }

    /* 对齐 */
    if (ctx->hw.align_w > 1)
        source_fmt->spatial.width  -= source_fmt->spatial.width  % ctx->hw.align_w;
    if (ctx->hw.align_h > 1)
        source_fmt->spatial.height -= source_fmt->spatial.height % ctx->hw.align_h;
    if (source_fmt->spatial.width  == 0) source_fmt->spatial.width  = ctx->hw.align_w;
    if (source_fmt->spatial.height == 0) source_fmt->spatial.height = ctx->hw.align_h;

    return VSC_OK;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  ops.commit_fmt()
 * ═══════════════════════════════════════════════════════════════════════ */

static int crop_vsc_commit_fmt(void *drv_ctx,
                               const vsc_mbus_fmt_t *final_fmt)
{
    crop_vsc_inst_t *ctx = (crop_vsc_inst_t *)drv_ctx;
    uint32_t max_w, max_h;
    crop_get_limits(&ctx->hw, &max_w, &max_h);

    if (final_fmt->spatial.width  > max_w) return VSC_ERR_COMMIT_FAILED;
    if (final_fmt->spatial.height > max_h) return VSC_ERR_COMMIT_FAILED;

    /* 更新 ROI 为 final_fmt 的尺寸，保持 x/y 不变 */
    crop_set_roi(&ctx->hw, ctx->hw.roi_x, ctx->hw.roi_y,
                 final_fmt->spatial.width, final_fmt->spatial.height);
    crop_enable(&ctx->hw);
    crop_commit(&ctx->hw);
    return VSC_OK;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Driver 实例
 * ═══════════════════════════════════════════════════════════════════════ */

static const vsc_fmt_transform_desc_t s_crop_template = {
    .type = VSC_TRANSFORM_CROP,
    .params.crop = { .min_w = 64, .min_h = 4, .max_w = 8192, .max_h = 8192,
                     .align_w = 8, .align_h = 8 },
};

const vsc_driver_t crop_vsc_driver = {
    .name               = "crop",
    .driver_id          = VSC_DRIVER_ID_CROP,
    .capabilities       = VSC_CAP_CROP,
    .schema             = NULL,
    .prop_count         = 0,
    .transform_template = &s_crop_template,
    .ops = {
        .init           = crop_vsc_init,
        .try_fmt_sink   = crop_vsc_try_fmt_sink,
        .try_fmt_source = crop_vsc_try_fmt_source,
        .commit_fmt     = crop_vsc_commit_fmt,
        .get_timing_req = NULL,
        .query_cap      = NULL,
        .set_ctrl       = NULL,
        .get_ctrl       = NULL,
    },
};
