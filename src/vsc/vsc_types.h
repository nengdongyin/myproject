/**
 * @file    vsc_types.h
 * @brief   VSC (Video Stream Controller) core type definitions.
 *
 * S0 memory strategy — all structures are fixed-size, no dynamic allocation.
 * This file defines the format descriptor, transform descriptor, entity/link/
 * pipeline graph, and resolver result types consumed by the Property Resolver.
 */

#ifndef VSC_TYPES_H
#define VSC_TYPES_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 *  Compile-time limits (S0 — fixed arrays)
 * ======================================================================== */

#define VSC_MAX_ENTITIES         16
#define VSC_MAX_LINKS            32
#define VSC_MAX_ADJ               8   /* max downstream links per entity */
#define VSC_MAX_PADS_PER_ENTITY   4
#define VSC_MAX_TRACE_ENTRIES    32
#define VSC_MAX_ENDPOINTS         8
#define VSC_MAX_ENTITY_NAME      32

/* ========================================================================
 *  Pixel format constants (FourCC-like, test-space only)
 * ======================================================================== */

#define VSC_FMT_RAW8    0x52415738u   /* "RAW8" */
#define VSC_FMT_RAW10   0x52415741u   /* "RAWA" */
#define VSC_FMT_RAW12   0x52415742u   /* "RAWB" */
#define VSC_FMT_RGB888  0x52474238u   /* "RGB8" */
#define VSC_FMT_YUV422  0x59555532u   /* "YUV2" */
#define VSC_FMT_INVALID 0x00000000u

/* ========================================================================
 *  Media-bus format descriptor
 * ======================================================================== */

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;      /* FourCC-like */
    uint32_t frame_rate_num;    /* numerator, 0 = don't care */
    uint32_t frame_rate_den;    /* denominator */
    uint8_t  bit_depth;
    uint8_t  lanes;
    uint8_t  reserved[2];
} vsc_mbus_fmt_t;

#define VSC_MBUS_FMT_INIT {0, 0, VSC_FMT_INVALID, 0, 1, 0, 0, {0}}

static inline bool vsc_fmt_equal(const vsc_mbus_fmt_t *a, const vsc_mbus_fmt_t *b)
{
    return a->width == b->width &&
           a->height == b->height &&
           a->pixel_format == b->pixel_format &&
           a->frame_rate_num == b->frame_rate_num &&
           a->frame_rate_den == b->frame_rate_den &&
           a->bit_depth == b->bit_depth &&
           a->lanes == b->lanes;
}

static inline bool vsc_fmt_is_valid(const vsc_mbus_fmt_t *f)
{
    return f->pixel_format != VSC_FMT_INVALID && f->width > 0 && f->height > 0;
}

/* ========================================================================
 *  Format field identifier (for adjustment trace)
 * ======================================================================== */

typedef enum {
    VSC_FMT_FIELD_NONE      = 0,
    VSC_FMT_FIELD_WIDTH     = 1,
    VSC_FMT_FIELD_HEIGHT    = 2,
    VSC_FMT_FIELD_FORMAT    = 3,
    VSC_FMT_FIELD_FRAMERATE = 4,
} vsc_fmt_field_t;

/* ========================================================================
 *  Entity classification
 * ======================================================================== */

typedef enum {
    VSC_ENTITY_SENSOR   = 0,  /* data source, only SOURCE pad */
    VSC_ENTITY_STREAM   = 1,  /* processing node, SINK + SOURCE pads */
    VSC_ENTITY_ANALYZER = 2,  /* TAP observer, SINK only */
    VSC_ENTITY_ENDPOINT = 3,  /* data sink, SINK only */
} vsc_entity_class_t;

/* ========================================================================
 *  Link type
 * ======================================================================== */

typedef enum {
    VSC_LINK_STREAM = 0,   /* main pixel data flow */
    VSC_LINK_TAP    = 1,   /* passive observation */
} vsc_link_type_t;

/* ========================================================================
 *  Transform type (for feasibility pre-check)
 * ======================================================================== */

