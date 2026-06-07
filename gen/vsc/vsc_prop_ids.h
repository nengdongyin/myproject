/* ═══════════════════════════════════════════════════════════
 *  GENERATED FILE — DO NOT EDIT
 *
 *  Compiler:  vsc_prop_gen.py  v1.0.0
 *  File:      vsc_prop_ids.h
 *  Checksum:  0x21B0C029
 *  Generated: 2026-06-07T10:23:13Z
 * ═══════════════════════════════════════════════════════════ */

#ifndef VSC_PROP_IDS_H
#define VSC_PROP_IDS_H

/* ── Driver ID constants ── */
#define VSC_DRIVER_ID_SENSOR_IMX477  0x01
#define VSC_DRIVER_ID_CROP  0x03
#define VSC_DRIVER_ID_BINNING  0x04
#define VSC_DRIVER_ID_DECODER  0x05
#define VSC_DRIVER_ID_HISTOGRAM  0x10

/* ── sensor_imx477 ── */
#define _SENSOR_IMX477_PROP_COUNT  7
#define VSC_PROP_SENSOR_IMX477_MAX_WIDTH  ((0x01 << 8) | 0x00)
#define VSC_PROP_SENSOR_IMX477_MAX_HEIGHT  ((0x01 << 8) | 0x01)
#define VSC_PROP_SENSOR_IMX477_EXPOSURE_US  ((0x01 << 8) | 0x02)
#define VSC_PROP_SENSOR_IMX477_GAIN_ANALOG  ((0x01 << 8) | 0x03)
#define VSC_PROP_SENSOR_IMX477_GAIN_DIGITAL  ((0x01 << 8) | 0x04)
#define VSC_PROP_SENSOR_IMX477_FRAME_RATE  ((0x01 << 8) | 0x05)
#define VSC_PROP_SENSOR_IMX477_SUPPORTED_FORMATS  ((0x01 << 8) | 0x06)

/* ── crop ── */
#define _CROP_PROP_COUNT  6
#define VSC_PROP_CROP_MAX_WIDTH  ((0x03 << 8) | 0x00)
#define VSC_PROP_CROP_MAX_HEIGHT  ((0x03 << 8) | 0x01)
#define VSC_PROP_CROP_ROI_X  ((0x03 << 8) | 0x02)
#define VSC_PROP_CROP_ROI_Y  ((0x03 << 8) | 0x03)
#define VSC_PROP_CROP_ROI_WIDTH  ((0x03 << 8) | 0x04)
#define VSC_PROP_CROP_ROI_HEIGHT  ((0x03 << 8) | 0x05)

/* ── binning ── */
#define _BINNING_PROP_COUNT  3
#define VSC_PROP_BINNING_FACTOR_X  ((0x04 << 8) | 0x00)
#define VSC_PROP_BINNING_FACTOR_Y  ((0x04 << 8) | 0x01)
#define VSC_PROP_BINNING_ENABLE  ((0x04 << 8) | 0x02)

/* ── decoder ── */
#define _DECODER_PROP_COUNT  4
#define VSC_PROP_DECODER_ENABLE  ((0x05 << 8) | 0x00)
#define VSC_PROP_DECODER_FMT_IN  ((0x05 << 8) | 0x01)
#define VSC_PROP_DECODER_FMT_OUT  ((0x05 << 8) | 0x02)
#define VSC_PROP_DECODER_PATTERN  ((0x05 << 8) | 0x03)

/* ── histogram ── */
#define _HISTOGRAM_PROP_COUNT  9
#define VSC_PROP_HISTOGRAM_ENABLE  ((0x10 << 8) | 0x00)
#define VSC_PROP_HISTOGRAM_MAX_BINS  ((0x10 << 8) | 0x01)
#define VSC_PROP_HISTOGRAM_ACTIVE_BINS  ((0x10 << 8) | 0x02)
#define VSC_PROP_HISTOGRAM_CHANNELS  ((0x10 << 8) | 0x03)
#define VSC_PROP_HISTOGRAM_ACTIVE_CHANNELS  ((0x10 << 8) | 0x04)
#define VSC_PROP_HISTOGRAM_WINDOW_X  ((0x10 << 8) | 0x05)
#define VSC_PROP_HISTOGRAM_WINDOW_Y  ((0x10 << 8) | 0x06)
#define VSC_PROP_HISTOGRAM_WINDOW_WIDTH  ((0x10 << 8) | 0x07)
#define VSC_PROP_HISTOGRAM_WINDOW_HEIGHT  ((0x10 << 8) | 0x08)

#endif /* VSC_PROP_IDS_H */
