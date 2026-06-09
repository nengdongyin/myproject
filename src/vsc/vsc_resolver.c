/**
 * @file    vsc_resolver.c
 * @brief   VSC Property Resolver — Phase 1 (feasibility), Phase 2 (forward
 *          propagate), Phase 3 (converge), and optional-entity auto-bridging.
 *
 * S0 strategy: all structures fixed-size, no malloc.  Every pipeline entity
 * carries a transform_desc (algebraic model for Phase 1) and optional
 * try_fmt_sink / try_fmt_source callbacks (exact model for Phase 2).
 */

#include "vsc_types.h"
#include "vsc_resolver.h"
#include <string.h>

/* ========================================================================
 *  Internal helpers — feasibility inverse transforms
 * ======================================================================== */

/**
 * @brief 逆变换 — PASS_THROUGH：约束不变
 * @param[in,out] c 可行性约束（不修改）
 * @details 直通实体对格式无任何改变，逆传播时约束完全透传。
 */
static void iv_apply_pass_through(vsc_feasibility_constraint_t *c)
{
    (void)c;  /* nothing to do */
}

/**
 * @brief 逆变换 — BINNING：保守策略，Phase 1 使用 factor=1
 * @param[in,out] c        可行性约束
 * @param[in]     factor_x X 方向缩放因子（Phase 1 忽略）
 * @param[in]     factor_y Y 方向缩放因子（Phase 1 忽略）
 * @details Phase 1 可行性检查采用最保守估计（factor=1），
 *          约束完全不变。这避免了对传感器提出过高的输入要求，
 *          防止因运行时缩放因子较小而产生假阴性 UNREACHABLE。
 *          Phase 2 通过驱动回调处理精确的运行时因子。
 * @see vsc_resolver_forward_propagate() Phase 2 精确传播
 */
static void iv_apply_binning(vsc_feasibility_constraint_t *c,
                             uint8_t factor_x, uint8_t factor_y)
{
    /* Phase 1 uses factor=1 (bypass) regardless of template value.
     * This is the most conservative setting — it imposes the lowest
     * requirement on the sensor.  A larger factor would increase the
     * required sensor input, potentially causing false-negative
     * UNREACHABLE when the runtime factor is actually smaller.
     * Phase 2 handles the exact runtime factor via driver callbacks. */
    (void)c; (void)factor_x; (void)factor_y;
    /* constraint unchanged — equivalent to factor=1 */
}

/**
 * @brief 逆变换 — CROP：扩展约束区间以反映裁剪能力
 * @param[in,out] c     可行性约束
 * @param[in]     min_w 裁剪硬件的最小输出宽度
 * @param[in]     min_h 裁剪硬件的最小输出高度
 * @param[in]     max_w 裁剪硬件的最大输出宽度（上界）
 * @param[in]     max_h 裁剪硬件的最大输出高度（上界）
 * @return 0 成功；-1 表示约束变为空（意图超出裁剪能力）
 * @details CROP 实体只能减小（或透传）尺寸。为保证输出落在
 *          约束要求的 [lo, hi] 区间内，输入必须满足：
 *          - 输入 ≥ 有效下界（eff_lo_w, eff_lo_h）
 *          - 输入 ≤ 裁剪最大输出（max_w, max_h）
 *          该函数将约束区间扩展为 [eff_lo, crop_max]，
 *          此区间即传感器的输入要求。
 */
static int iv_apply_crop(vsc_feasibility_constraint_t *c,
                         uint32_t min_w, uint32_t min_h,
                         uint32_t max_w, uint32_t max_h)
{
    /* effective lo: can't demand less than crop can produce */
    uint32_t eff_lo_w = c->width_range.lo;
    uint32_t eff_lo_h = c->height_range.lo;

    if (eff_lo_w < min_w) eff_lo_w = min_w;
    if (eff_lo_h < min_h) eff_lo_h = min_h;

    /* upper bound: crop can only output up to max_w */
    if (eff_lo_w > max_w || eff_lo_h > max_h)
        return -1;  /* empty — intent exceeds crop capability */

    c->width_range.lo  = eff_lo_w;
    c->width_range.hi  = max_w;
    c->height_range.lo = eff_lo_h;
    c->height_range.hi = max_h;

    return 0;
}

/**
 * @brief 逆变换 — PIXEL_FMT_CONV：格式要求转换为输入格式
 * @param[in,out] c       可行性约束
 * @param[in]     fmt_in  转换器接受的输入格式
 * @param[in]     fmt_out 转换器产生的输出格式
 * @return 0 成功；-1 表示当前要求格式与转换器不兼容
 * @details 如果当前约束的 required_format 与转换器输出格式匹配
 *          （或为 VSC_FMT_INVALID 通配），则将要求格式切换为输入格式。
 *          不匹配时返回 -1，表示此转换器无法满足当前的格式要求。
 */
static int iv_apply_fmt_conv(vsc_feasibility_constraint_t *c,
                             uint32_t fmt_in, uint32_t fmt_out)
{
    /* only activate if the current required format matches the converter's output */
    if (c->required_format == VSC_FMT_INVALID ||
        c->required_format == fmt_out) {
        c->required_format = fmt_in;
        return 0;
    }
    /* mismatch — this converter can't produce the required output format */
    return -1;
}

/**
 * @brief 对单个 Entity 的变换描述符执行完整逆变换
 * @param[in]     desc Entity 的变换描述符
 * @param[in,out] c    可行性约束（会被修改）
 * @return 0 成功；-1 约束变为空（不可达）
 * @details 根据 desc->type 分发到对应的逆变换函数。
 *          对于 MULTI_STAGE 类型，按逆序（从最后一级到第一级）
 *          递归调用自身——这是逆传播的核心递归入口。
 * @see iv_apply_pass_through(), iv_apply_binning(),
 *      iv_apply_crop(), iv_apply_fmt_conv()
 */
