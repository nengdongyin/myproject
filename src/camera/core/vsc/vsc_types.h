/**
 * @file    vsc_types.h
 * @brief   VSC 完整版类型 — pipeline 图拓扑、可行性检查、调整追踪
 *
 * 核心类型（vsc_mbus_fmt_t, vsc_ip_ops_t, vsc_driver_t 等）来自
 * vsc_lite/vsc_core_types.h。本文件仅补充 VSC pipeline 独有类型。
 */

#ifndef VSC_TYPES_H
#define VSC_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include "vsc_core_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 *  VSC 独有 — 编译期上限
 * ======================================================================== */

#define VSC_MAX_ENTITIES         16
#define VSC_MAX_LINKS            32
#define VSC_MAX_ADJ               8
#define VSC_MAX_PADS_PER_ENTITY   4
#define VSC_MAX_TRACE_ENTRIES    32
#define VSC_MAX_ENDPOINTS         8
#define VSC_MAX_ENTITY_NAME      32
#define VSC_MAX_ADJ               8
#define VSC_MAX_PADS_PER_ENTITY   4

/* ── alias for VSC code compatibility ── */
typedef vsc_endpoint_t vsc_endpoint_fmt_t;

static inline bool vsc_fmt_equal(const vsc_mbus_fmt_t *a, const vsc_mbus_fmt_t *b) {
    return a->width == b->width && a->height == b->height
        && a->pixel_format == b->pixel_format
        && a->frame_rate_num == b->frame_rate_num
        && a->frame_rate_den == b->frame_rate_den
        && a->bit_depth == b->bit_depth && a->lanes == b->lanes;
}

/* ========================================================================
 *  Entity / Link
 * ======================================================================== */

typedef enum {
    VSC_ENTITY_SENSOR=0, VSC_ENTITY_STREAM=1, VSC_ENTITY_ANALYZER=2, VSC_ENTITY_ENDPOINT=3,
} vsc_entity_class_t;

typedef enum { VSC_LINK_STREAM=0, VSC_LINK_TAP=1 } vsc_link_type_t;

typedef struct { uint8_t src_entity, dst_entity; vsc_link_type_t type; uint8_t flags, reserved; } vsc_link_t;
#define VSC_LINK_FLAG_ENABLED 0x01

typedef struct { vsc_mbus_fmt_t sink_fmt, source_fmt; bool visited, active; } vsc_propagation_state_t;

typedef struct vsc_entity {
    char name[VSC_MAX_ENTITY_NAME];
    vsc_entity_class_t entity_class;
    vsc_fmt_transform_desc_t transform_desc;
    vsc_propagation_state_t prop_state;
    const vsc_ip_ops_t *ops;
    void *drv_ctx;
} vsc_entity_t;

/* ========================================================================
 *  可行性约束
 * ======================================================================== */

typedef struct { uint32_t lo; uint32_t hi; } vsc_interval_t;
static inline bool vsc_interval_is_empty(const vsc_interval_t *iv) { return iv->lo > iv->hi; }

typedef struct {
    vsc_interval_t width_range, height_range;
    uint32_t required_format, frame_rate_num, frame_rate_den;
} vsc_feasibility_constraint_t;

/* ========================================================================
 *  Pipeline
 * ======================================================================== */

typedef enum { VSC_PIPELINE_UNCONFIGURED=0, VSC_PIPELINE_CONFIGURED=1, VSC_PIPELINE_STREAMING=2, VSC_PIPELINE_DIRTY=3 } vsc_pipeline_state_t;

typedef struct {
    uint8_t num_entities; vsc_entity_t entities[VSC_MAX_ENTITIES];
    uint8_t num_links; vsc_link_t links[VSC_MAX_LINKS];
    uint8_t execution_order[VSC_MAX_ENTITIES], num_exec_order;
    uint8_t tap_observers[VSC_MAX_ENTITIES], num_taps;
    uint8_t endpoints[VSC_MAX_ENDPOINTS], num_endpoints;
    uint8_t adj_count[VSC_MAX_ENTITIES];
    uint8_t adj_dst[VSC_MAX_ENTITIES][VSC_MAX_ADJ];
    vsc_link_type_t adj_type[VSC_MAX_ENTITIES][VSC_MAX_ADJ];
    vsc_pipeline_state_t state;
} vsc_pipeline_t;

/* ========================================================================
 *  Property metadata (VSC codegen 使用)
 * ======================================================================== */

#define VSC_PROP_READONLY     (1 << 0)
#define VSC_PROP_RUNTIME      (1 << 1)
#define VSC_PROP_PERSIST      (1 << 2)
#define VSC_PROP_TRANSACTION  (1 << 3)
#define VSC_PROP_ATOMIC       (1 << 4)
#define VSC_PROP_LAZY         (1 << 5)
#define VSC_PROP_DEBUG        (1 << 6)

typedef enum { VSC_TYPE_U32=0, VSC_TYPE_I32, VSC_TYPE_F32, VSC_TYPE_BOOL, VSC_TYPE_ENUM, VSC_TYPE_STRING } vsc_prop_type_t;
typedef union { uint32_t u32; int32_t i32; float f32; bool b; const char *str; } vsc_prop_val_t;
typedef struct { uint16_t prop_id; const char *name; vsc_prop_type_t type; uint8_t flags; vsc_prop_val_t default_val, min_val, max_val; uint16_t max_ref_id; } vsc_prop_meta_t;
typedef struct { uint16_t prop_id; uint16_t ref_id; } vsc_prop_dep_t;

#ifdef __cplusplus
}
#endif
#endif /* VSC_TYPES_H */