typedef enum {
    VSC_TRANSFORM_PASS_THROUGH   = 0,
    VSC_TRANSFORM_BINNING        = 1,
    VSC_TRANSFORM_CROP           = 2,
    VSC_TRANSFORM_PIXEL_FMT_CONV = 3,
    VSC_TRANSFORM_MULTI_STAGE    = 4,  /* compound: sub-transforms applied in
                                          forward order; reverse is reverse order */
} vsc_fmt_transform_type_t;

/* max sub-transforms in a MULTI_STAGE descriptor */
#define VSC_MAX_SUB_TRANSFORMS 8

/* ========================================================================
 *  Transform descriptor — algebraic approximation for Phase 1
 * ======================================================================== */

typedef struct vsc_fmt_transform_desc vsc_fmt_transform_desc_t;

struct vsc_fmt_transform_desc {
    vsc_fmt_transform_type_t type;

    union {
        struct {
            uint8_t factor_x;
            uint8_t factor_y;
        } binning;

        struct {
            uint32_t min_w;
            uint32_t min_h;
            uint32_t max_w;
            uint32_t max_h;
            uint8_t  align_w;
            uint8_t  align_h;
        } crop;

        struct {
            uint32_t fmt_in;
            uint32_t fmt_out;
        } pixel_fmt_conv;

        struct {
            uint8_t count;
            const vsc_fmt_transform_desc_t *subs;  /* static const array */
        } multi_stage;
    } params;
};

/* ========================================================================
 *  Interval for feasibility constraint propagation
 * ======================================================================== */

typedef struct {
    uint32_t lo;   /* inclusive */
    uint32_t hi;   /* inclusive, UINT32_MAX = unbounded upper */
} vsc_interval_t;

static inline bool vsc_interval_is_empty(const vsc_interval_t *iv)
{
    return iv->lo > iv->hi;
}

/* ========================================================================
 *  Feasibility constraint (output of inverse propagation)
 * ======================================================================== */

typedef struct {
    vsc_interval_t width_range;
    vsc_interval_t height_range;
    uint32_t       required_format;   /* VSC_FMT_INVALID = any */
    uint32_t       frame_rate_num;
    uint32_t       frame_rate_den;
} vsc_feasibility_constraint_t;

/* ========================================================================
 *  Propagation state (per-entity, reset each try)
 * ======================================================================== */

typedef struct {
    vsc_mbus_fmt_t sink_fmt;
    vsc_mbus_fmt_t source_fmt;
    bool           visited;
    bool           active;
} vsc_propagation_state_t;

/* ========================================================================
 *  Entity (forward-declare vsc_ip_ops_t — defined below)
 * ======================================================================== */

typedef struct vsc_ip_ops vsc_ip_ops_t;

typedef struct vsc_entity {
    char                      name[VSC_MAX_ENTITY_NAME];
    vsc_entity_class_t        entity_class;
    vsc_fmt_transform_desc_t  transform_desc;      /* for Phase 1 */
    vsc_propagation_state_t   prop_state;          /* runtime, per try */

    /* Driver ops (shared from vsc_driver_t, NOT entity-owned).
     * NULL ops entries = not implemented (e.g. ENDPOINT has no source
     * transform, SENSOR has no sink transform). */
    const vsc_ip_ops_t  *ops;

    void *drv_ctx;            /* driver private context (owned by driver) */
} vsc_entity_t;

/* ========================================================================
 *  P1 Driver types (consumed by generated vsc_driver_registry.c)
 * ======================================================================== */

/** @brief Property meta flags — bitmask */
#define VSC_PROP_READONLY     (1 << 0)
#define VSC_PROP_RUNTIME      (1 << 1)
#define VSC_PROP_PERSIST      (1 << 2)
#define VSC_PROP_TRANSACTION  (1 << 3)
#define VSC_PROP_ATOMIC       (1 << 4)
#define VSC_PROP_LAZY         (1 << 5)
#define VSC_PROP_DEBUG        (1 << 6)

