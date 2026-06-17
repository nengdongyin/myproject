/**
 * @file    histogram_vsc.c
 * @brief   Histogram IP VSC 适配器 — ANALYZER（TAP observer）。
 *
 * 只有 try_fmt_sink（被动校验），无 try_fmt_source / commit_fmt。
 */

#include "histogram_vsc.h"
#include "vsc_driver_ids.h"
#include <string.h>

static int hist_vsc_init(void *inst)
{
    histogram_vsc_inst_t *ctx = (histogram_vsc_inst_t *)inst;
    (void)ctx;
    return VSC_OK;
}

static int hist_vsc_sink(void *drv_ctx, const vsc_mbus_fmt_t *proposed, vsc_mbus_fmt_t *clamped)
{
    histogram_vsc_inst_t *ctx = (histogram_vsc_inst_t *)drv_ctx;
    *clamped = *proposed;
    if (!histogram_supports_format(&ctx->hw, proposed->spatial.pixel_format))
        return VSC_ERR_PROPAGATION_SINK;
    return VSC_OK;
}

/* ANALYZER — no try_fmt_source, no commit_fmt */

const vsc_driver_t histogram_vsc_driver = {
    .name = "histogram", .driver_id = VSC_DRIVER_ID_HISTOGRAM,
    .capabilities = VSC_CAP_STATISTICS,
    .ops = {
        .init           = hist_vsc_init,
        .try_fmt_sink   = hist_vsc_sink,
        .try_fmt_source = NULL,
        .commit_fmt     = NULL,
        .get_timing_req = NULL,
        .query_cap      = NULL,
        .set_ctrl       = NULL,
        .get_ctrl       = NULL,
    },
};