static int feasibility_inverse_one(const vsc_fmt_transform_desc_t *desc,
                                   vsc_feasibility_constraint_t *c)
{
    switch (desc->type) {
    case VSC_TRANSFORM_PASS_THROUGH:
        iv_apply_pass_through(c);
        return 0;

    case VSC_TRANSFORM_BINNING:
        iv_apply_binning(c, desc->params.binning.factor_x,
                            desc->params.binning.factor_y);
        return 0;

    case VSC_TRANSFORM_CROP:
        return iv_apply_crop(c,
                             desc->params.crop.min_w,
                             desc->params.crop.min_h,
                             desc->params.crop.max_w,
                             desc->params.crop.max_h);

    case VSC_TRANSFORM_PIXEL_FMT_CONV:
        return iv_apply_fmt_conv(c,
                                 desc->params.pixel_fmt_conv.fmt_in,
                                 desc->params.pixel_fmt_conv.fmt_out);

    case VSC_TRANSFORM_MULTI_STAGE:
        /* reverse order for inverse propagation */
        for (int si = desc->params.multi_stage.count; si > 0; si--) {
            if (feasibility_inverse_one(
                    &desc->params.multi_stage.subs[si - 1], c) != 0)
                return -1;
        }
        return 0;

    default:
        return -1;
    }
}

/* ========================================================================
 *  Pipeline build — 拓扑排序（Kahn 算法，仅 STREAM 链路）
 * ======================================================================== */

/**
 * @brief 构建管线拓扑：拓扑排序、邻接表、端点/TAP 收集
 * @param[in,out] pipeline 包含 entities[] 和 links[] 的管线
 * @return VSC_OK 或 VSC_ERR_TOPOLOGY_BROKEN（存在环或断连）
 * @details 采用 **Kahn 算法**（基于入度的拓扑排序）处理 STREAM 链路：
 *          **阶段 1 — 构建入度与邻接表**
 *          遍历所有 STREAM 链路，为每个目标节点增加入度计数，
 *          同时填充 adj_dst[][] / adj_type[][] 邻接矩阵。
 *          **阶段 2 — TAP 观察者收集**
 *          遍历 TAP 链路，将目标为 ANALYZER 实体的节点加入
 *          tap_observers[] 列表，同时将 TAP 链路加入邻接表
 *          （供 Phase 2 forward_propagate 使用）。
 *          **阶段 3 — Kahn 初始化**
 *          将所有入度为 0 的节点入队。
 *          **阶段 4 — Kahn 主循环**
 *          出队节点，写入 execution_order[]，递减其下游邻居的入度，
 *          入度归零的下游节点入队。
 *          **阶段 5 — 环检测**
 *          遍历所有节点，若仍有入度 > 0 的节点则存在环，
 *          返回 VSC_ERR_TOPOLOGY_BROKEN。
 *          **阶段 6 — 端点收集**
 *          将所有 VSC_ENTITY_ENDPOINT 类型的节点加入 endpoints[]。
 *          最后将管线状态设为 UNCONFIGURED。
 * @note 时间复杂度 O(V + E)，空间复杂度 O(V²)（固定大小邻接矩阵）
 * @warning 调用前必须确保 entities[] 和 links[] 已填充完毕
 */
int vsc_pipeline_build(vsc_pipeline_t *pipeline)
{
    uint8_t i, j;
    uint8_t in_degree[VSC_MAX_ENTITIES];
    uint8_t queue[VSC_MAX_ENTITIES];
    uint8_t q_head = 0, q_tail = 0;

    memset(in_degree, 0, sizeof(in_degree));
    memset(pipeline->execution_order, 0, sizeof(pipeline->execution_order));
    memset(pipeline->tap_observers, 0, sizeof(pipeline->tap_observers));
    memset(pipeline->endpoints, 0, sizeof(pipeline->endpoints));
    memset(pipeline->adj_count, 0, sizeof(pipeline->adj_count));
    pipeline->num_exec_order = 0;
    pipeline->num_taps       = 0;
    pipeline->num_endpoints  = 0;

    /* ── build in-degree + adjacency (STREAM links only) ── */
    for (i = 0; i < pipeline->num_links; i++) {
        const vsc_link_t *l = &pipeline->links[i];

        if (l->type != VSC_LINK_STREAM) continue;

        uint8_t src = l->src_entity;
        uint8_t dst = l->dst_entity;
        uint8_t cnt = pipeline->adj_count[src];

        if (cnt < VSC_MAX_ADJ) {
            pipeline->adj_dst[src][cnt]  = dst;
            pipeline->adj_type[src][cnt] = VSC_LINK_STREAM;
            pipeline->adj_count[src]     = cnt + 1;
        }
        in_degree[dst]++;
    }

    /* ── collect TAP observers ── */
    for (i = 0; i < pipeline->num_links; i++) {
        if (pipeline->links[i].type == VSC_LINK_TAP) {
            uint8_t dst = pipeline->links[i].dst_entity;
            /* add to tap list if ANALYZER (best-effort check) */
            if (pipeline->entities[dst].entity_class == VSC_ENTITY_ANALYZER) {
                if (pipeline->num_taps < VSC_MAX_ENTITIES)
                    pipeline->tap_observers[pipeline->num_taps++] = dst;
            }
            /* also add to adjacency (for forward_propagate TAP notifications) */
            uint8_t src = pipeline->links[i].src_entity;
            uint8_t cnt = pipeline->adj_count[src];
            if (cnt < VSC_MAX_ADJ) {
                pipeline->adj_dst[src][cnt]  = dst;
                pipeline->adj_type[src][cnt] = VSC_LINK_TAP;
                pipeline->adj_count[src]     = cnt + 1;
            }
        }
    }

    /* ── Kahn init: enqueue entities with in_degree == 0 ── */
    for (i = 0; i < pipeline->num_entities; i++) {
        if (in_degree[i] == 0)
            queue[q_tail++] = i;
    }

    /* ── Kahn loop ── */
    while (q_head < q_tail) {
        uint8_t u = queue[q_head++];
        pipeline->execution_order[pipeline->num_exec_order++] = u;

        for (j = 0; j < pipeline->adj_count[u]; j++) {
            if (pipeline->adj_type[u][j] != VSC_LINK_STREAM) continue;
            uint8_t v = pipeline->adj_dst[u][j];
            if (in_degree[v] > 0) {
                in_degree[v]--;
                if (in_degree[v] == 0)
                    queue[q_tail++] = v;
            }
        }
    }

    /* ── cycle / disconnected check ── */
    for (i = 0; i < pipeline->num_entities; i++) {
        if (in_degree[i] > 0)
            return VSC_ERR_TOPOLOGY_BROKEN;
    }

    /* ── collect ENDPOINTs ── */
    for (i = 0; i < pipeline->num_entities; i++) {
        if (pipeline->entities[i].entity_class == VSC_ENTITY_ENDPOINT) {
            if (pipeline->num_endpoints < VSC_MAX_ENDPOINTS)
                pipeline->endpoints[pipeline->num_endpoints++] = i;
        }
    }

    pipeline->state = VSC_PIPELINE_UNCONFIGURED;
    return VSC_OK;
}

