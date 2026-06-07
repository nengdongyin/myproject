/* ═══════════════════════════════════════════════════════════
 *  GENERATED FILE — DO NOT EDIT
 *
 *  Compiler:  vsc_prop_gen.py  v1.0.0
 *  File:      vsc_driver_registry.c
 *  Checksum:  0x21B0C029
 *  Generated: 2026-06-07T10:23:13Z
 * ═══════════════════════════════════════════════════════════ */

#include "vsc_types.h"
#include "vsc_prop_ids.h"

/* external schema + transform declarations */
extern const vsc_prop_meta_t _sensor_imx477_schema[];
extern const vsc_fmt_transform_desc_t _sensor_imx477_transform;
extern const vsc_prop_meta_t _crop_schema[];
extern const vsc_fmt_transform_desc_t _crop_transform;
extern const vsc_prop_meta_t _binning_schema[];
extern const vsc_fmt_transform_desc_t _binning_transform;
extern const vsc_prop_meta_t _decoder_schema[];
extern const vsc_fmt_transform_desc_t _decoder_transform;
extern const vsc_prop_meta_t _histogram_schema[];

/* ── driver registry ── */
const vsc_driver_t _vsc_drivers[] = {
    {
        .name               = "sensor_imx477",
        .driver_id          = VSC_DRIVER_ID_SENSOR_IMX477,
        .capabilities       = 0 | VSC_CAP_SENSOR | VSC_CAP_EXPOSURE_CTRL,
        .schema             = _sensor_imx477_schema,
        .prop_count         = _SENSOR_IMX477_PROP_COUNT,
        .transform_template = &_sensor_imx477_transform,
        .ops                = { NULL, NULL, NULL, NULL },
    },
    {
        .name               = "crop",
        .driver_id          = VSC_DRIVER_ID_CROP,
        .capabilities       = 0 | VSC_CAP_CROP,
        .schema             = _crop_schema,
        .prop_count         = _CROP_PROP_COUNT,
        .transform_template = &_crop_transform,
        .ops                = { NULL, NULL, NULL, NULL },
    },
    {
        .name               = "binning",
        .driver_id          = VSC_DRIVER_ID_BINNING,
        .capabilities       = 0 | VSC_CAP_BINNING,
        .schema             = _binning_schema,
        .prop_count         = _BINNING_PROP_COUNT,
        .transform_template = &_binning_transform,
        .ops                = { NULL, NULL, NULL, NULL },
    },
    {
        .name               = "decoder",
        .driver_id          = VSC_DRIVER_ID_DECODER,
        .capabilities       = 0 | VSC_CAP_FORMAT_CONV,
        .schema             = _decoder_schema,
        .prop_count         = _DECODER_PROP_COUNT,
        .transform_template = &_decoder_transform,
        .ops                = { NULL, NULL, NULL, NULL },
    },
    {
        .name               = "histogram",
        .driver_id          = VSC_DRIVER_ID_HISTOGRAM,
        .capabilities       = 0 | VSC_CAP_STATISTICS,
        .schema             = _histogram_schema,
        .prop_count         = _HISTOGRAM_PROP_COUNT,
        .transform_template = NULL,
        .ops                = { NULL, NULL, NULL, NULL },
    },
    { NULL, 0, 0, NULL, 0, NULL, { NULL, NULL, NULL, NULL } },
};

