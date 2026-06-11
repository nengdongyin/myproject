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

/**
 * @brief 媒体总线格式描述符
 * @details 描述视频数据在 Entity 之间传输时的格式参数，
 *          包括分辨率、像素格式（FourCC 风格）、帧率、位深和通道数。
 *          帧率分子为 0 表示"不关心"（通配）。
 */
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

/**
 * @brief 比较两个格式描述符是否完全相等
 * @param[in] a 第一个格式描述符指针
 * @param[in] b 第二个格式描述符指针
 * @return 逐字段相等时返回 true
 */
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

/**
 * @brief 检查格式描述符是否有效
 * @param[in] f 格式描述符指针
 * @return pixel_format 非 INVALID 且宽高均大于 0 时返回 true
 */
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

/**
 * @brief 整数闭区间，用于可行性约束传播
 * @details lo ≤ hi 表示合法区间；lo > hi 表示空区间（无合法值）。
 *          hi = UINT32_MAX 表示上界无限制。
 */
typedef struct {
    uint32_t lo;   /**< 下界（包含） */
    uint32_t hi;   /**< 上界（包含），UINT32_MAX 表示无限制 */
} vsc_interval_t;

/**
 * @brief 判断区间是否为空
 * @param[in] iv 区间指针
 * @return lo > hi 时返回 true
 */
static inline bool vsc_interval_is_empty(const vsc_interval_t *iv)
{
    return iv->lo > iv->hi;
}

/* ========================================================================
 *  Feasibility constraint (output of inverse propagation)
 * ======================================================================== */

/**
 * @brief 可行性约束 — 逆传播的输出
 * @details Phase 1 可行性检查将用户意图从端点沿管线逆向传播到传感器，
 *          生成此约束。如果传感器的能力范围覆盖该约束，则协商可行。
 *          required_format = VSC_FMT_INVALID 表示格式方面无要求（通配）。
 */
typedef struct {
    vsc_interval_t width_range;        /**< 宽度允许区间 [lo, hi] */
    vsc_interval_t height_range;       /**< 高度允许区间 [lo, hi] */
    uint32_t       required_format;    /**< 要求的像素格式，VSC_FMT_INVALID 表示任意 */
    uint32_t       frame_rate_num;     /**< 要求的帧率分子 */
    uint32_t       frame_rate_den;     /**< 要求的帧率分母 */
} vsc_feasibility_constraint_t;

/* ========================================================================
 *  Propagation state (per-entity, reset each try)
 * ======================================================================== */

/**
 * @brief 单次格式协商中每个 Entity 的传播状态
 * @details 每次调用 vsc_resolver_try_fmt() 时重置。
 *          sink_fmt 记录该实体接收到的输入格式，
 *          source_fmt 记录该实体产生的输出格式，
 *          visited/active 供 BFS 传播算法使用。
 */
typedef struct {
    vsc_mbus_fmt_t sink_fmt;    /**< 输入侧（SINK pad）协商后的格式 */
    vsc_mbus_fmt_t source_fmt;  /**< 输出侧（SOURCE pad）协商后的格式 */
    bool           visited;     /**< BFS 是否已访问过该节点 */
    bool           active;      /**< 该实体在当前协商中是否有效 */
} vsc_propagation_state_t;

/* ========================================================================
 *  Entity (forward-declare vsc_ip_ops_t — defined below)
 * ======================================================================== */

typedef struct vsc_ip_ops vsc_ip_ops_t;

/**
 * @brief 视频管线中的一个处理节点
 * @details Entity 是 Pipeline 图的基本顶点，代表传感器、
 *          FPGA IP 核（crop/binning/decoder）、分析器或输出端点。
 *          每个 Entity 携带一个 transform_desc（Phase 1 代数模型）
 *          和一个可选的 ops 虚表（Phase 2 精确模型）。
 *          生命周期由 vsc_pipeline_t 管理，无独立分配。
 */
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

/**
 * @brief 两个 Entity 之间的有向连接
 * @details Link 表示数据流方向（src → dst），分为 STREAM（主数据流）
 *          和 TAP（被动观测）两种类型。flags 字段用于标记链路状态
 *          （如 VSC_LINK_FLAG_ENABLED）。
 */