/* ========================================================================
 *  Optional entity auto-bridging
 * ======================================================================== */

/**
 * @brief 移除可选 Entity 并尝试自动桥接
 * @param[in,out] pipeline   管线
 * @param[in]     entity_idx 要移除的 Entity 索引
 * @return VSC_OK、VSC_ERR_CANNOT_AUTO_BRIDGE 或 VSC_ERR_TOPOLOGY_BROKEN
 * @details 自动桥接策略处理三种拓扑情况：
 *          **SISO（单入单出）** — 最常见情况：
 *          创建一条直接连接 in_src[0] → out_dst[0] 的新 STREAM 链路，
 *          替代被移除的节点。
 *          **MIMO（多入或多出）** — 无法自动桥接：
 *          返回 VSC_ERR_CANNOT_AUTO_BRIDGE，需要应用层干预。
 *          **叶节点/根节点（0 入或 0 出）** — 直接移除：
 *          无需创建新链路，仅删除该节点及其关联链路即可。
 *          操作完成后：
 *          1. 删除所有涉及 entity_idx 的链路（压缩 links[] 数组）
 *          2. 删除实体本身（压缩 entities[] 数组并前移后续元素）
 *          3. 修正所有链路中的 Entity 索引（因数组压缩导致索引偏移）
 *          4. 重新调用 vsc_pipeline_build() 重建拓扑
 * @warning 本函数会修改 entities[] 和 links[] 的内容和数量，
 *          调用者必须确保外部引用（如 entity_idx）在调用后失效
 */
int vsc_pipeline_remove_optional(vsc_pipeline_t *pipeline, uint8_t entity_idx)
{
    if (entity_idx >= pipeline->num_entities)
        return VSC_ERR_CANNOT_AUTO_BRIDGE;

    uint8_t n_entities = pipeline->num_entities;
    uint8_t n_links    = pipeline->num_links;

    /* ── find incoming / outgoing STREAM links ── */
    uint8_t in_src[VSC_MAX_LINKS], in_count = 0;
    uint8_t out_dst[VSC_MAX_LINKS], out_count = 0;

    for (uint8_t i = 0; i < n_links; i++) {
        if (pipeline->links[i].dst_entity == entity_idx &&
            pipeline->links[i].type == VSC_LINK_STREAM) {
            if (in_count >= VSC_MAX_LINKS) return VSC_ERR_CANNOT_AUTO_BRIDGE;
            in_src[in_count++] = pipeline->links[i].src_entity;
        }
        if (pipeline->links[i].src_entity == entity_idx &&
            pipeline->links[i].type == VSC_LINK_STREAM) {
            if (out_count >= VSC_MAX_LINKS) return VSC_ERR_CANNOT_AUTO_BRIDGE;
            out_dst[out_count++] = pipeline->links[i].dst_entity;
        }
    }

    /* ── auto-bridging rules ── */
    if (in_count == 1 && out_count == 1) {
        /* SISO: create direct bridge */
        if (n_links >= VSC_MAX_LINKS) return VSC_ERR_CANNOT_AUTO_BRIDGE;

        pipeline->links[n_links].src_entity = in_src[0];
        pipeline->links[n_links].dst_entity = out_dst[0];
        pipeline->links[n_links].type       = VSC_LINK_STREAM;
        pipeline->links[n_links].flags      = VSC_LINK_FLAG_ENABLED;
        n_links++;
    } else if (in_count > 1 || out_count > 1) {
        /* MIMO — can't auto-bridge */
        return VSC_ERR_CANNOT_AUTO_BRIDGE;
    }
    /* else: leaf/root (0 in or 0 out) — just remove, no bridge needed */

    /* ── remove all links involving entity_idx ── */
    uint8_t write = 0;
    for (uint8_t i = 0; i < n_links; i++) {
        if (pipeline->links[i].src_entity == entity_idx ||
            pipeline->links[i].dst_entity == entity_idx) {
            continue;  /* drop */
        }
        if (write != i) pipeline->links[write] = pipeline->links[i];
        write++;
    }
    pipeline->num_links = write;

    /* ── remove entity, compact array ── */
    for (uint8_t i = entity_idx; i < n_entities - 1; i++) {
        pipeline->entities[i] = pipeline->entities[i + 1];
    }
    pipeline->num_entities = n_entities - 1;

    /* ── fixup link indices (entities shifted) ── */
    for (uint8_t i = 0; i < pipeline->num_links; i++) {
        if (pipeline->links[i].src_entity > entity_idx)
            pipeline->links[i].src_entity--;
        if (pipeline->links[i].dst_entity > entity_idx)
            pipeline->links[i].dst_entity--;
    }

    /* ── rebuild topology ── */
    return vsc_pipeline_build(pipeline);
}

/**
 * @brief 从用户意图初始化可行性约束
 * @param[out] c      待初始化的约束
 * @param[in]  intent 用户期望的输出格式
 * @details 将意图的 width/height 设为精确的点区间 [val, val]，
 *          pixel_format 直接赋值。帧率同样精确匹配。
 */
static void constraint_from_intent(vsc_feasibility_constraint_t *c,
                                   const vsc_mbus_fmt_t *intent)
{
    c->width_range.lo  = intent->width;
    c->width_range.hi  = intent->width;
    c->height_range.lo = intent->height;
    c->height_range.hi = intent->height;
    c->required_format = intent->pixel_format;
    c->frame_rate_num  = intent->frame_rate_num;
    c->frame_rate_den  = intent->frame_rate_den;
}

/* ========================================================================
 *  Phase 1 helper — trace reverse path from an entity to SENSOR
 * ======================================================================== */

/**
 * @brief Walk backwards from `start_idx` along STREAM links until a SENSOR
 *        entity is reached.
 *
 * @param path  [out] entity indices from start_idx back to the node just
 *              after SENSOR (SENSOR itself is excluded).
 * @return      number of entities on the path (0 if start is already SENSOR).
 */
