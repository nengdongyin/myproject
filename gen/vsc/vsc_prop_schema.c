/* ═══════════════════════════════════════════════════════════
 *  GENERATED FILE — DO NOT EDIT
 *
 *  Compiler:  vsc_prop_gen.py  v1.0.0
 *  File:      vsc_prop_schema.c
 *  Checksum:  0x21B0C029
 *  Generated: 2026-06-07T10:23:13Z
 * ═══════════════════════════════════════════════════════════ */

#include "vsc_types.h"
#include "vsc_prop_ids.h"

/* ═══════════ sensor_imx477 ═══════════ */

const vsc_fmt_transform_desc_t _sensor_imx477_transform = {
    .type = VSC_TRANSFORM_CROP,
    .params.crop = {
        .min_w  = 1,
        .min_h  = 1,
        .max_w  = 4056,
        .max_h  = 3040,
        .align_w = 1,
        .align_h = 1,
    },
};

const vsc_prop_meta_t _sensor_imx477_schema[] = {
    {
        .prop_id     = VSC_PROP_SENSOR_IMX477_MAX_WIDTH,
        .name        = "sensor_imx477.max_width",
        .type        = VSC_TYPE_U32,
        .flags       = 0 | VSC_PROP_READONLY,
        .default_val = { .u32 = 4056 },
        .min_val     = { .u32 = 1 },
        .max_val     = { .u32 = 4056 },
        .max_ref_id  = 0,
    },
    {
        .prop_id     = VSC_PROP_SENSOR_IMX477_MAX_HEIGHT,
        .name        = "sensor_imx477.max_height",
        .type        = VSC_TYPE_U32,
        .flags       = 0 | VSC_PROP_READONLY,
        .default_val = { .u32 = 3040 },
        .min_val     = { .u32 = 1 },
        .max_val     = { .u32 = 3040 },
        .max_ref_id  = 0,
    },
    {
        .prop_id     = VSC_PROP_SENSOR_IMX477_EXPOSURE_US,
        .name        = "sensor_imx477.exposure_us",
        .type        = VSC_TYPE_U32,
        .flags       = 0 | VSC_PROP_RUNTIME,
        .default_val = { .u32 = 10000 },
        .min_val     = { .u32 = 10 },
        .max_val     = { .u32 = 500000 },
        .max_ref_id  = 0,
    },
    {
        .prop_id     = VSC_PROP_SENSOR_IMX477_GAIN_ANALOG,
        .name        = "sensor_imx477.gain_analog",
        .type        = VSC_TYPE_F32,
        .flags       = 0 | VSC_PROP_RUNTIME,
        .default_val = { .f32 = 1.0f },
        .min_val     = { .f32 = 1.0f },
        .max_val     = { .f32 = 16.0f },
        .max_ref_id  = 0,
    },
    {
        .prop_id     = VSC_PROP_SENSOR_IMX477_GAIN_DIGITAL,
        .name        = "sensor_imx477.gain_digital",
        .type        = VSC_TYPE_F32,
        .flags       = 0 | VSC_PROP_RUNTIME,
        .default_val = { .f32 = 1.0f },
        .min_val     = { .f32 = 1.0f },
        .max_val     = { .f32 = 4.0f },
        .max_ref_id  = 0,
    },
    {
        .prop_id     = VSC_PROP_SENSOR_IMX477_FRAME_RATE,
        .name        = "sensor_imx477.frame_rate",
        .type        = VSC_TYPE_U32,
        .flags       = 0 | VSC_PROP_RUNTIME | VSC_PROP_TRANSACTION,
        .default_val = { .u32 = 30 },
        .min_val     = { .u32 = 1 },
        .max_val     = { .u32 = 120 },
        .max_ref_id  = 0,
    },
    {
        .prop_id     = VSC_PROP_SENSOR_IMX477_SUPPORTED_FORMATS,
        .name        = "sensor_imx477.supported_formats",
        .type        = VSC_TYPE_U32,
        .flags       = 0 | VSC_PROP_READONLY,
        .default_val = { .u32 = 0 },
        .min_val     = { .u32 = 0 },
        .max_val     = { .u32 = 0 },
        .max_ref_id  = 0,
    },
};

/* ═══════════ crop ═══════════ */

const vsc_fmt_transform_desc_t _crop_transform = {
    .type = VSC_TRANSFORM_CROP,
    .params.crop = {
        .min_w  = 64,
        .min_h  = 4,
        .max_w  = 8192,
        .max_h  = 8192,
        .align_w = 8,
        .align_h = 8,
    },
};

