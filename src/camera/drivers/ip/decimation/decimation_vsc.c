/**
 * @file    decimation_vsc.c
 * @brief   Decimation IP 的 VSC 适配器实现。
 *
 * try_fmt_sink:  透传
 * try_fmt_source: 认领 dec_x/dec_y 标记，除以 factor 并坐标变换
 */

#include "decimation_vsc.h"
#include "vsc_driver_ids.h"
#include "vsc_ctrl_ids.h"
#include <string.h>

static int dec_vsc_init(void *inst)
{
    decimation_vsc_inst_t *ctx = (decimation_vsc_inst_t *)inst;
    (void)ctx;
    return VSC_OK;
}

static int dec_vsc_sink(void *drv_ctx, const vsc_mbus_fmt_t *proposed, vsc_mbus_fmt_t *clamped)
{
    (void)drv_ctx;
    *clamped = *proposed;
    return VSC_OK;
}

/* ── try_fmt_source ── */
static int dec_vsc_source(void *drv_ctx, const vsc_mbus_fmt_t *sink, vsc_mbus_fmt_t *src)
{
    (void)drv_ctx;
    uint8_t dx = sink->spatial.dec_x; if (dx < 1) dx = 1;
    uint8_t dy = sink->spatial.dec_y; if (dy < 1) dy = 1;
    *src = *sink;
    if (dx > 1) { src->spatial.width   /= dx; src->spatial.offsetx /= dx; }
    if (dy > 1) { src->spatial.height  /= dy; src->spatial.offsety /= dy; }
    src->spatial.dec_x = 1;
    src->spatial.dec_y = 1;
    return VSC_OK;
}

/* ── commit ── */
static int dec_vsc_commit(void *drv_ctx, const vsc_mbus_fmt_t *final_fmt)
{
    decimation_vsc_inst_t *ctx = (decimation_vsc_inst_t *)drv_ctx;
    (void)final_fmt;
    decimation_enable(&ctx->hw);
    decimation_commit(&ctx->hw);
    return VSC_OK;
}

/* ── set_ctrl / get_ctrl ── */
static int dec_set_ctrl(void *drv_ctx, uint32_t ctrl_id, uint32_t value)
{
    decimation_vsc_inst_t *ctx = (decimation_vsc_inst_t *)drv_ctx;
    switch (ctrl_id) {
    case VSC_CTRL_DEC_FACTOR_X:
        decimation_set_factors(&ctx->hw, (uint8_t)value, ctx->hw.factor_y);
        return VSC_OK;
    case VSC_CTRL_DEC_FACTOR_Y:
        decimation_set_factors(&ctx->hw, ctx->hw.factor_x, (uint8_t)value);
        return VSC_OK;
    default:
        return VSC_ERR_NOT_SUPPORTED;
    }
}

static int dec_get_ctrl(void *drv_ctx, uint32_t ctrl_id, uint32_t *value)
{
    decimation_vsc_inst_t *ctx = (decimation_vsc_inst_t *)drv_ctx;
    switch (ctrl_id) {
    case VSC_CTRL_DEC_FACTOR_X: *value = ctx->hw.factor_x; return VSC_OK;
    case VSC_CTRL_DEC_FACTOR_Y: *value = ctx->hw.factor_y; return VSC_OK;
    default: return VSC_ERR_NOT_SUPPORTED;
    }
}

static const vsc_fmt_transform_desc_t s_dec_template = {
    .type = VSC_TRANSFORM_DECIMATION,
    .params.decimation = { .factor_x = 2, .factor_y = 2 },
};

const vsc_driver_t decimation_vsc_driver = {
    .name = "decimation", .driver_id = VSC_DRIVER_ID_DECIMATION,
    .capabilities = VSC_CAP_DECIMATION,
    .transform_template = &s_dec_template,
    .ops = {
        .init           = dec_vsc_init,
        .try_fmt_sink   = dec_vsc_sink,
        .try_fmt_source = dec_vsc_source,
        .commit_fmt     = dec_vsc_commit,
        .get_timing_req = NULL,
        .query_cap      = NULL,
        .set_ctrl       = dec_set_ctrl,
        .get_ctrl       = dec_get_ctrl,
    },
};