static uint8_t find_reverse_path(const vsc_pipeline_t *pipeline,
                                 uint8_t start_idx,
                                 uint8_t *path)
{
    uint8_t len = 0;
    uint8_t cur = start_idx;

    /* guard: already at SENSOR */
    if (pipeline->entities[cur].entity_class == VSC_ENTITY_SENSOR)
        return 0;

    while (1) {
        /* find the STREAM link where cur is the destination */
        bool found = false;
        for (uint8_t li = 0; li < pipeline->num_links; li++) {
            if (pipeline->links[li].dst_entity == cur &&
                pipeline->links[li].type == VSC_LINK_STREAM) {
                /* move to upstream entity */
                cur = pipeline->links[li].src_entity;
                found = true;
                break;
            }
        }

        if (!found) break; /* orphaned — no upstream STREAM link */

        if (pipeline->entities[cur].entity_class == VSC_ENTITY_SENSOR)
            break; /* reached sensor — don't include it in path */

        path[len++] = cur;
    }

    return len;
}

/* ========================================================================
 *  Phase 1 — 可行性检查（逐端点逆传播）
 * ======================================================================== */

/**
 * @brief Phase 1 可行性检查：从输出端点逆向传播约束到传感器
 * @param[in]  pipeline 已构建的管线（需已完成拓扑排序）
 * @param[in]  intent   用户期望的输出格式
 * @param[out] result   协商结果（失败时 reachable_max 会填充可达上限）
 * @return VSC_OK 表示可行性通过；VSC_ERR_UNREACHABLE 表示不可达
 * @details 实现三个分支路径：
 *          **路径 A — 空管线保护**
 *          entities[] 或 execution_order[] 为空时直接返回失败。
 *          **路径 B — 无端点回退（线性管线）**
 *          当 pipeline->num_endpoints == 0 时，按 execution_order 逆序
 *          遍历所有 STREAM 实体，逐实体执行逆变换。最后检查传感器约束。
 *          这是一个简化的线性路径，假设管线为单链结构。
 *          **路径 C — 逐端点检查（主路径）**
 *          对每个注册的端点：
 *          1. 调用 find_reverse_path() 回溯到传感器，获得逆序路径
 *          2. 沿路径逐实体执行 feasibility_inverse_one()
 *          3. 对传感器进行最终约束检查（分辨率区间 + 像素格式）
 *          4. 至少一个端点通过即视为可行
 *          所有路径在失败时填充 result->reachable_max，
 *          记录从 CROP 实体或传感器获取的最大可达尺寸。
 * @note 本函数不修改管线状态（const pipeline），可安全并发调用
 * @warning 本函数约 200 行，包含 goto 跳转（fail_no_endpoints /
 *          check_sensor_linear），未来可考虑拆分为独立子函数
 */
