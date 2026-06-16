/**
 * @file    decoder_vsc.c
 * @brief   Decoder IP 的 VSC 适配器 — RAW→RGB 格式转换。
 *
 * try_fmt_sink:  校验输入是否为支持的 RAW 格式
 * try_fmt_source: 变换 pixel_format RAW→RGB，尺寸不变
 */

#include "decoder_vsc.h"
#include "decoder_driver.h"
#include "vsc_driver_ids.h"
#include <string.h>

#define DEC_MAX_INSTANCES 4

typedef struct { bool in_use; decoder_dev_t hw; } dec_ctx_t;
static dec_ctx_t g_pool[DEC_MAX_INSTANCES];

void decoder_vsc_reset(void) { memset(g_pool, 0, sizeof(g_pool)); }

static int dec_vsc_init(void **drv_ctx, uint32_t base_addr,
                        const vsc_override_t *overrides, uint8_t num_over)
{
    dec_ctx_t *ctx = NULL;
    for (int i = 0; i < DEC_MAX_INSTANCES; i++)
        if (!g_pool[i].in_use) { ctx = &g_pool[i]; ctx->in_use = true; break; }
    if (!ctx) return VSC_ERR_TOPOLOGY_BROKEN;

    decoder_init(&ctx->hw, base_addr);
    (void)overrides; (void)num_over;
    *drv_ctx = ctx;
    return VSC_OK;
}

static int dec_vsc_sink(void *drv_ctx, const vsc_mbus_fmt_t *proposed, vsc_mbus_fmt_t *clamped)
{
    dec_ctx_t *ctx = (dec_ctx_t *)drv_ctx;
    *clamped = *proposed;
    if (!decoder_supports_input(&ctx->hw, proposed->pixel_format))
        return VSC_ERR_PROPAGATION_SINK;
    return VSC_OK;
}

static int dec_vsc_source(void *drv_ctx, const vsc_mbus_fmt_t *sink, vsc_mbus_fmt_t *src)
{
    dec_ctx_t *ctx = (dec_ctx_t *)drv_ctx;
    *src = *sink;
    src->pixel_format = ctx->hw.fmt_out;  /* RAW → RGB888 */
    return VSC_OK;
}

static int dec_vsc_commit(void *drv_ctx, const vsc_mbus_fmt_t *final_fmt)
{
    dec_ctx_t *ctx = (dec_ctx_t *)drv_ctx;
    (void)final_fmt;
    decoder_enable(&ctx->hw);
    decoder_commit(&ctx->hw);
    return VSC_OK;
}

static const vsc_fmt_transform_desc_t s_dec_template = {
    .type = VSC_TRANSFORM_PIXEL_FMT_CONV,
    .params.pixel_fmt_conv = { .fmt_in = VSC_FMT_RAW10, .fmt_out = VSC_FMT_RGB888 },
};

const vsc_driver_t decoder_vsc_driver = {
    .name = "decoder", .driver_id = VSC_DRIVER_ID_DECODER,
    .capabilities = VSC_CAP_FORMAT_CONV,
    .transform_template = &s_dec_template,
    .ops = {
        .init           = dec_vsc_init,
        .try_fmt_sink   = dec_vsc_sink,
        .try_fmt_source = dec_vsc_source,
        .commit_fmt     = dec_vsc_commit,
        .get_timing_req = NULL,
        .query_cap      = NULL,
        .set_ctrl       = NULL,
        .get_ctrl       = NULL,
    },
};
