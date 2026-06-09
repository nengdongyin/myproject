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
#include <string.h>

/* ========================================================================
 *  Internal helpers — feasibility inverse transforms
 * ======================================================================== */

/**
 * @brief Apply a PASS_THROUGH inverse: constraint unchanged.
 */
static void iv_apply_pass_through(vsc_feasibility_constraint_t *c)
{
    (void)c;  /* nothing to do */
}

/**
 * @brief Apply a BINNING inverse: multiply width/height ranges by factor.
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
 * @brief Apply a CROP inverse: widen constraint conservatively.
 *
 * A CROP entity can only reduce (or pass through) dimensions.  To guarantee
 * output in [lo, hi], the input must be >= effective_lo (at minimum) and
 * <= crop_max (upper bound of the crop hardware).
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
 * @brief Apply a PIXEL_FMT_CONV inverse: if output fmt matches fmt_out,
 *        require input fmt to be fmt_in.
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
 * @brief Run full inverse transform for one entity's transform_desc.
 * @return 0 on success, -1 if constraint becomes empty.
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
 *  Pipeline build — topological sort (Kahn's algorithm, STREAM links only)
 * ======================================================================== */

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

/* ── helper: init constraint from intent ── */
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
 *  Phase 1 — Feasibility check (per-endpoint reverse path)
 * ======================================================================== */

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
 *  Phase 2 — Forward propagation (BFS along STREAM links)
 * ======================================================================== */

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
 *  adjustment_trace helpers
 * ======================================================================== */

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
 *  Phase 3 — Convergence
 * ======================================================================== */

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
 *  Phase 4 — Commit
 * ======================================================================== */

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
 *  Full try_fmt pipeline
 * ======================================================================== */

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
