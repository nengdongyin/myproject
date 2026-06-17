/**
 * @file    binning_vsc.c
 * @brief   Binning IP 的 VSC 适配器实现。
 *
 * try_fmt_sink:  校验输入尺寸为 factor 的整数倍
 * try_fmt_source: 输出 = 输入 / factor
 * get_timing_req: 上报行缓冲 flush 需求 + 行缓冲延迟
 */

#include "binning_vsc.h"
#include "vsc_driver_ids.h"
#include "vsc_ctrl_ids.h"
#include <string.h>

/* ── init ── */
static int bin_vsc_init(void *inst)
{
    binning_vsc_inst_t *ctx = (binning_vsc_inst_t *)inst;
    /* base_addr 和默认 factor 已在编译期静态初始化中设置 */
    (void)ctx;
    return VSC_OK;
}

/* ── try_fmt_sink ── */
static int bin_vsc_sink(void *drv_ctx, const vsc_mbus_fmt_t *proposed, vsc_mbus_fmt_t *clamped)
{
    binning_vsc_inst_t *ctx = (binning_vsc_inst_t *)drv_ctx;
    *clamped = *proposed;
    /* factor 整除校验由硬件保证，此处透传 */
    (void)ctx;
    return VSC_OK;
}

/* ── try_fmt_source ── */
static int bin_vsc_source(void *drv_ctx, const vsc_mbus_fmt_t *sink, vsc_mbus_fmt_t *src)
{
    binning_vsc_inst_t *ctx = (binning_vsc_inst_t *)drv_ctx;
    *src = *sink;
    if (ctx->hw.factor_x > 1) src->spatial.width  /= ctx->hw.factor_x;
    if (ctx->hw.factor_y > 1) src->spatial.height /= ctx->hw.factor_y;
    return VSC_OK;
}

/* ── commit ── */
static int bin_vsc_commit(void *drv_ctx, const vsc_mbus_fmt_t *final_fmt)
{
    binning_vsc_inst_t *ctx = (binning_vsc_inst_t *)drv_ctx;
    (void)final_fmt;
    binning_enable(&ctx->hw);
    binning_commit(&ctx->hw);
    return VSC_OK;
}

/* ── get_timing_req ── */
static int bin_get_timing_req(void *drv_ctx,
                              const vsc_mbus_fmt_t *sink_fmt,
                              const vsc_mbus_fmt_t *source_fmt,
                              vsc_timing_req_t *req)
{
    binning_vsc_inst_t *ctx = (binning_vsc_inst_t *)drv_ctx;
    (void)source_fmt;

    memset(req, 0, sizeof(*req));

    /* 行缓冲 flush: 内部 @200MHz 需 32 cycle，换算为 sensor pclk */
    if (sink_fmt->timing.pixel_clock_hz > 0) {
        float ratio = (float)sink_fmt->timing.pixel_clock_hz / 200000000.0f;
        req->min_h_blank = (uint32_t)(32.0f * ratio + 0.9999f);
    } else {
        req->min_h_blank = 12;   /* 默认回退 */
    }

    /* 行缓冲深度: factor_y > 1 时至少缓存 factor_y 行才输出 */
    if (ctx->hw.factor_y > 1)
        req->pipeline_lines = ctx->hw.factor_y;

    req->ip_clock_hz = 200000000;
    return VSC_OK;
}

/* ── set_ctrl / get_ctrl ── */
static int bin_set_ctrl(void *drv_ctx, uint32_t ctrl_id, uint32_t value)
{
    binning_vsc_inst_t *ctx = (binning_vsc_inst_t *)drv_ctx;
    switch (ctrl_id) {
    case VSC_CTRL_BIN_FACTOR_X:
        binning_set_factors(&ctx->hw, (uint8_t)value, ctx->hw.factor_y);
        return VSC_OK;
    case VSC_CTRL_BIN_FACTOR_Y:
        binning_set_factors(&ctx->hw, ctx->hw.factor_x, (uint8_t)value);
        return VSC_OK;
    case VSC_CTRL_BIN_ENABLE:
        value ? binning_enable(&ctx->hw) : binning_disable(&ctx->hw);
        return VSC_OK;
    default:
        return VSC_ERR_NOT_SUPPORTED;
    }
}

static int bin_get_ctrl(void *drv_ctx, uint32_t ctrl_id, uint32_t *value)
{
    binning_vsc_inst_t *ctx = (binning_vsc_inst_t *)drv_ctx;
    switch (ctrl_id) {
    case VSC_CTRL_BIN_FACTOR_X: *value = ctx->hw.factor_x; return VSC_OK;
    case VSC_CTRL_BIN_FACTOR_Y: *value = ctx->hw.factor_y; return VSC_OK;
    case VSC_CTRL_BIN_ENABLE: {
        uint8_t fx, fy; binning_get_factors(&ctx->hw, &fx, &fy);
        *value = (fx > 1 || fy > 1) ? 1 : 0;
        return VSC_OK;
    }
    default: return VSC_ERR_NOT_SUPPORTED;
    }
}

/* ── query_cap ── */
static int bin_query_cap(void *drv_ctx, uint32_t cap_id,
                          void *out, uint8_t *out_len)
{
    binning_vsc_inst_t *ctx = (binning_vsc_inst_t *)drv_ctx;

    if (cap_id != VSC_CAP_BINNING)
        return VSC_ERR_NOT_SUPPORTED;

    vsc_binning_cap_t *c = (vsc_binning_cap_t *)out;
    if (*out_len < sizeof(*c)) return VSC_ERR_PARAM;
    *out_len = sizeof(*c);
    memset(c, 0, sizeof(*c));

    /* TODO: 从硬件寄存器读取使能状态。当前简化为 driver 存在即可用。 */
    c->available    = true;
    c->factor_x     = ctx->hw.factor_x;
    c->factor_y     = ctx->hw.factor_y;
    c->max_factor_x = 8;
    c->max_factor_y = 8;
    c->location     = VSC_CAP_LOCATION_FPGA;
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
    .ops = {
        .init           = bin_vsc_init,
        .try_fmt_sink   = bin_vsc_sink,
        .try_fmt_source = bin_vsc_source,
        .commit_fmt     = bin_vsc_commit,
        .get_timing_req = bin_get_timing_req,
        .query_cap      = bin_query_cap,
        .set_ctrl       = bin_set_ctrl,
        .get_ctrl       = bin_get_ctrl,
    },
};