/** @brief Property value type */
typedef enum {
    VSC_TYPE_U32 = 0,
    VSC_TYPE_I32 = 1,
    VSC_TYPE_F32 = 2,
    VSC_TYPE_BOOL = 3,
    VSC_TYPE_ENUM = 4,
    VSC_TYPE_STRING = 5,
} vsc_prop_type_t;

/** @brief Property value union */
typedef union {
    uint32_t u32;
    int32_t  i32;
    float    f32;
    bool     b;
    const char *str;
} vsc_prop_val_t;

/** @brief Property metadata — one entry per property, generated by Codegen */
typedef struct {
    uint16_t        prop_id;
    const char     *name;
    vsc_prop_type_t type;
    uint8_t         flags;
    vsc_prop_val_t  default_val;
    vsc_prop_val_t  min_val;
    vsc_prop_val_t  max_val;
    uint16_t        max_ref_id;   /* 0 = none, else target VSC_PROP_* */
} vsc_prop_meta_t;

/**
 * @brief Driver ops table.
 *
 * init() is called once per Instance during vsc_system_init().
 * It receives base_addr (from system_graph.json) and any prop_overrides.
 * The driver allocates a private context and returns it via *drv_ctx.
 *
 * try_fmt_sink / try_fmt_source / commit_fmt are the P0 Resolver callbacks.
 * drv_ctx is the pointer returned by init().
 */
/* forward declaration for vsc_ip_ops_t.init() parameter */
typedef struct { char key[64]; uint32_t value; } vsc_override_t;

struct vsc_ip_ops {
    int (*init)(void **drv_ctx, uint32_t base_addr,
                const vsc_override_t *overrides, uint8_t num_over);
    int (*try_fmt_sink)(void *drv_ctx, const vsc_mbus_fmt_t *proposed,
                        vsc_mbus_fmt_t *clamped);
    int (*try_fmt_source)(void *drv_ctx, const vsc_mbus_fmt_t *sink_fmt,
                          vsc_mbus_fmt_t *source_fmt);
    int (*commit_fmt)(void *drv_ctx, const vsc_mbus_fmt_t *final_fmt);
};

/* ── Driver capability bits ── */
#define VSC_CAP_SENSOR          (1 << 0)   /* image sensor driver           */
#define VSC_CAP_EXPOSURE_CTRL   (1 << 1)   /* has exposure control          */
#define VSC_CAP_STATISTICS      (1 << 2)   /* provides statistics data      */
#define VSC_CAP_HDR             (1 << 3)   /* supports HDR mode             */
#define VSC_CAP_TRIGGER         (1 << 4)   /* supports external trigger     */
#define VSC_CAP_CROP            (1 << 5)   /* crop / ROI processing         */
#define VSC_CAP_BINNING         (1 << 6)   /* binning / downscaling         */
#define VSC_CAP_FORMAT_CONV     (1 << 7)   /* pixel format conversion       */

/** @brief Driver descriptor — one per IP/sensor type (generated by Codegen) */
typedef struct vsc_driver {
    const char                 *name;
    uint16_t                    driver_id;
    uint32_t                    capabilities;  /* VSC_CAP_* bitmask */
    const vsc_prop_meta_t      *schema;
    uint8_t                     prop_count;
    const vsc_fmt_transform_desc_t *transform_template;
    vsc_ip_ops_t                ops;
} vsc_driver_t;

/* dependency edge — for generated dependency map */
typedef struct {
    uint16_t prop_id;    /* dependent property */
    uint16_t ref_id;     /* referenced property */
} vsc_prop_dep_t;

/* ========================================================================
 *  Link
 * ======================================================================== */

typedef struct {
    uint8_t          src_entity;   /* index into pipeline.entities[] */
    uint8_t          dst_entity;
    vsc_link_type_t  type;
    uint8_t          flags;        /* reserved */
    uint8_t          reserved;
} vsc_link_t;