int vsc_resolver_feasibility_check(const vsc_pipeline_t *pipeline,
                                   const vsc_mbus_fmt_t *intent,
                                   vsc_resolver_result_t *result)
{
    uint8_t i, ep;
    bool    any_feasible = false;
    vsc_mbus_fmt_t best_reachable = {0, 0, VSC_FMT_INVALID, 0, 1, 0, 0, {0}};

    memset(result, 0, sizeof(*result));

    /* ── guard: empty pipeline ── */
    if (pipeline->num_entities == 0 || pipeline->num_exec_order == 0) {
        result->status = VSC_NEGOTIATE_FAILED;
        return VSC_ERR_UNREACHABLE;
    }

    /* ── if no endpoints registered, fall back to linear reverse walk ── */
    if (pipeline->num_endpoints == 0) {
        /* simple linear pipeline: walk execution_order in reverse */
        vsc_feasibility_constraint_t c;
        constraint_from_intent(&c, intent);

        for (i = pipeline->num_exec_order; i > 0; i--) {
            uint8_t idx = pipeline->execution_order[i - 1];
            const vsc_entity_t *e = &pipeline->entities[idx];
            if (e->entity_class == VSC_ENTITY_SENSOR) break;
            if (feasibility_inverse_one(&e->transform_desc, &c) != 0)
                goto fail_no_endpoints;
        }
        /* check sensor (same logic as per-endpoint below) */
        goto check_sensor_linear;
    }

    /* ── per-endpoint reverse path check ── */
    for (ep = 0; ep < pipeline->num_endpoints; ep++) {
        uint8_t ep_idx = pipeline->endpoints[ep];
        uint8_t rev_path[VSC_MAX_ENTITIES];
        uint8_t path_len;

        vsc_feasibility_constraint_t c;
        constraint_from_intent(&c, intent);

        /* trace reverse path from this endpoint */
        path_len = find_reverse_path(pipeline, ep_idx, rev_path);

        /* walk the path (already in reverse order: nearest to endpoint first) */
        bool path_ok = true;
        for (i = 0; i < path_len; i++) {
            const vsc_entity_t *e = &pipeline->entities[rev_path[i]];
            if (feasibility_inverse_one(&e->transform_desc, &c) != 0) {
                path_ok = false;
                /* record reachable bounds from the failing entity */
                if (e->transform_desc.type == VSC_TRANSFORM_CROP) {
                    if (e->transform_desc.params.crop.max_w > best_reachable.width)
                        best_reachable.width  = e->transform_desc.params.crop.max_w;
                    if (e->transform_desc.params.crop.max_h > best_reachable.height)
                        best_reachable.height = e->transform_desc.params.crop.max_h;
                }
                break;
            }
        }

        if (!path_ok) continue; /* try next endpoint */

        /* ── find the SENSOR that feeds this path and verify constraint ── */
        {
            const vsc_entity_t *sensor_e = NULL;
            /* the SENSOR is the upstream entity of the last node on the path */
            if (path_len > 0) {
                uint8_t last = rev_path[path_len - 1];
                /* find the STREAM link where 'last' is the destination */
                for (uint8_t li = 0; li < pipeline->num_links; li++) {
                    if (pipeline->links[li].dst_entity == last &&
                        pipeline->links[li].type == VSC_LINK_STREAM) {
                        uint8_t src = pipeline->links[li].src_entity;
                        if (pipeline->entities[src].entity_class == VSC_ENTITY_SENSOR)
                            sensor_e = &pipeline->entities[src];
                        break;
                    }
                }
            }
            if (!sensor_e) {
                /* try finding any SENSOR */
                for (i = 0; i < pipeline->num_entities; i++) {
                    if (pipeline->entities[i].entity_class == VSC_ENTITY_SENSOR) {
                        sensor_e = &pipeline->entities[i];
                        break;
                    }
                }
            }

            if (!sensor_e) continue;

            uint32_t smax_w = sensor_e->transform_desc.params.crop.max_w;
            uint32_t smax_h = sensor_e->transform_desc.params.crop.max_h;
            uint32_t smin_w = sensor_e->transform_desc.params.crop.min_w;
            uint32_t smin_h = sensor_e->transform_desc.params.crop.min_h;

            if (c.width_range.lo  > smax_w || c.width_range.hi  < smin_w ||
                c.height_range.lo > smax_h || c.height_range.hi < smin_h) {
                if (smax_w > best_reachable.width)
                    best_reachable.width  = smax_w;
                if (smax_h > best_reachable.height)
                    best_reachable.height = smax_h;
                continue;
            }

            /* format check */
            if (sensor_e->transform_desc.type == VSC_TRANSFORM_PIXEL_FMT_CONV &&
                c.required_format != VSC_FMT_INVALID &&
                c.required_format != sensor_e->transform_desc.params.pixel_fmt_conv.fmt_in) {
                continue;
            }
        }

        /* this endpoint path is feasible */
        any_feasible = true;
        break; /* at least one works */
    }

    if (!any_feasible) {
        result->status = VSC_NEGOTIATE_FAILED;
        if (best_reachable.width > 0) {
            result->reachable_max = best_reachable;
        }
        return VSC_ERR_UNREACHABLE;
    }

    return VSC_OK;

    /* ── fallback paths (no endpoints / linear chain) ── */
fail_no_endpoints:
    result->status = VSC_NEGOTIATE_FAILED;
    return VSC_ERR_UNREACHABLE;

check_sensor_linear:
    /* duplicate sensor-check from above for the no-endpoints fallback */
    {
        vsc_feasibility_constraint_t c;
        constraint_from_intent(&c, intent);

        for (i = pipeline->num_exec_order; i > 0; i--) {
            uint8_t idx = pipeline->execution_order[i - 1];
            const vsc_entity_t *e = &pipeline->entities[idx];
            if (e->entity_class == VSC_ENTITY_SENSOR) break;
            if (feasibility_inverse_one(&e->transform_desc, &c) != 0) {
                result->status = VSC_NEGOTIATE_FAILED;
                return VSC_ERR_UNREACHABLE;
            }
        }

        for (i = 0; i < pipeline->num_entities; i++) {
            const vsc_entity_t *e = &pipeline->entities[i];
            if (e->entity_class != VSC_ENTITY_SENSOR) continue;
            uint32_t smax_w = e->transform_desc.params.crop.max_w;
            uint32_t smax_h = e->transform_desc.params.crop.max_h;
            uint32_t smin_w = e->transform_desc.params.crop.min_w;
            uint32_t smin_h = e->transform_desc.params.crop.min_h;
            if (c.width_range.lo  > smax_w || c.width_range.hi  < smin_w ||
                c.height_range.lo > smax_h || c.height_range.hi < smin_h) {
                result->status = VSC_NEGOTIATE_FAILED;
                result->reachable_max.width  = smax_w;
                result->reachable_max.height = smax_h;
                return VSC_ERR_UNREACHABLE;
            }
            break;
        }
    }

    return VSC_OK;
}

/* ========================================================================
 *  Phase 2 — 正向传播（沿 STREAM 链路的 BFS）
 * ======================================================================== */

/**
 * @brief Phase 2 正向传播：从传感器沿 STREAM 链路 BFS 传播格式
 * @param[in,out] pipeline 管线（会修改每个 Entity 的 prop_state）
 * @param[in]     intent   用户期望的输出格式
 * @return VSC_OK 或 VSC_ERR_NO_SENSOR / VSC_ERR_TOPOLOGY_BROKEN
 * @details 采用 **BFS 算法**（广度优先搜索）沿 STREAM 链路传播格式：
 *          **初始化阶段**
 *          重置所有实体的 prop_state（sink_fmt/source_fmt 设为 INVALID，
 *          active=true，visited=false）。
 *          **根节点发现**
 *          找到所有 SENSOR 实体作为 BFS 的根节点。对每个 SENSOR：
 *          - 如果有 try_fmt_source 回调，调用之获取传感器输出格式
 *          - 否则直接将 intent 赋值给 source_fmt
 *          - 标记 visited，入队
 *          - 如果没有 SENSOR，返回 VSC_ERR_NO_SENSOR
 *          **BFS 主循环**
 *          对队首节点 u，遍历其所有下游邻居：
 *          - **STREAM 链路**：
 *            Step A+B: 将 u 的 source_fmt 传给下游的 try_fmt_sink，
 *            获取下游的 sink_fmt。失败则标记 inactive。
 *            Step C: 如果下游是 STREAM 实体，调用 try_fmt_source 计算
 *            其输出格式；否则 source_fmt = sink_fmt
 *            Step D: 将下游入队（若未访问过）
 *          - **TAP 链路**：
 *            仅做被动验证——调用 try_fmt_sink 检查 TAP 是否接受该格式，
 *            但不入队（TAP 不传播格式）。
 *          **完成检查**
 *          确保所有 STREAM/ENDPOINT 实体均被访问，否则返回
 *          VSC_ERR_TOPOLOGY_BROKEN。
 * @note 每次调用 vsc_resolver_try_fmt() 都会重新执行本函数
 */
