/**
 * @file    sensor_vsc.c
 * @brief   图像传感器 VSC 适配器 — 桥接 ISC (Image Sensor Controller) 框架。
 *
 * 通过 isc_bridge_init() 初始化 ISC，调用 isc_open/isc_try_fmt/isc_set_fmt。
 * 在 try_fmt_source 中填充时序基准值，在 commit_fmt 中下发时序到硬件。
 */

#include "sensor_vsc.h"
#include "isc_bridge.h"
#include "isc.h"
#include "vsc_driver_ids.h"
#include "vsc_ctrl_ids.h"
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════
 *  格式转换 helper: vsc_mbus_fmt_t ↔ isc_fmt_t
 *
 *  ISC 使用 Bayer 顺序编码 (e.g. SRGGB10 = 'RG10'),
 *  VSC 使用通用 RAW 编码 (e.g. RAW10).
 *  转换时需在两套格式空间之间映射。
 * ═══════════════════════════════════════════════════════════════════════ */

static uint32_t vsc_pixfmt_to_isc(uint32_t vsc_fmt)
{
    switch (vsc_fmt) {
    case VSC_FMT_RAW8:  return ISC_PIX_FMT_SRGGB8;
    case VSC_FMT_RAW10: return ISC_PIX_FMT_SRGGB10;
    case VSC_FMT_RAW12: return ISC_PIX_FMT_SRGGB12;
    default:            return vsc_fmt;  /* RGB888 等非 RAW 格式直接透传 */
    }
}

static uint32_t isc_pixfmt_to_vsc(uint32_t isc_fmt)
{
    switch (isc_fmt) {
    case ISC_PIX_FMT_SRGGB8:
    case ISC_PIX_FMT_SBGGR8:
    case ISC_PIX_FMT_SGRBG8:
    case ISC_PIX_FMT_SGBRG8:
    case ISC_PIX_FMT_GREY8:
        return VSC_FMT_RAW8;
    case ISC_PIX_FMT_SRGGB10:
    case ISC_PIX_FMT_SBGGR10:
    case ISC_PIX_FMT_SGRBG10:
    case ISC_PIX_FMT_SGBRG10:
    case ISC_PIX_FMT_GREY10:
        return VSC_FMT_RAW10;
    case ISC_PIX_FMT_SRGGB12:
    case ISC_PIX_FMT_SBGGR12:
        return VSC_FMT_RAW12;
    default:
        return isc_fmt;  /* RGB888 等非 Bayer 格式直接透传 */
    }
}

static void vsc_to_isc(const vsc_mbus_fmt_t *v, isc_fmt_t *i)
{
    memset(i, 0, sizeof(*i));
    i->width          = v->spatial.width;
    i->height         = v->spatial.height;
    i->pixel_format   = vsc_pixfmt_to_isc(v->spatial.pixel_format);
    i->frame_rate_num = v->spatial.frame_rate_num;
    i->frame_rate_den = v->spatial.frame_rate_den;
    i->bit_depth      = v->spatial.bit_depth;
}