const vsc_prop_meta_t _crop_schema[] = {
    {
        .prop_id     = VSC_PROP_CROP_MAX_WIDTH,
        .name        = "crop.max_width",
        .type        = VSC_TYPE_U32,
        .flags       = 0 | VSC_PROP_READONLY,
        .default_val = { .u32 = 8192 },
        .min_val     = { .u32 = 64 },
        .max_val     = { .u32 = 8192 },
        .max_ref_id  = 0,
    },
    {
        .prop_id     = VSC_PROP_CROP_MAX_HEIGHT,
        .name        = "crop.max_height",
        .type        = VSC_TYPE_U32,
        .flags       = 0 | VSC_PROP_READONLY,
        .default_val = { .u32 = 8192 },
        .min_val     = { .u32 = 4 },
        .max_val     = { .u32 = 8192 },
        .max_ref_id  = 0,
    },
    {
        .prop_id     = VSC_PROP_CROP_ROI_X,
        .name        = "crop.roi.x",
        .type        = VSC_TYPE_U32,
        .flags       = 0 | VSC_PROP_RUNTIME | VSC_PROP_PERSIST,
        .default_val = { .u32 = 0 },
        .min_val     = { .u32 = 0 },
        .max_val     = { .u32 = 0 },
        .max_ref_id  = VSC_PROP_CROP_MAX_WIDTH,
    },
    {
        .prop_id     = VSC_PROP_CROP_ROI_Y,
        .name        = "crop.roi.y",
        .type        = VSC_TYPE_U32,
        .flags       = 0 | VSC_PROP_RUNTIME | VSC_PROP_PERSIST,
        .default_val = { .u32 = 0 },
        .min_val     = { .u32 = 0 },
        .max_val     = { .u32 = 0 },
        .max_ref_id  = VSC_PROP_CROP_MAX_HEIGHT,
    },
    {
        .prop_id     = VSC_PROP_CROP_ROI_WIDTH,
        .name        = "crop.roi.width",
        .type        = VSC_TYPE_U32,
        .flags       = 0 | VSC_PROP_RUNTIME | VSC_PROP_PERSIST | VSC_PROP_TRANSACTION,
        .default_val = { .u32 = 1920 },
        .min_val     = { .u32 = 64 },
        .max_val     = { .u32 = 0 },
        .max_ref_id  = VSC_PROP_CROP_MAX_WIDTH,
    },
    {
        .prop_id     = VSC_PROP_CROP_ROI_HEIGHT,
        .name        = "crop.roi.height",
        .type        = VSC_TYPE_U32,
        .flags       = 0 | VSC_PROP_RUNTIME | VSC_PROP_PERSIST | VSC_PROP_TRANSACTION,
        .default_val = { .u32 = 1080 },
        .min_val     = { .u32 = 4 },
        .max_val     = { .u32 = 0 },
        .max_ref_id  = VSC_PROP_CROP_MAX_HEIGHT,
    },
};

/* ═══════════ binning ═══════════ */

const vsc_fmt_transform_desc_t _binning_transform = {
    .type = VSC_TRANSFORM_BINNING,
    .params.binning = {
        .factor_x = 2,
        .factor_y = 2,
    },
};

const vsc_prop_meta_t _binning_schema[] = {
    {
        .prop_id     = VSC_PROP_BINNING_FACTOR_X,
        .name        = "binning.factor_x",
        .type        = VSC_TYPE_U32,
        .flags       = 0 | VSC_PROP_RUNTIME,
        .default_val = { .u32 = 2 },
        .min_val     = { .u32 = 1 },
        .max_val     = { .u32 = 4 },
        .max_ref_id  = 0,
    },
    {
        .prop_id     = VSC_PROP_BINNING_FACTOR_Y,
        .name        = "binning.factor_y",
        .type        = VSC_TYPE_U32,
        .flags       = 0 | VSC_PROP_RUNTIME,
        .default_val = { .u32 = 2 },
        .min_val     = { .u32 = 1 },
        .max_val     = { .u32 = 4 },
        .max_ref_id  = 0,
    },
    {
        .prop_id     = VSC_PROP_BINNING_ENABLE,
        .name        = "binning.enable",
        .type        = VSC_TYPE_BOOL,
        .flags       = 0 | VSC_PROP_RUNTIME | VSC_PROP_PERSIST,
        .default_val = { .b = 1 },
        .min_val     = { .b = 0 },
        .max_val     = { .b = 0 },
        .max_ref_id  = 0,
    },
};

/* ═══════════ decoder ═══════════ */

const vsc_fmt_transform_desc_t _decoder_transform = {
    .type = VSC_TRANSFORM_PIXEL_FMT_CONV,
    .params.pixel_fmt_conv = {
        .fmt_in  = VSC_FMT_RAW10,
        .fmt_out = VSC_FMT_RGB888,
    },
};

