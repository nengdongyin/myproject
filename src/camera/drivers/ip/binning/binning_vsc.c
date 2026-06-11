/**
 * @file    binning_vsc.c
 * @brief   Binning IP 的 VSC 适配器实现。
 *
 * try_fmt_sink:  校验输入尺寸为 factor 的整数倍
 * try_fmt_source: 输出 = 输入 / factor
 */

#include "binning_vsc.h"
#include "binning_driver.h"
#include "vsc_prop_ids.h"
#include <string.h>

#define BIN_MAX_INSTANCES 4

typedef struct { bool in_use; binning_dev_t hw; } bin_ctx_t;
static bin_ctx_t g_pool[BIN_MAX_INSTANCES];

void binning_vsc_reset(void) { memset(g_pool, 0, sizeof(g_pool)); }

/* ── init ── */
static int bin_vsc_init(void **drv_ctx, uint32_t base_addr,
                        const vsc_override_t *overrides, uint8_t num_over)
{
    bin_ctx_t *ctx = NULL;
    for (int i = 0; i < BIN_MAX_INSTANCES; i++)
        if (!g_pool[i].in_use) { ctx = &g_pool[i]; ctx->in_use = true; break; }
    if (!ctx) return VSC_ERR_TOPOLOGY_BROKEN;

    binning_init(&ctx->hw, base_addr);
    for (uint8_t i = 0; i < num_over; i++) {
        const char *k = overrides[i].key;
        if (strstr(k, "factor_x")) binning_set_factors(&ctx->hw, (uint8_t)overrides[i].value, ctx->hw.factor_y);
        if (strstr(k, "factor_y")) binning_set_factors(&ctx->hw, ctx->hw.factor_x, (uint8_t)overrides[i].value);
    }
    *drv_ctx = ctx;
    return VSC_OK;
}

/* ── try_fmt_sink ── */
static int bin_vsc_sink(void *drv_ctx, const vsc_mbus_fmt_t *proposed, vsc_mbus_fmt_t *clamped)
{
    bin_ctx_t *ctx = (bin_ctx_t *)drv_ctx;
    *clamped = *proposed;
    /* ensure dimensions are divisible by factor (conservative: just pass through) */
    (void)ctx;
    return VSC_OK;
}

/* ── try_fmt_source ── */
static int bin_vsc_source(void *drv_ctx, const vsc_mbus_fmt_t *sink, vsc_mbus_fmt_t *src)
{
    bin_ctx_t *ctx = (bin_ctx_t *)drv_ctx;
    *src = *sink;
    if (ctx->hw.factor_x > 1) src->width  /= ctx->hw.factor_x;
    if (ctx->hw.factor_y > 1) src->height /= ctx->hw.factor_y;
    return VSC_OK;
}

/* ── commit ── */
static int bin_vsc_commit(void *drv_ctx, const vsc_mbus_fmt_t *final_fmt)
{
    bin_ctx_t *ctx = (bin_ctx_t *)drv_ctx;
    (void)final_fmt;
    binning_enable(&ctx->hw);
    binning_commit(&ctx->hw);
    return VSC_OK;
}

static const vsc_fmt_transform_desc_t s_bin_template = {
    .type = VSC_TRANSFORM_BINNING,
    .params.binning = { .factor_x = 2, .factor_y = 2 },
};

const vsc_driver_t binning_vsc_driver = {
    .name = "binning", .driver_id = VSC_DRIVER_ID_BINNING,
    .capabilities = VSC_CAP_BINNING,
    .transform_template = &s_bin_template,
    .ops = { bin_vsc_init, bin_vsc_sink, bin_vsc_source, bin_vsc_commit },
};