typedef struct {
    uint8_t          src_entity;   /**< 源 Entity 在 pipeline.entities[] 中的索引 */
    uint8_t          dst_entity;   /**< 目标 Entity 在 pipeline.entities[] 中的索引 */
    vsc_link_type_t  type;         /**< 链路类型：STREAM 或 TAP */
    uint8_t          flags;        /**< 链路标志位（如 VSC_LINK_FLAG_ENABLED） */
    uint8_t          reserved;     /**< 保留字段 */
} vsc_link_t;

#define VSC_LINK_FLAG_ENABLED  0x01

/* ========================================================================
 *  Pipeline — 核心图容器
 * ======================================================================== */

/**
 * @brief 管线状态枚举
 * @details 用于跟踪管线生命周期：UNCONFIGURED → CONFIGURED（commit 后）→
 *          STREAMING（流传输中）→ DIRTY（部分提交失败需重建）。
 */
typedef enum {
    VSC_PIPELINE_UNCONFIGURED = 0,  /**< 初始状态：未配置 */
    VSC_PIPELINE_CONFIGURED   = 1,  /**< 已配置：commit 成功 */
    VSC_PIPELINE_STREAMING    = 2,  /**< 流传输中：不可重配置 */
    VSC_PIPELINE_DIRTY        = 3,  /**< 脏状态：部分提交失败 */
} vsc_pipeline_state_t;

/**
 * @brief 视频管线 — 核心图容器
 * @details Pipeline 是整个 VSC 子系统的核心数据结构，包含：
 *          - 拓扑信息（entities + links）
 *          - 预计算数据（拓扑排序执行顺序、邻接表、TAP/端点索引）
 *          - 运行时状态（管线状态机）
 *          所有成员均为固定大小数组（S0 内存策略），无动态分配。
 * @see vsc_pipeline_build() 用于从 entities/links 计算派生字段
 */
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

/**
 * @brief 格式调整轨迹中的单条记录
 * @details 记录一次格式协商过程中，某个 Entity 的某个 Pad 上
 *          发生的格式变化（字段、原值、新值、原因）。
 *          用于诊断为何最终格式与用户意图不一致。
 */
typedef struct {
    char                entity_name[VSC_MAX_ENTITY_NAME]; /**< 实体名称 */
    char                pad_name[8];        /**< Pad 名称："SINK" 或 "SOURCE" */
    vsc_fmt_field_t     field_changed;      /**< 发生变化的格式字段 */
    vsc_mbus_fmt_t      original;           /**< 变化前的格式值 */
    vsc_mbus_fmt_t      adjusted;           /**< 变化后的格式值 */
    vsc_adjust_reason_t reason;             /**< 调整原因分类 */
    char                reason_detail[64];  /**< 调整原因的文本说明 */
} vsc_adjustment_entry_t;

/* ========================================================================
 *  Adjustment trace (diagnostics returned to application)
 * ======================================================================== */

/**
 * @brief 格式调整轨迹 — 协商过程的诊断记录
 * @details 包含最多 VSC_MAX_TRACE_ENTRIES 条调整记录，
 *          按传播顺序排列。应用层可通过此轨迹了解
 *          每个 Entity 对格式所做的修改及其原因。
 */
typedef struct {
    uint8_t                num_entries;                          /**< 有效条目数 */
    vsc_adjustment_entry_t entries[VSC_MAX_TRACE_ENTRIES];       /**< 调整记录数组 */
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

/**
 * @brief 格式协商的完整结果
 * @details 由 vsc_resolver_try_fmt() 返回，包含：
 *          - status：协商状态（精确匹配/调整后接受/部分失败/完全失败）
 *          - primary_fmt：主输出端点的最终格式
 *          - endpoint_fmts：所有端点的格式（支持多分支输出）
 *          - trace：格式变更轨迹（诊断用途）
 *          - reachable_max：协商失败时的可达上限（调试用途）
 */
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

/* ────────────────────────────────────────────────────────────────────────
 *  Pipeline Graph API  —  see  vsc_resolver.h
 *  Property Resolver API —  see  vsc_resolver.h
 *
 *  vsc_types.h 只包含类型定义；所有公开函数声明已迁移到 vsc_resolver.h。
 * ──────────────────────────────────────────────────────────────────────── */

#ifdef __cplusplus
}
#endif

#endif /* VSC_TYPES_H */
