/**
 * @file    decoder_vsc.c
 * @brief   Decoder IP 的 VSC 适配器 — RAW→RGB 格式转换。
 *
 * try_fmt_sink:  校验输入是否为支持的 RAW 格式
 * try_fmt_source: 变换 pixel_format RAW→RGB，尺寸不变
 */

#include "decoder_vsc.h"
#include "vsc_driver_ids.h"
#include <string.h>

static int dec_vsc_init(void *inst)
{
    decoder_vsc_inst_t *ctx = (decoder_vsc_inst_t *)inst;
    (void)ctx;
    return VSC_OK;
}

static int dec_vsc_sink(void *drv_ctx, const vsc_mbus_fmt_t *proposed, vsc_mbus_fmt_t *clamped)
{
    decoder_vsc_inst_t *ctx = (decoder_vsc_inst_t *)drv_ctx;
    *clamped = *proposed;
    if (!decoder_supports_input(&ctx->hw, proposed->spatial.pixel_format))
        return VSC_ERR_PROPAGATION_SINK;
    return VSC_OK;
}

static int dec_vsc_source(void *drv_ctx, const vsc_mbus_fmt_t *sink, vsc_mbus_fmt_t *src)
{
    decoder_vsc_inst_t *ctx = (decoder_vsc_inst_t *)drv_ctx;
    *src = *sink;
    src->spatial.pixel_format = ctx->hw.fmt_out;  /* RAW → RGB888 */
    return VSC_OK;
}

static int dec_vsc_commit(void *drv_ctx, const vsc_mbus_fmt_t *final_fmt)
{
    decoder_vsc_inst_t *ctx = (decoder_vsc_inst_t *)drv_ctx;
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