#define VSC_LINK_FLAG_ENABLED  0x01

/* ========================================================================
 *  Pipeline — the central graph container
 * ======================================================================== */

typedef enum {
    VSC_PIPELINE_UNCONFIGURED = 0,
    VSC_PIPELINE_CONFIGURED   = 1,
    VSC_PIPELINE_STREAMING    = 2,
    VSC_PIPELINE_DIRTY        = 3,
} vsc_pipeline_state_t;

typedef struct {
    /* ── topology ── */
    uint8_t       num_entities;
    vsc_entity_t  entities[VSC_MAX_ENTITIES];
    uint8_t       num_links;
    vsc_link_t    links[VSC_MAX_LINKS];

    /* ── derived (computed by topo_sort) ── */
    uint8_t       execution_order[VSC_MAX_ENTITIES];  /* STREAM-only, topo-sorted */
    uint8_t       num_exec_order;
    uint8_t       tap_observers[VSC_MAX_ENTITIES];    /* ANALYZER indices */
    uint8_t       num_taps;
    uint8_t       endpoints[VSC_MAX_ENDPOINTS];
    uint8_t       num_endpoints;

    /* ── adjacency (pre-built for BFS) ── */
    uint8_t       adj_count[VSC_MAX_ENTITIES];
    uint8_t       adj_dst[VSC_MAX_ENTITIES][VSC_MAX_ADJ];    /* dst_idx per src */
    vsc_link_type_t adj_type[VSC_MAX_ENTITIES][VSC_MAX_ADJ]; /* link type */

    /* ── state ── */
    vsc_pipeline_state_t state;
} vsc_pipeline_t;

/* ========================================================================
 *  Adjustment reason
 * ======================================================================== */

typedef enum {
    VSC_ADJUST_NONE        = 0,
    VSC_ADJUST_CLAMP       = 1,   /* clamped to capability range */
    VSC_ADJUST_HALVE       = 2,   /* binning halved the dimension */
    VSC_ADJUST_FORMAT_CONV = 3,   /* pixel format converted */
    VSC_ADJUST_REJECTED    = 4,   /* entity rejected the format */
} vsc_adjust_reason_t;

/* ========================================================================
 *  Adjustment trace entry
 * ======================================================================== */

typedef struct {
    char                entity_name[VSC_MAX_ENTITY_NAME];
    char                pad_name[8];        /* "SINK" / "SOURCE" */
    vsc_fmt_field_t     field_changed;
    vsc_mbus_fmt_t      original;
    vsc_mbus_fmt_t      adjusted;
    vsc_adjust_reason_t reason;
    char                reason_detail[64];
} vsc_adjustment_entry_t;

/* ========================================================================
 *  Adjustment trace (diagnostics returned to application)
 * ======================================================================== */

typedef struct {
    uint8_t                num_entries;
    vsc_adjustment_entry_t entries[VSC_MAX_TRACE_ENTRIES];
} vsc_adjustment_trace_t;

/* ========================================================================
 *  Negotiation status
 * ======================================================================== */

typedef enum {
    VSC_NEGOTIATE_EXACT    = 0,  /* intent matched exactly */
    VSC_NEGOTIATE_ADJUSTED = 1,  /* accepted with modifications */
    VSC_NEGOTIATE_PARTIAL  = 2,  /* some branches/TAPs failed */
    VSC_NEGOTIATE_FAILED   = 3,  /* no valid configuration */
} vsc_negotiation_status_t;

/* ========================================================================
 *  Endpoint format (for multi-branch reporting)
 * ======================================================================== */

typedef struct {
    char           entity_name[VSC_MAX_ENTITY_NAME];
    vsc_mbus_fmt_t final_fmt;
    bool           active;
} vsc_endpoint_fmt_t;

/* ========================================================================
 *  Resolver result (returned to application)
 * ======================================================================== */