int vsc_resolver_forward_propagate(vsc_pipeline_t *pipeline,
                                   const vsc_mbus_fmt_t *intent)
{
    uint8_t i;
    uint8_t queue[VSC_MAX_ENTITIES];
    uint8_t q_head = 0, q_tail = 0;
    bool    in_queue[VSC_MAX_ENTITIES];

    /* ── reset all propagation states ── */
    for (i = 0; i < pipeline->num_entities; i++) {
        vsc_entity_t *e = &pipeline->entities[i];
        memset(&e->prop_state, 0, sizeof(e->prop_state));
        e->prop_state.active  = true;
        e->prop_state.sink_fmt.pixel_format   = VSC_FMT_INVALID;
        e->prop_state.source_fmt.pixel_format = VSC_FMT_INVALID;
    }
    memset(in_queue, 0, sizeof(in_queue));

    /* ── find SENSOR entities as BFS roots ── */
    for (i = 0; i < pipeline->num_entities; i++) {
        vsc_entity_t *e = &pipeline->entities[i];
        if (e->entity_class != VSC_ENTITY_SENSOR) continue;

        if (e->ops && e->ops->try_fmt_source) {
            e->ops->try_fmt_source(e->drv_ctx, intent, &e->prop_state.source_fmt);
        } else {
            e->prop_state.source_fmt = *intent;
        }
        e->prop_state.visited = true;
        queue[q_tail++] = i;
        in_queue[i] = true;
    }

    if (q_tail == 0)
        return VSC_ERR_NO_SENSOR;

    /* ── BFS main loop ── */
    while (q_head < q_tail) {
        uint8_t u = queue[q_head++];
        vsc_entity_t *src = &pipeline->entities[u];

        if (!vsc_fmt_is_valid(&src->prop_state.source_fmt))
            continue;  /* source entity failed; downstream handled below */

        const vsc_mbus_fmt_t *out_fmt = &src->prop_state.source_fmt;

        for (i = 0; i < pipeline->adj_count[u]; i++) {
            uint8_t         v   = pipeline->adj_dst[u][i];
            vsc_link_type_t ltype = pipeline->adj_type[u][i];
            vsc_entity_t   *dst  = &pipeline->entities[v];

            if (ltype == VSC_LINK_STREAM) {
                /* Step A+B: pass to sink pad & validate */
                if (dst->ops && dst->ops->try_fmt_sink) {
                    int rc = dst->ops->try_fmt_sink(dst->drv_ctx, out_fmt,
                                                    &dst->prop_state.sink_fmt);
                    if (rc != VSC_OK || !vsc_fmt_is_valid(&dst->prop_state.sink_fmt)) {
                        dst->prop_state.active = false;
                        continue;
                    }
                } else {
                    dst->prop_state.sink_fmt = *out_fmt;
                }

                /* Step C: if entity has source pad, compute output */
                if (dst->entity_class == VSC_ENTITY_STREAM) {
                    if (dst->ops && dst->ops->try_fmt_source) {
                        dst->ops->try_fmt_source(dst->drv_ctx,
                                                 &dst->prop_state.sink_fmt,
                                                 &dst->prop_state.source_fmt);
                    } else {
                        dst->prop_state.source_fmt = dst->prop_state.sink_fmt;
                    }
                }

                /* enqueue if not visited */
                if (!in_queue[v]) {
                    dst->prop_state.visited = true;
                    queue[q_tail++] = v;
                    in_queue[v] = true;
                }
            }
            else if (ltype == VSC_LINK_TAP) {
                /* TAP: passive validation only */
                if (dst->ops && dst->ops->try_fmt_sink) {
                    int rc = dst->ops->try_fmt_sink(dst->drv_ctx, out_fmt,
                                                    &dst->prop_state.sink_fmt);
                    if (rc == VSC_OK && vsc_fmt_is_valid(&dst->prop_state.sink_fmt)) {
                        dst->prop_state.active = true;
                    } else {
                        dst->prop_state.active = false;
                    }
                }
                /* TAP is never enqueued — no format propagation */
            }
        }
    }

    /* ── check all STREAM / ENDPOINT entities were visited ── */
    for (i = 0; i < pipeline->num_entities; i++) {
        vsc_entity_t *e = &pipeline->entities[i];
        if ((e->entity_class == VSC_ENTITY_STREAM ||
             e->entity_class == VSC_ENTITY_ENDPOINT) &&
            !e->prop_state.visited) {
            return VSC_ERR_TOPOLOGY_BROKEN;
        }
    }

    return VSC_OK;
}

/* ========================================================================
 *  adjustment_trace helpers — 格式变更检测与轨迹记录
 * ======================================================================== */

/**
 * @brief 检测 sink 到 source 之间哪个格式字段发生了变化
 * @param[in] sink   输入端格式
 * @param[in] source 输出端格式
 * @return 第一个变化的字段 ID，无变化返回 VSC_FMT_FIELD_NONE
 * @details 按 width → height → pixel_format → framerate 的优先级顺序检测，
 *          返回第一个不匹配的字段。
 */
static vsc_fmt_field_t detect_field_change(const vsc_mbus_fmt_t *sink,
                                           const vsc_mbus_fmt_t *source)
{
    if (sink->width        != source->width)        return VSC_FMT_FIELD_WIDTH;
    if (sink->height       != source->height)       return VSC_FMT_FIELD_HEIGHT;
    if (sink->pixel_format != source->pixel_format) return VSC_FMT_FIELD_FORMAT;
    if (sink->frame_rate_num != source->frame_rate_num ||
        sink->frame_rate_den != source->frame_rate_den)
        return VSC_FMT_FIELD_FRAMERATE;
    return VSC_FMT_FIELD_NONE;
}

/**
 * @brief 根据 Entity 的变换类型推断格式调整的原因分类
 * @param[in] e     Entity 指针
 * @param[in] field 发生变化的格式字段
 * @return 调整原因枚举值
 * @details BINNING → VSC_ADJUST_HALVE（halve 语义不准确，实际为缩放），
 *          PIXEL_FMT_CONV → VSC_ADJUST_FORMAT_CONV，
 *          CROP/其他 → VSC_ADJUST_CLAMP。
 */
static vsc_adjust_reason_t infer_reason(const vsc_entity_t *e,
                                        vsc_fmt_field_t field)
{
    if (field == VSC_FMT_FIELD_NONE) return VSC_ADJUST_NONE;

    switch (e->transform_desc.type) {
    case VSC_TRANSFORM_BINNING:
        return VSC_ADJUST_HALVE;
    case VSC_TRANSFORM_PIXEL_FMT_CONV:
        return VSC_ADJUST_FORMAT_CONV;
    case VSC_TRANSFORM_CROP:
        return VSC_ADJUST_CLAMP;
    default:
        return VSC_ADJUST_CLAMP;
    }
}