const vsc_prop_meta_t _decoder_schema[] = {
    {
        .prop_id     = VSC_PROP_DECODER_ENABLE,
        .name        = "decoder.enable",
        .type        = VSC_TYPE_BOOL,
        .flags       = 0 | VSC_PROP_RUNTIME | VSC_PROP_PERSIST,
        .default_val = { .b = 1 },
        .min_val     = { .b = 0 },
        .max_val     = { .b = 0 },
        .max_ref_id  = 0,
    },
    {
        .prop_id     = VSC_PROP_DECODER_FMT_IN,
        .name        = "decoder.fmt_in",
        .type        = VSC_TYPE_U32,
        .flags       = 0 | VSC_PROP_READONLY,
        .default_val = { .u32 = 0 },
        .min_val     = { .u32 = 0 },
        .max_val     = { .u32 = 0 },
        .max_ref_id  = 0,
    },
    {
        .prop_id     = VSC_PROP_DECODER_FMT_OUT,
        .name        = "decoder.fmt_out",
        .type        = VSC_TYPE_U32,
        .flags       = 0 | VSC_PROP_READONLY,
        .default_val = { .u32 = 0 },
        .min_val     = { .u32 = 0 },
        .max_val     = { .u32 = 0 },
        .max_ref_id  = 0,
    },
    {
        .prop_id     = VSC_PROP_DECODER_PATTERN,
        .name        = "decoder.pattern",
        .type        = VSC_TYPE_ENUM,
        .flags       = 0 | VSC_PROP_RUNTIME,
        .default_val = { .u32 = 0 },
        .min_val     = { .u32 = 0 },
        .max_val     = { .u32 = 0 },
        .max_ref_id  = 0,
    },
};

/* ═══════════ histogram ═══════════ */

const vsc_prop_meta_t _histogram_schema[] = {
    {
        .prop_id     = VSC_PROP_HISTOGRAM_ENABLE,
        .name        = "histogram.enable",
        .type        = VSC_TYPE_BOOL,
        .flags       = 0 | VSC_PROP_RUNTIME | VSC_PROP_PERSIST,
        .default_val = { .b = 0 },
        .min_val     = { .b = 0 },
        .max_val     = { .b = 0 },
        .max_ref_id  = 0,
    },
    {
        .prop_id     = VSC_PROP_HISTOGRAM_MAX_BINS,
        .name        = "histogram.max_bins",
        .type        = VSC_TYPE_U32,
        .flags       = 0 | VSC_PROP_READONLY,
        .default_val = { .u32 = 256 },
        .min_val     = { .u32 = 64 },
        .max_val     = { .u32 = 256 },
        .max_ref_id  = 0,
    },
    {
        .prop_id     = VSC_PROP_HISTOGRAM_ACTIVE_BINS,
        .name        = "histogram.active_bins",
        .type        = VSC_TYPE_U32,
        .flags       = 0 | VSC_PROP_RUNTIME,
        .default_val = { .u32 = 64 },
        .min_val     = { .u32 = 64 },
        .max_val     = { .u32 = 0 },
        .max_ref_id  = VSC_PROP_HISTOGRAM_MAX_BINS,
    },
    {
        .prop_id     = VSC_PROP_HISTOGRAM_CHANNELS,
        .name        = "histogram.channels",
        .type        = VSC_TYPE_U32,
        .flags       = 0 | VSC_PROP_READONLY,
        .default_val = { .u32 = 0 },
        .min_val     = { .u32 = 0 },
        .max_val     = { .u32 = 0 },
        .max_ref_id  = 0,
    },
    {
        .prop_id     = VSC_PROP_HISTOGRAM_ACTIVE_CHANNELS,
        .name        = "histogram.active_channels",
        .type        = VSC_TYPE_ENUM,
        .flags       = 0 | VSC_PROP_RUNTIME,
        .default_val = { .u32 = 0 },
        .min_val     = { .u32 = 0 },
        .max_val     = { .u32 = 0 },
        .max_ref_id  = 0,
    },
    {
        .prop_id     = VSC_PROP_HISTOGRAM_WINDOW_X,
        .name        = "histogram.window.x",
        .type        = VSC_TYPE_U32,
        .flags       = 0 | VSC_PROP_RUNTIME,
        .default_val = { .u32 = 0 },
        .min_val     = { .u32 = 0 },
        .max_val     = { .u32 = 8192 },
        .max_ref_id  = 0,
    },
    {
        .prop_id     = VSC_PROP_HISTOGRAM_WINDOW_Y,
        .name        = "histogram.window.y",
        .type        = VSC_TYPE_U32,
        .flags       = 0 | VSC_PROP_RUNTIME,
        .default_val = { .u32 = 0 },
        .min_val     = { .u32 = 0 },
        .max_val     = { .u32 = 8192 },
        .max_ref_id  = 0,
    },
    {
        .prop_id     = VSC_PROP_HISTOGRAM_WINDOW_WIDTH,
        .name        = "histogram.window.width",
        .type        = VSC_TYPE_U32,
        .flags       = 0 | VSC_PROP_RUNTIME,
        .default_val = { .u32 = 1920 },
        .min_val     = { .u32 = 64 },
        .max_val     = { .u32 = 8192 },
        .max_ref_id  = 0,
    },
    {
        .prop_id     = VSC_PROP_HISTOGRAM_WINDOW_HEIGHT,
        .name        = "histogram.window.height",
        .type        = VSC_TYPE_U32,
        .flags       = 0 | VSC_PROP_RUNTIME,
        .default_val = { .u32 = 1080 },
        .min_val     = { .u32 = 4 },
        .max_val     = { .u32 = 8192 },
        .max_ref_id  = 0,
    },
};
