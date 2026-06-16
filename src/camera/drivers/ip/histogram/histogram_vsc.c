/**
 * @file    histogram_vsc.c
 * @brief   Histogram IP VSC 适配器 — ANALYZER（TAP observer）。
 *
 * 只有 try_fmt_sink（被动校验），无 try_fmt_source / commit_fmt。
 */

#include "histogram_vsc.h"
#include "histogram_driver.h"
#include "vsc_driver_ids.h"
#include <string.h>

#define HIST_MAX_INSTANCES 4

typedef struct { bool in_use; histogram_dev_t hw; } hist_ctx_t;
static hist_ctx_t g_pool[HIST_MAX_INSTANCES];

void histogram_vsc_reset(void) { memset(g_pool, 0, sizeof(g_pool)); }

static int hist_vsc_init(void **drv_ctx, uint32_t base_addr,
                         const vsc_override_t *overrides, uint8_t num_over)
{
    hist_ctx_t *ctx = NULL;
    for (int i = 0; i < HIST_MAX_INSTANCES; i++)
        if (!g_pool[i].in_use) { ctx = &g_pool[i]; ctx->in_use = true; break; }
    if (!ctx) return VSC_ERR_TOPOLOGY_BROKEN;

    histogram_init(&ctx->hw, base_addr);
    (void)overrides; (void)num_over;
    *drv_ctx = ctx;
    return VSC_OK;
}

static int hist_vsc_sink(void *drv_ctx, const vsc_mbus_fmt_t *proposed, vsc_mbus_fmt_t *clamped)
{
    hist_ctx_t *ctx = (hist_ctx_t *)drv_ctx;
    *clamped = *proposed;
    if (!histogram_supports_format(&ctx->hw, proposed->pixel_format))
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