/**
 * @brief 向调整轨迹中添加一条记录
 * @param[in,out] trace  轨迹容器
 * @param[in]     e      相关 Entity
 * @param[in]     pad    Pad 名称（"SINK" 或 "SOURCE"）
 * @param[in]     field  变化的字段
 * @param[in]     orig   变化前的格式值
 * @param[in]     adj    变化后的格式值
 * @param[in]     reason 调整原因
 * @param[in]     detail 原因的文字说明
 * @details 轨迹条目达到 VSC_MAX_TRACE_ENTRIES 上限后静默丢弃后续记录。
 */
static void add_trace_entry(vsc_adjustment_trace_t *trace,
                            const vsc_entity_t *e,
                            const char *pad,
                            vsc_fmt_field_t field,
                            const vsc_mbus_fmt_t *orig,
                            const vsc_mbus_fmt_t *adj,
                            vsc_adjust_reason_t reason,
                            const char *detail)
{
    if (trace->num_entries >= VSC_MAX_TRACE_ENTRIES) return;
    vsc_adjustment_entry_t *entry = &trace->entries[trace->num_entries++];

    memset(entry, 0, sizeof(*entry));
    strncpy(entry->entity_name, e->name, VSC_MAX_ENTITY_NAME - 1);
    strncpy(entry->pad_name, pad, sizeof(entry->pad_name) - 1);
    entry->field_changed = field;
    entry->original       = *orig;
    entry->adjusted       = *adj;
    entry->reason         = reason;
    strncpy(entry->reason_detail, detail, sizeof(entry->reason_detail) - 1);
}

/* ========================================================================
 *  Phase 3 — 收敛（收集端点格式 + 构建调整轨迹 + 判定协商状态）
 * ======================================================================== */

/**
 * @brief Phase 3 收敛：收集端点格式、构建调整轨迹、判定协商状态
 * @param[in]  pipeline 已完成正向传播的管线
 * @param[in]  intent   用户原始意图格式
 * @param[out] result   协商结果（填充 primary_fmt、endpoint_fmts、trace、status）
 * @return VSC_OK 或 VSC_ERR_NO_ACTIVE_ENDPOINT
 * @details 三个子阶段：
 *          **子阶段 1 — 端点格式收集**
 *          - 有显式端点：遍历 endpoints[]，收集所有 active 且格式有效的端点
 *          - 无显式端点（回退）：从 execution_order 逆序查找最后一个
 *            STREAM 实体，取其 source_fmt（优先）或 sink_fmt 作为主输出
 *          - 第一个有效端点即设为主输出（primary_fmt）
 *          - 无任何有效端点时返回 VSC_ERR_NO_ACTIVE_ENDPOINT
 *          **子阶段 2 — 调整轨迹构建**
 *          沿 execution_order 遍历，对每个 Entity 的 SOURCE pad
 *          检测 sink → source 之间的格式变化，记录到 trace 中。
 *          同时检查所有 TAP 观察者：active=false 的 TAP 记录一条
 *          VSC_ADJUST_REJECTED 条目。
 *          **子阶段 3 — 协商状态判定**
 *          - primary_fmt == intent → VSC_NEGOTIATE_EXACT
 *          - 否则 → VSC_NEGOTIATE_ADJUSTED
 *          - 存在任何 TAP 被拒绝 → 降级为 VSC_NEGOTIATE_PARTIAL
 * @see vsc_resolver_forward_propagate() 必须先执行
 */
int vsc_resolver_converge(vsc_pipeline_t *pipeline,
                          const vsc_mbus_fmt_t *intent,
                          vsc_resolver_result_t *result)
{
    uint8_t i;
    bool    has_primary = false;

    result->num_endpoints = 0;
    result->trace.num_entries = 0;

    /* ── collect endpoint formats ── */
    if (pipeline->num_endpoints > 0) {
        for (i = 0; i < pipeline->num_endpoints; i++) {
            uint8_t idx = pipeline->endpoints[i];
            vsc_entity_t *ep = &pipeline->entities[idx];

            if (!ep->prop_state.active || !vsc_fmt_is_valid(&ep->prop_state.sink_fmt))
                continue;

            if (result->num_endpoints < VSC_MAX_ENDPOINTS) {
                vsc_endpoint_fmt_t *ef = &result->endpoint_fmts[result->num_endpoints++];
                strncpy(ef->entity_name, ep->name, VSC_MAX_ENTITY_NAME - 1);
                ef->final_fmt = ep->prop_state.sink_fmt;
                ef->active    = true;

                if (!has_primary) {
                    result->primary_fmt = ef->final_fmt;
                    strncpy(result->primary_endpoint, ep->name,
                            VSC_MAX_ENTITY_NAME - 1);
                    has_primary = true;
                }
            }
        }
    } else {
        /* no explicit ENDPOINT — use the last STREAM entity in execution_order */
        for (i = pipeline->num_exec_order; i > 0; i--) {
            uint8_t idx = pipeline->execution_order[i - 1];
            vsc_entity_t *ep = &pipeline->entities[idx];
            if (ep->entity_class == VSC_ENTITY_STREAM &&
                ep->prop_state.active) {
                /* use source_fmt if available (last node outputs), else sink_fmt */
                const vsc_mbus_fmt_t *fmt = vsc_fmt_is_valid(&ep->prop_state.source_fmt)
                    ? &ep->prop_state.source_fmt : &ep->prop_state.sink_fmt;
                if (!vsc_fmt_is_valid(fmt)) continue;
                result->primary_fmt = *fmt;
                strncpy(result->primary_endpoint, ep->name,
                        VSC_MAX_ENTITY_NAME - 1);
                if (result->num_endpoints < VSC_MAX_ENDPOINTS) {
                    vsc_endpoint_fmt_t *ef = &result->endpoint_fmts[result->num_endpoints++];
                    strncpy(ef->entity_name, ep->name, VSC_MAX_ENTITY_NAME - 1);
                    ef->final_fmt = *fmt;
                    ef->active = true;
                }
                has_primary = true;
                break;
            }
        }
    }

    if (!has_primary) {
        result->status = VSC_NEGOTIATE_FAILED;
        return VSC_ERR_NO_ACTIVE_ENDPOINT;
    }

    /* ── build adjustment trace ── */
    for (i = 0; i < pipeline->num_exec_order; i++) {
        uint8_t idx = pipeline->execution_order[i];
        vsc_entity_t *e = &pipeline->entities[idx];

        if (!e->prop_state.visited) continue;

        /* SOURCE pad entry */
        if (vsc_fmt_is_valid(&e->prop_state.source_fmt)) {
            vsc_fmt_field_t field = detect_field_change(
                &e->prop_state.sink_fmt, &e->prop_state.source_fmt);
            vsc_adjust_reason_t reason = infer_reason(e, field);
            add_trace_entry(&result->trace, e, "SOURCE", field,
                            &e->prop_state.sink_fmt,
                            &e->prop_state.source_fmt,
                            reason, "");
        }
    }

    /* ── TAP status ── */
    for (i = 0; i < pipeline->num_taps; i++) {
        uint8_t idx = pipeline->tap_observers[i];
        vsc_entity_t *tap = &pipeline->entities[idx];

        if (!tap->prop_state.active) {
            add_trace_entry(&result->trace, tap, "SINK",
                            VSC_FMT_FIELD_FORMAT,
                            &tap->prop_state.sink_fmt,
                            &tap->prop_state.sink_fmt,
                            VSC_ADJUST_REJECTED,
                            "TAP inactive: format not supported");
        }
    }

    /* ── determine negotiation status ── */
    if (vsc_fmt_equal(&result->primary_fmt, intent)) {
        result->status = VSC_NEGOTIATE_EXACT;
    } else {
        result->status = VSC_NEGOTIATE_ADJUSTED;
    }

    /* check for TAP failures that degrade to PARTIAL */
    for (i = 0; i < result->trace.num_entries; i++) {
        if (result->trace.entries[i].reason == VSC_ADJUST_REJECTED) {
            result->status = VSC_NEGOTIATE_PARTIAL;
            break;
        }
    }

    return VSC_OK;
}

