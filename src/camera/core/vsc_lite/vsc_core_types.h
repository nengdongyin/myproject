/**
 * @file    vsc_lite_types.h
 * @brief   VSC Lite 独立类型定义 — 不依赖 vsc/ 目录的任何文件
 *
 * VSC Lite 子系统完全自包含：vsc_lite.h + vsc_lite.c + vsc_ctrl_ids.h + 本文件。
 */

#ifndef VSC_LITE_TYPES_H
#define VSC_LITE_TYPES_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 *  Pixel format constants
 * ======================================================================== */

#define VSC_FMT_RAW8    0x52415738u
#define VSC_FMT_RAW10   0x52415741u
#define VSC_FMT_RAW12   0x52415742u
#define VSC_FMT_RGB888  0x52474238u
#define VSC_FMT_YUV422  0x59555532u
#define VSC_FMT_INVALID 0x00000000u

/* ========================================================================
 *  Capability bits
 * ======================================================================== */

#define VSC_CAP_SENSOR          (1 << 0)
#define VSC_CAP_EXPOSURE_CTRL   (1 << 1)
#define VSC_CAP_STATISTICS      (1 << 2)
#define VSC_CAP_HDR             (1 << 3)
#define VSC_CAP_TRIGGER         (1 << 4)
#define VSC_CAP_CROP            (1 << 5)
#define VSC_CAP_BINNING         (1 << 6)
#define VSC_CAP_FORMAT_CONV     (1 << 7)
#define VSC_CAP_DECIMATION      (1 << 8)

#define VSC_CAP_LOCATION_SENSOR   0
#define VSC_CAP_LOCATION_FPGA     1

/* ========================================================================
 *  Media-bus format descriptor
 * ======================================================================== */

/** @brief 空间/分辨率参数 */
typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t offsetx;         /* ROI 水平起始偏移 (GenICam 规范)    */
    uint32_t offsety;         /* ROI 垂直起始偏移 (GenICam 规范)    */
    uint8_t  bin_x;           /* binning X 因子 (1=无)              */
    uint8_t  bin_y;           /* binning Y 因子 (1=无)              */
    uint8_t  dec_x;           /* decimation X 因子 (1=无)           */
    uint8_t  dec_y;           /* decimation Y 因子 (1=无)           */
    uint32_t pixel_format;
    uint32_t frame_rate_num;
    uint32_t frame_rate_den;
    uint8_t  bit_depth;
    uint8_t  lanes;
} vsc_spatial_fmt_t;

/** @brief 时序参数 */
typedef struct {
    uint32_t pixel_clock_hz;
    uint32_t h_total;
    uint32_t h_active;
    uint32_t h_blank;
    uint32_t v_total;
    uint32_t v_active;
    uint32_t v_blank;
    uint32_t reserved_t[2];
} vsc_timing_fmt_t;

/** @brief 完整 media-bus 格式 = 空间 + 时序 */
typedef struct {
    vsc_spatial_fmt_t spatial;
    vsc_timing_fmt_t  timing;
} vsc_mbus_fmt_t;

#define VSC_MBUS_FMT_INIT { {0, 0, 0, 0, 1, 1, 1, 1, VSC_FMT_INVALID, 0, 1, 0, 0}, {0, 0, 0, 0, 0, 0, 0, {0}} }

static inline bool vsc_fmt_is_valid(const vsc_mbus_fmt_t *f)
{
    return f->spatial.pixel_format != VSC_FMT_INVALID
        && f->spatial.width > 0 && f->spatial.height > 0;
}

/* ========================================================================
 *  Driver ops vtable
 * ======================================================================== */

typedef struct { char key[64]; uint32_t value; } vsc_override_t;

typedef struct {
    uint32_t min_h_total;
    uint32_t min_h_blank;
    uint32_t min_v_total;
    uint32_t min_v_blank;
    uint32_t pipeline_lines;
    uint32_t ip_clock_hz;
    uint32_t reserved[4];
} vsc_timing_req_t;

/** @brief 能力描述符统一头 — query_cap 可用此类型做可用性预检 */
typedef struct { bool available; } vsc_cap_header_t;

typedef struct vsc_ip_ops {
    int (*init)(void *inst);
    int (*try_fmt_sink)(void *drv_ctx, const vsc_mbus_fmt_t *proposed,
                        vsc_mbus_fmt_t *clamped);
    int (*try_fmt_source)(void *drv_ctx, const vsc_mbus_fmt_t *sink_fmt,
                          vsc_mbus_fmt_t *source_fmt);
    int (*commit_fmt)(void *drv_ctx, const vsc_mbus_fmt_t *final_fmt);
    int (*get_timing_req)(void *drv_ctx, const vsc_mbus_fmt_t *sink_fmt,
                          const vsc_mbus_fmt_t *source_fmt,
                          vsc_timing_req_t *req);
    int (*query_cap)(void *drv_ctx, uint32_t cap_id,
                     void *out, uint8_t *out_len);
    int (*set_ctrl)(void *drv_ctx, uint32_t ctrl_id, uint32_t value);
    int (*get_ctrl)(void *drv_ctx, uint32_t ctrl_id, uint32_t *value);
} vsc_ip_ops_t;

/* ========================================================================
 *  Capability structs
 * ======================================================================== */