static void isc_to_vsc(const isc_fmt_t *i, vsc_mbus_fmt_t *v)
{
    v->spatial.width          = i->width;
    v->spatial.height         = i->height;
    v->spatial.pixel_format   = isc_pixfmt_to_vsc(i->pixel_format);
    v->spatial.frame_rate_num = i->frame_rate_num;
    v->spatial.frame_rate_den = i->frame_rate_den;
    v->spatial.bit_depth      = i->bit_depth;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  私有上下文
 * ═══════════════════════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════════════════════
 *  ops.init() — 实例由应用层预分配
 * ═══════════════════════════════════════════════════════════════════════ */

static int sensor_vsc_init(void *inst)
{
    sensor_vsc_inst_t *ctx = (sensor_vsc_inst_t *)inst;

    int rc = isc_bridge_init();
    if (rc != 0) return VSC_ERR_CANNOT_AUTO_BRIDGE;

    if (ctx->model[0] == '\0')
        strcpy(ctx->model, "sensor_imx477");

    rc = isc_open(ctx->model, &ctx->isc_dev);
    if (rc != ISC_OK) {
        ctx->isc_dev = NULL;
        rc = VSC_OK;
    }

    return VSC_OK;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  ops.try_fmt_source()
 *
 *  阶段 A 职责: 协商空间格式 + 填充时序基准值
 * ======================================================================= */

static int sensor_vsc_try_fmt_source(void *drv_ctx,
                                     const vsc_mbus_fmt_t *intent,
                                     vsc_mbus_fmt_t *source_fmt)
{
    sensor_vsc_inst_t *ctx = (sensor_vsc_inst_t *)drv_ctx;

    if (ctx->isc_dev) {
        isc_fmt_t isc_in;
        vsc_to_isc(intent, &isc_in);
        /* 应用缓存的 binning 控制值 */
        if (ctx->bin_enabled) {
            isc_in.reduction_x    = ctx->bin_factor_x;
            isc_in.reduction_y    = ctx->bin_factor_y;
            isc_in.reduction_mode = ISC_REDUCE_BIN_SUM;
        }
        int rc = isc_try_fmt(ctx->isc_dev, &isc_in);
        if (rc != ISC_OK) return VSC_ERR_PROPAGATION_SOURCE;
        isc_to_vsc(&isc_in, source_fmt);

        /* 填充时序基准值 */
        isc_timing_t timing;
        rc = isc_try_timing(ctx->isc_dev, &isc_in, &timing);
        if (rc == ISC_OK) {
            source_fmt->timing.pixel_clock_hz = timing.pixel_clock_hz;
            source_fmt->timing.h_active       = source_fmt->spatial.width;
            source_fmt->timing.h_total        = timing.line_length_pclk;
            source_fmt->timing.h_blank        = source_fmt->timing.h_total - source_fmt->timing.h_active;
            source_fmt->timing.v_active       = source_fmt->spatial.height;
            source_fmt->timing.v_total        = timing.frame_length_lines;
            source_fmt->timing.v_blank        = source_fmt->timing.v_total - source_fmt->timing.v_active;

            /* 缓存传感器上限（后续聚合阶段检查用） */
            ctx->max_h_total = timing.line_length_pclk * 4;   /* 保守: 4× 基准 */
            ctx->max_v_total = timing.frame_length_lines * 4;
        } else {
            /* 回退: ISC 不支持 try_timing，用默认值 */
            source_fmt->timing.pixel_clock_hz = 74250000;   /* IMX477 典型值 */
            source_fmt->timing.h_active       = source_fmt->spatial.width;
            source_fmt->timing.h_total        = source_fmt->spatial.width + 128;
            source_fmt->timing.h_blank        = 128;
            source_fmt->timing.v_active       = source_fmt->spatial.height;
            source_fmt->timing.v_total        = source_fmt->spatial.height + 80;
            source_fmt->timing.v_blank        = 80;
            ctx->max_h_total = 0xFFFF;
            ctx->max_v_total = 0xFFFF;
        }
        ctx->bin_factor_x = 1;
        ctx->bin_factor_y = 1;
        ctx->bin_enabled  = false;
    } else {
        /* 回退：内置格式表 */
        *source_fmt = *intent;
        uint32_t supported[] = { VSC_FMT_RAW8, VSC_FMT_RAW10, VSC_FMT_RAW12, 0 };
        bool ok = false;
        for (int i = 0; supported[i]; i++)
            if (supported[i] == intent->spatial.pixel_format) { ok = true; break; }
        if (!ok) source_fmt->spatial.pixel_format = supported[0];
        if (source_fmt->spatial.width  > 4056) source_fmt->spatial.width  = 4056;
        if (source_fmt->spatial.height > 3040) source_fmt->spatial.height = 3040;

        /* 回退时序 */
        source_fmt->timing.pixel_clock_hz = 74250000;
        source_fmt->timing.h_active       = source_fmt->spatial.width;
        source_fmt->timing.h_total        = source_fmt->spatial.width + 128;
        source_fmt->timing.h_blank        = 128;
        source_fmt->timing.v_active       = source_fmt->spatial.height;
        source_fmt->timing.v_total        = source_fmt->spatial.height + 80;
        source_fmt->timing.v_blank        = 80;
        ctx->max_h_total = 0xFFFF;
        ctx->max_v_total = 0xFFFF;
    }
    return VSC_OK;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  ops.get_timing_req() — 阶段 B
 *
 *  Sensor 自身通常无额外时序需求，但通过 reserved 上报物理上限，
 *  供框架在聚合阶段检查是否超出传感器能力。
 * ======================================================================= */

static int sensor_get_timing_req(void *drv_ctx,
                                 const vsc_mbus_fmt_t *sink_fmt,
                                 const vsc_mbus_fmt_t *source_fmt,
                                 vsc_timing_req_t *req)
{
    sensor_vsc_inst_t *ctx = (sensor_vsc_inst_t *)drv_ctx;
    (void)sink_fmt;
    (void)source_fmt;

    memset(req, 0, sizeof(*req));

    /* 通过 reserved 上报传感器物理上限 */
    req->reserved[0] = ctx->max_h_total;
    req->reserved[1] = ctx->max_v_total;

    return VSC_OK;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  ops.query_cap()
 * ======================================================================= */

static int sensor_query_cap(void *drv_ctx, uint32_t cap_id,
                             void *out, uint8_t *out_len)
{
    sensor_vsc_inst_t *ctx = (sensor_vsc_inst_t *)drv_ctx;

    switch (cap_id) {
    case VSC_CAP_BINNING: {
        vsc_binning_cap_t *c = (vsc_binning_cap_t *)out;
        if (*out_len < sizeof(*c)) return VSC_ERR_PARAM;
        *out_len = sizeof(*c);
        memset(c, 0, sizeof(*c));
        c->available    = ctx->bin_enabled;
        c->factor_x     = ctx->bin_factor_x;
        c->factor_y     = ctx->bin_factor_y;
        c->max_factor_x = 4;
        c->max_factor_y = 4;
        c->location     = VSC_CAP_LOCATION_SENSOR;
        return VSC_OK;
    }
    case VSC_CAP_SENSOR: {
        vsc_sensor_cap_t *c = (vsc_sensor_cap_t *)out;
        if (*out_len < sizeof(*c)) return VSC_ERR_PARAM;
        *out_len = sizeof(*c);
        memset(c, 0, sizeof(*c));
        c->available       = true;
        c->max_width       = 4056;
        c->max_height      = 3040;
        c->pixel_clock_hz  = (ctx->isc_dev) ? 74250000 : 0;
        c->num_formats     = 3;
        c->supported_formats[0] = VSC_FMT_RAW8;
        c->supported_formats[1] = VSC_FMT_RAW10;
        c->supported_formats[2] = VSC_FMT_RAW12;
        return VSC_OK;
    }
    default:
        return VSC_ERR_NOT_SUPPORTED;
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 *  ops.set_ctrl / get_ctrl — 桥接 VSC 控制 ID 到 ISC
 * ======================================================================= */

static int sensor_set_ctrl(void *drv_ctx, uint32_t ctrl_id, uint32_t value)
{
    sensor_vsc_inst_t *ctx = (sensor_vsc_inst_t *)drv_ctx;
    if (!ctx->isc_dev) return VSC_ERR_NOT_SUPPORTED;

    switch (ctrl_id) {
    case VSC_CTRL_BIN_FACTOR_X:
        ctx->bin_factor_x = (uint8_t)value;
        return VSC_OK;
    case VSC_CTRL_BIN_FACTOR_Y:
        ctx->bin_factor_y = (uint8_t)value;
        return VSC_OK;
    case VSC_CTRL_BIN_ENABLE:
        ctx->bin_enabled = (value != 0);
        return VSC_OK;
    case VSC_CTRL_SENSOR_EXPOSURE:
        return isc_set_ctrl(ctx->isc_dev, ISC_CID_EXPOSURE,
                            (isc_ctrl_value_t){.i64 = (int64_t)value});
    case VSC_CTRL_SENSOR_GAIN:
        return isc_set_ctrl(ctx->isc_dev, ISC_CID_ANALOG_GAIN,
                            (isc_ctrl_value_t){.i64 = (int64_t)value});
    default:
        return VSC_ERR_NOT_SUPPORTED;
    }
}

static int sensor_get_ctrl(void *drv_ctx, uint32_t ctrl_id, uint32_t *value)
{
    sensor_vsc_inst_t *ctx = (sensor_vsc_inst_t *)drv_ctx;
    if (!ctx->isc_dev) return VSC_ERR_NOT_SUPPORTED;

    switch (ctrl_id) {
    case VSC_CTRL_BIN_FACTOR_X:  *value = ctx->bin_factor_x;  return VSC_OK;
    case VSC_CTRL_BIN_FACTOR_Y:  *value = ctx->bin_factor_y;  return VSC_OK;
    case VSC_CTRL_BIN_ENABLE:    *value = ctx->bin_enabled;   return VSC_OK;
    case VSC_CTRL_SENSOR_EXPOSURE: {
        isc_ctrl_value_t v;
        int rc = isc_get_ctrl(ctx->isc_dev, ISC_CID_EXPOSURE, &v);
        if (rc == ISC_OK) *value = (uint32_t)v.i64;
        return (rc == ISC_OK) ? VSC_OK : VSC_ERR_NOT_SUPPORTED;
    }
    case VSC_CTRL_SENSOR_GAIN: {
        isc_ctrl_value_t v;
        int rc = isc_get_ctrl(ctx->isc_dev, ISC_CID_ANALOG_GAIN, &v);
        if (rc == ISC_OK) *value = (uint32_t)v.i64;
        return (rc == ISC_OK) ? VSC_OK : VSC_ERR_NOT_SUPPORTED;
    }
    default:
        return VSC_ERR_NOT_SUPPORTED;
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 *  ops.commit_fmt()
 *
 *  提交空间格式到 ISC；时序字段（h_total / v_total）由 ISC 自动计算。
 *  若 final_fmt 携带了收紧后的时序约束（如 h_total > ISC 默认值），
 *  尝试通过 isc_set_fmt 间接生效（ISC 驱动内核对齐到合法范围）。
 * ======================================================================= */

static int sensor_vsc_commit_fmt(void *drv_ctx,
                                 const vsc_mbus_fmt_t *final_fmt)
{
    sensor_vsc_inst_t *ctx = (sensor_vsc_inst_t *)drv_ctx;
    if (ctx->isc_dev) {
        isc_fmt_t isc_fmt;
        vsc_to_isc(final_fmt, &isc_fmt);
        int rc = isc_set_fmt(ctx->isc_dev, &isc_fmt);
        return (rc == ISC_OK) ? VSC_OK : VSC_ERR_COMMIT_FAILED;
    }
    (void)final_fmt;
    return VSC_OK;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Transform template（供 Phase 1 可行性检查使用）
 * ═══════════════════════════════════════════════════════════════════════ */

static const vsc_fmt_transform_desc_t s_sensor_template = {
    .type = VSC_TRANSFORM_CROP,
    .params.crop = { .min_w = 1, .min_h = 1, .max_w = 4056, .max_h = 3040 },
};

/* ═══════════════════════════════════════════════════════════════════════
 *  Driver 实例
 * ═══════════════════════════════════════════════════════════════════════ */

const vsc_driver_t sensor_imx477_vsc_driver = {
    .name         = "sensor_imx477",
    .driver_id    = VSC_DRIVER_ID_SENSOR_IMX477,
    .capabilities = VSC_CAP_SENSOR | VSC_CAP_EXPOSURE_CTRL,
    .transform_template = &s_sensor_template,
    .ops = {
        .init           = sensor_vsc_init,
        .try_fmt_sink   = NULL,
        .try_fmt_source = sensor_vsc_try_fmt_source,
        .commit_fmt     = sensor_vsc_commit_fmt,
        .get_timing_req = sensor_get_timing_req,
        .query_cap      = sensor_query_cap,
        .set_ctrl       = sensor_set_ctrl,
        .get_ctrl       = sensor_get_ctrl,
    },
};