/* ========================================================================
 *  Phase 4 — 硬件提交（将协商结果写入 FPGA 寄存器）
 * ======================================================================== */

/**
 * @brief Phase 4 提交：将最终协商格式写入各实体的硬件寄存器
 * @param[in,out] pipeline  管线
 * @param[in]     final_fmt 协商确定的最终格式
 * @return VSC_OK 或 VSC_ERR_BUSY / VSC_ERR_COMMIT_FAILED
 * @details 沿 execution_order（SENSOR → … → ENDPOINT）遍历，
 *          对每个有 commit_fmt 回调的实体调用之。
 *          - 若管线当前处于 STREAMING 状态，拒绝提交（VSC_ERR_BUSY）
 *          - 任何实体的 commit_fmt 失败会导致管线标记为 DIRTY，
 *            且不执行回滚（硬件状态不确定）
 *          - 全部成功则将管线状态设为 CONFIGURED
 * @warning 部分提交失败不回滚——调用者应在重试前重建管线
 */
int vsc_pipeline_commit_fmt(vsc_pipeline_t *pipeline,
                            const vsc_mbus_fmt_t *final_fmt)
{
    uint8_t i;

    if (pipeline->state == VSC_PIPELINE_STREAMING)
        return VSC_ERR_BUSY;

    /* ── walk execution_order (SENSOR → … → ENDPOINT) ── */
    for (i = 0; i < pipeline->num_exec_order; i++) {
        uint8_t idx = pipeline->execution_order[i];
        vsc_entity_t *e = &pipeline->entities[idx];

        /* skip entities without a commit callback */
        if (!e->ops || !e->ops->commit_fmt) continue;

        int rc = e->ops->commit_fmt(e->drv_ctx, final_fmt);
        if (rc != VSC_OK) {
            pipeline->state = VSC_PIPELINE_DIRTY;
            return VSC_ERR_COMMIT_FAILED;
        }
    }

    pipeline->state = VSC_PIPELINE_CONFIGURED;
    return VSC_OK;
}

/* ========================================================================
 *  完整 try_fmt 管线（Phase 1 → 2 → 3，不含 commit）
 * ======================================================================== */

/**
 * @brief 完整格式协商管线：Phase 1（可行性）→ Phase 2（正向传播）→ Phase 3（收敛）
 * @param[in]  pipeline 已构建的管线
 * @param[in]  intent   用户期望的输出格式
 * @param[out] result   协商结果（含状态、主格式、端点格式、轨迹）
 * @return VSC_OK 或负值错误码
 * @details 三阶段流水线：
 *          1. **意图校验** — width/height/pixel_format 合法性检查
 *          2. **Phase 1** — 可行性逆传播检查，不可达时直接返回
 *             （result.reachable_max 已填充）
 *          3. **Phase 2** — BFS 正向传播，填充各实体的 prop_state
 *          4. **Phase 3** — 收敛：收集端点格式、构建轨迹、判定状态
 * @note 本函数不执行硬件提交（commit），调用者需显式调用
 *        vsc_pipeline_commit_fmt() 将结果写入硬件。
 * @see vsc_resolver_feasibility_check()
 * @see vsc_resolver_forward_propagate()
 * @see vsc_resolver_converge()
 * @see vsc_pipeline_commit_fmt()
 */
int vsc_resolver_try_fmt(vsc_pipeline_t *pipeline,
                         const vsc_mbus_fmt_t *intent,
                         vsc_resolver_result_t *result)
{
    int rc;

    memset(result, 0, sizeof(*result));

    /* ── validate intent ── */
    if (intent->width == 0 || intent->height == 0 ||
        intent->pixel_format == VSC_FMT_INVALID) {
        result->status = VSC_NEGOTIATE_FAILED;
        return VSC_ERR_INVALID_INTENT;
    }

    /* ── Phase 1: feasibility check ── */
    rc = vsc_resolver_feasibility_check(pipeline, intent, result);
    if (rc == VSC_ERR_UNREACHABLE)
        return rc;   /* result.reachable_max is populated */

    /* ── Phase 2: forward propagation ── */
    rc = vsc_resolver_forward_propagate(pipeline, intent);
    if (rc != VSC_OK) {
        result->status = VSC_NEGOTIATE_FAILED;
        return rc;
    }

    /* ── Phase 3: convergence ── */
    rc = vsc_resolver_converge(pipeline, intent, result);
    return rc;
}