typedef struct {
    bool     available;
    uint8_t  factor_x;
    uint8_t  factor_y;
    uint8_t  max_factor_x;
    uint8_t  max_factor_y;
    uint8_t  location;
    uint8_t  reserved[2];
} vsc_binning_cap_t;

typedef struct {
    bool     available;
    uint32_t min_w, min_h, max_w, max_h;
    uint32_t cur_w, cur_h;
    uint8_t  align_w, align_h;
    uint8_t  reserved[2];
} vsc_crop_cap_t;

typedef struct {
    bool     available;
    uint32_t max_width, max_height;
    uint32_t pixel_clock_hz;
    uint8_t  num_formats;
    uint8_t  reserved[3];
    uint32_t supported_formats[8];
} vsc_sensor_cap_t;

/* ========================================================================
 *  Transform descriptor (driver descriptor 需要)
 * ======================================================================== */

typedef enum {
    VSC_TRANSFORM_PASS_THROUGH   = 0,
    VSC_TRANSFORM_BINNING        = 1,
    VSC_TRANSFORM_CROP           = 2,
    VSC_TRANSFORM_PIXEL_FMT_CONV = 3,
    VSC_TRANSFORM_DECIMATION     = 4,
    VSC_TRANSFORM_MULTI_STAGE    = 5,
} vsc_fmt_transform_type_t;
#define VSC_MAX_SUB_TRANSFORMS 8
typedef struct vsc_fmt_transform_desc {
    vsc_fmt_transform_type_t type;
    union {
        struct { uint8_t factor_x, factor_y; } binning;
        struct { uint8_t factor_x, factor_y; } decimation;
        struct { uint32_t min_w, min_h, max_w, max_h; uint8_t align_w, align_h; } crop;
        struct { uint32_t fmt_in, fmt_out; } pixel_fmt_conv;
        struct { uint8_t count; const struct vsc_fmt_transform_desc *subs; } multi_stage;
    } params;
} vsc_fmt_transform_desc_t;

/* ========================================================================
 *  Driver descriptor
 * ======================================================================== */

typedef struct vsc_driver {
    const char                    *name;
    uint16_t                       driver_id;
    uint32_t                       capabilities;
    const void                    *schema;
    uint8_t                        prop_count;
    const vsc_fmt_transform_desc_t *transform_template;
    vsc_ip_ops_t                   ops;
} vsc_driver_t;

/* ========================================================================
 *  Negotiation result
 * ======================================================================== */

typedef enum {
    VSC_NEGOTIATE_EXACT    = 0,
    VSC_NEGOTIATE_ADJUSTED = 1,
    VSC_NEGOTIATE_PARTIAL  = 2,
    VSC_NEGOTIATE_FAILED   = 3,
} vsc_negotiation_status_t;

typedef enum {
    VSC_FMT_FIELD_NONE=0, VSC_FMT_FIELD_WIDTH=1, VSC_FMT_FIELD_HEIGHT=2,
    VSC_FMT_FIELD_FORMAT=3, VSC_FMT_FIELD_FRAMERATE=4,
} vsc_fmt_field_t;

typedef enum {
    VSC_ADJUST_NONE=0, VSC_ADJUST_CLAMP=1, VSC_ADJUST_HALVE=2,
    VSC_ADJUST_FORMAT_CONV=3, VSC_ADJUST_REJECTED=4,
} vsc_adjust_reason_t;

#define VSC_MAX_TRACE_ENTRIES 32
typedef struct {
    char entity_name[32]; char pad_name[8];
    vsc_fmt_field_t field_changed; vsc_mbus_fmt_t original, adjusted;
    vsc_adjust_reason_t reason; char reason_detail[64];
} vsc_adjustment_entry_t;

typedef struct {
    uint8_t num_entries;
    vsc_adjustment_entry_t entries[VSC_MAX_TRACE_ENTRIES];
} vsc_adjustment_trace_t;

typedef struct { char entity_name[32]; vsc_mbus_fmt_t final_fmt; bool active; } vsc_endpoint_t;

typedef struct {
    vsc_negotiation_status_t status;
    vsc_mbus_fmt_t           primary_fmt;
    char                     primary_endpoint[32];
    uint8_t                  num_endpoints;
    vsc_endpoint_t           endpoint_fmts[8];
    vsc_adjustment_trace_t   trace;
    vsc_mbus_fmt_t           reachable_max;
} vsc_resolver_result_t;

/* ========================================================================
 *  Error codes
 * ======================================================================== */

#define VSC_OK                         0
#define VSC_ERR_INVALID_INTENT        -1
#define VSC_ERR_UNREACHABLE           -2
#define VSC_ERR_PROPAGATION_SINK      -3
#define VSC_ERR_PROPAGATION_SOURCE    -4
#define VSC_ERR_TOPOLOGY_BROKEN       -5
#define VSC_ERR_NO_ACTIVE_ENDPOINT    -6
#define VSC_ERR_NO_SENSOR             -7
#define VSC_ERR_CANNOT_AUTO_BRIDGE    -8
#define VSC_ERR_BUSY                  -9
#define VSC_ERR_COMMIT_FAILED         -10
#define VSC_ERR_NOT_SUPPORTED         -11
#define VSC_ERR_PARAM                 -12

#ifdef __cplusplus
}
#endif

#endif /* VSC_LITE_TYPES_H */