typedef struct {
    vsc_negotiation_status_t status;
    vsc_mbus_fmt_t           primary_fmt;
    char                     primary_endpoint[VSC_MAX_ENTITY_NAME];
    uint8_t                  num_endpoints;
    vsc_endpoint_fmt_t       endpoint_fmts[VSC_MAX_ENDPOINTS];
    vsc_adjustment_trace_t   trace;
    /* reachable bounds when status == FAILED (feasibility check) */
    vsc_mbus_fmt_t           reachable_max;
} vsc_resolver_result_t;

/* ========================================================================
 *  Resolver error codes
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
#define VSC_WARN_TAP_INACTIVE          1

/* ========================================================================
 *  Pipeline build / topo helpers
 * ======================================================================== */

/**
 * @brief Run topological sort on STREAM links and populate execution_order,
 *        tap_observers, endpoints, and adjacency arrays.
 *
 * execution_order contains SENSOR + STREAM + ENDPOINT entities in topo order
 * (all entities reachable via STREAM links).
 *
 * Must be called after entities[] and links[] are populated.
 * Returns VSC_OK or VSC_ERR_TOPOLOGY_BROKEN (cycle or disconnected STREAM).
 */
int vsc_pipeline_build(vsc_pipeline_t *pipeline);

/**
 * @brief Remove an optional entity and auto-bridge if possible.
 *
 * Handles SISO auto-bridging, TAP link removal, leaf/root removal.
 * Re-runs topological sort.
 *
 * @return VSC_OK, VSC_ERR_CANNOT_AUTO_BRIDGE, or VSC_ERR_TOPOLOGY_BROKEN.
 */
int vsc_pipeline_remove_optional(vsc_pipeline_t *pipeline, uint8_t entity_idx);

/**
 * @brief Commit: write the final negotiated format to hardware registers
 *        for all STREAM entities in execution_order.
 *
 * Must be called after a successful try_fmt whose result was accepted.
 * Does NOT perform rollback on partial failure — marks pipeline DIRTY.
 *
 * @return VSC_OK or VSC_ERR_COMMIT_FAILED.
 */
int vsc_pipeline_commit_fmt(vsc_pipeline_t *pipeline,
                            const vsc_mbus_fmt_t *final_fmt);

/* ── Driver registry access (see vsc_driver_registry.h) ── */

/* ========================================================================
 *  Resolver API
 * ======================================================================== */

/**
 * @brief Full try_fmt pipeline: Phase 1 (feasibility) → Phase 2 (forward
 *        propagate) → Phase 3 (converge).
 *
 * @param pipeline   Pre-built pipeline with entities and links.
 * @param intent     Application's desired output format.
 * @param result     [out] Negotiation result, including final fmt and trace.
 * @return VSC_OK on success, negative error code on failure.
 */
int vsc_resolver_try_fmt(vsc_pipeline_t *pipeline,
                         const vsc_mbus_fmt_t *intent,
                         vsc_resolver_result_t *result);

/**
 * @brief Phase 1 only: reverse-constraint feasibility check.
 *
 * @return VSC_OK if feasible, VSC_ERR_UNREACHABLE if not (result.reachable_max
 *         populated with approximate bounds).
 */
int vsc_resolver_feasibility_check(const vsc_pipeline_t *pipeline,
                                   const vsc_mbus_fmt_t *intent,
                                   vsc_resolver_result_t *result);

/**
 * @brief Phase 2 only: forward propagation along STREAM links.
 *
 * Requires pipeline.execution_order to be already sorted.
 * Populates each entity's prop_state.
 *
 * @return VSC_OK or negative error.
 */
int vsc_resolver_forward_propagate(vsc_pipeline_t *pipeline,
                                   const vsc_mbus_fmt_t *intent);

/**
 * @brief Phase 3 only: converge endpoint formats into result.
 *
 * Requires forward_propagate to have completed.
 */
int vsc_resolver_converge(vsc_pipeline_t *pipeline,
                          const vsc_mbus_fmt_t *intent,
                          vsc_resolver_result_t *result);

#ifdef __cplusplus
}
#endif

#endif /* VSC_TYPES_H */
