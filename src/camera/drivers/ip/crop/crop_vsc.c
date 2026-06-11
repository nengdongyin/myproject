/**
 * @file    crop_vsc.c
 * @brief   Crop IP 的 VSC 适配器实现。
 *
 * 持有 crop_dev_t 实例，将 vsc_ip_ops_t 回调委托给纯 HW 驱动。
 *
 * 结构：
 *   crop_vsc_ctx_t
 *     └── crop_dev_t hw      ← 纯硬件驱动实例（零框架依赖）
 *
 *   vsc_ip_ops_t:
 *     init()          → crop_init(&ctx->hw, base_addr)
 *                        + crop_set_limits() for overrides
 *     try_fmt_sink()  → crop_get_limits() 校验 + clamp
 *     try_fmt_source()→ crop_get_roi() 裁剪 + 对齐
 *     commit_fmt()    → crop_set_roi() + crop_commit()
 */

#include "crop_vsc.h"
#include "crop_driver.h"
#include "vsc_prop_ids.h"
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════
 *  私有上下文（每个 Instance 一个）
 * ═══════════════════════════════════════════════════════════════════════ */

#define CROP_VSC_MAX_INSTANCES  4

typedef struct {
    bool        in_use;
    crop_dev_t  hw;        /* 纯 HW 驱动实例 */
} crop_vsc_ctx_t;

static crop_vsc_ctx_t g_ctx_pool[CROP_VSC_MAX_INSTANCES];

/* for test isolation */
void crop_vsc_reset(void)
{
    memset(g_ctx_pool, 0, sizeof(g_ctx_pool));
}

/* ═══════════════════════════════════════════════════════════════════════
 *  ops.init()
 * ═══════════════════════════════════════════════════════════════════════ */

static int crop_vsc_init(void **drv_ctx, uint32_t base_addr,
                         const vsc_override_t *overrides,
                         uint8_t num_over)
{
    /* 从静态池分配 */
    crop_vsc_ctx_t *ctx = NULL;
    for (int i = 0; i < CROP_VSC_MAX_INSTANCES; i++) {
        if (!g_ctx_pool[i].in_use) {
            ctx = &g_ctx_pool[i];
            ctx->in_use = true;
            break;
        }
    }
    if (!ctx) return VSC_ERR_TOPOLOGY_BROKEN;

    /* 初始化纯 HW 驱动 */
    crop_init(&ctx->hw, base_addr);

    /* 应用 overrides（收紧，不放宽） */
    for (uint8_t i = 0; i < num_over; i++) {
        const char *key = overrides[i].key;
        uint32_t    val = overrides[i].value;
        if (strcmp(key, "crop.max_width") == 0 || strcmp(key, "max_width") == 0 ||
            strcmp(key, "crop.max_height") == 0 || strcmp(key, "max_height") == 0) {
            uint32_t mw = ctx->hw.max_w, mh = ctx->hw.max_h;
            if (strstr(key, "width"))  mw = val;
            if (strstr(key, "height")) mh = val;
            if (crop_set_limits(&ctx->hw, mw, mh) != 0)
                return VSC_ERR_UNREACHABLE;
        }
    }

    *drv_ctx = ctx;
    return VSC_OK;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  ops.try_fmt_sink()
 * ═══════════════════════════════════════════════════════════════════════ */

static int crop_vsc_try_fmt_sink(void *drv_ctx,
                                 const vsc_mbus_fmt_t *proposed,
                                 vsc_mbus_fmt_t *clamped)
{
    crop_vsc_ctx_t *ctx = (crop_vsc_ctx_t *)drv_ctx;
    uint32_t max_w, max_h;
    crop_get_limits(&ctx->hw, &max_w, &max_h);

    *clamped = *proposed;
    if (clamped->width  > max_w) clamped->width  = max_w;
    if (clamped->height > max_h) clamped->height = max_h;
    return VSC_OK;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  ops.try_fmt_source()
 * ═══════════════════════════════════════════════════════════════════════ */

static int crop_vsc_try_fmt_source(void *drv_ctx,
                                   const vsc_mbus_fmt_t *sink_fmt,
                                   vsc_mbus_fmt_t *source_fmt)
{
    crop_vsc_ctx_t *ctx = (crop_vsc_ctx_t *)drv_ctx;
    uint32_t rx, ry, rw, rh;
    crop_get_roi(&ctx->hw, &rx, &ry, &rw, &rh);

    *source_fmt = *sink_fmt;
    if (rw > 0 && rw < source_fmt->width)  source_fmt->width  = rw;
    if (rh > 0 && rh < source_fmt->height) source_fmt->height = rh;

    /* 对齐 */
    if (ctx->hw.align_w > 1)
        source_fmt->width  -= source_fmt->width  % ctx->hw.align_w;
    if (ctx->hw.align_h > 1)
        source_fmt->height -= source_fmt->height % ctx->hw.align_h;
    if (source_fmt->width  == 0) source_fmt->width  = ctx->hw.align_w;
    if (source_fmt->height == 0) source_fmt->height = ctx->hw.align_h;

    return VSC_OK;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  ops.commit_fmt()
 * ═══════════════════════════════════════════════════════════════════════ */

static int crop_vsc_commit_fmt(void *drv_ctx,
                               const vsc_mbus_fmt_t *final_fmt)
{
    crop_vsc_ctx_t *ctx = (crop_vsc_ctx_t *)drv_ctx;
    uint32_t max_w, max_h;
    crop_get_limits(&ctx->hw, &max_w, &max_h);

    if (final_fmt->width  > max_w) return VSC_ERR_COMMIT_FAILED;
    if (final_fmt->height > max_h) return VSC_ERR_COMMIT_FAILED;

    /* 更新 ROI 为 final_fmt 的尺寸，保持 x/y 不变 */
    crop_set_roi(&ctx->hw, ctx->hw.roi_x, ctx->hw.roi_y,
                 final_fmt->width, final_fmt->height);
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
    },
};
