/**
 * @file vsc_lite.c
 * @brief VSC Lite 实现 — 线性管线前向传播 + 反向可行性预检 + 独立时序聚合
 *
 * 协商分为四个阶段:
 *   阶段 0 — 反向可行性: endpoint → sensor 逆推，确认用户意图可达
 *   阶段 A — 前向空间传播: sensor → endpoint 协商 width/height/format
 *           sensor 填充时序基准值，中间 stage 透传
 *   阶段 B — 时序聚合: 遍历所有 stage 调用 get_timing_req
 *           并行约束取 max，串行延迟取 sum，合并 sensor 基准
 *   阶段 C — 收敛: 组装最终 vsc_mbus_fmt_t 写入 result
 */

#include "vsc_lite.h"
#include <string.h>
#include <stdio.h>

#ifndef VSC_LITE_DEBUG_TIMING
#define VSC_LITE_DEBUG_TIMING 0
#endif

/* ================================================================
 *  helper — 时序字段从 sink 透传到 source
 *          类型系统保证只操作 vsc_timing_fmt_t
 * ================================================================ */

static void vsc_lite_copy_timing(const vsc_timing_fmt_t *src,
                                 vsc_timing_fmt_t *dst)
{
    *dst = *src;
}

/* ================================================================
 *  阶段 0 — 反向可行性预检
 *
 *  从 endpoint 逆推到 sensor，检查用户 intent 是否可达。
 *  只读：不修改任何 stage 状态。成功时返回 sensor 所需的最小尺寸。
 * ================================================================ */

static int vsc_lite_feasibility_check(const vsc_lite_pipeline_t *pipe,
                                      const vsc_mbus_fmt_t *intent,
                                      uint32_t *out_w_min, uint32_t *out_h_min)
{
    uint32_t w = intent->spatial.width;
    uint32_t h = intent->spatial.height;

    /* 从 endpoint 前一级逆推到 sensor（跳过 endpoint 自身） */
    for (int i = pipe->count - 2; i >= 0; i--)
    {
        const vsc_driver_t *drv = pipe->stages[i].driver;
        if (!drv || !drv->transform_template)
            continue;

        const vsc_fmt_transform_desc_t *td = drv->transform_template;
        switch (td->type) {
        case VSC_TRANSFORM_BINNING:
            w *= td->params.binning.factor_x;
            h *= td->params.binning.factor_y;
            break;
        case VSC_TRANSFORM_CROP:
            if (w < td->params.crop.min_w) w = td->params.crop.min_w;
            if (h < td->params.crop.min_h) h = td->params.crop.min_h;
            if (w > td->params.crop.max_w || h > td->params.crop.max_h)
                return VSC_ERR_UNREACHABLE;
            break;
        case VSC_TRANSFORM_PIXEL_FMT_CONV:
        case VSC_TRANSFORM_PASS_THROUGH:
            break;  /* 尺寸不变 */
        default:
            break;
        }
    }

    /* 检查 sensor 能力（通过其 transform_template 的 crop 约束或 max */
    if (pipe->count > 0 && pipe->stages[0].driver
        && pipe->stages[0].driver->transform_template)
    {
        const vsc_fmt_transform_desc_t *td = pipe->stages[0].driver->transform_template;
        if ((td->type == VSC_TRANSFORM_CROP || td->type == VSC_TRANSFORM_PASS_THROUGH)
            && (w > td->params.crop.max_w || h > td->params.crop.max_h))
            return VSC_ERR_UNREACHABLE;
    }

    *out_w_min = w;
    *out_h_min = h;
    return VSC_OK;
}

/* ================================================================
 *  阶段 A — 前向空间传播
 *
 *  sensor → endpoint 逐级协商 spatial 字段。
 *  sensor 同时填充时序基准值到 source_fmt.timing。
 *  返回 endpoint 的 sink 格式（最终输出空间格式）。
 * ================================================================ */

static int vsc_lite_forward_spatial(vsc_lite_pipeline_t *pipe,
                                    const vsc_mbus_fmt_t *sensor_intent,
                                    vsc_mbus_fmt_t *out_endpoint_sink)
{
    /* ── Stage 0: sensor ── */
    vsc_lite_stage_t *sensor = &pipe->stages[0];
    const vsc_ip_ops_t *ops = sensor->driver ? &sensor->driver->ops : NULL;

    if (ops && ops->try_fmt_source)
    {
        int rc = ops->try_fmt_source(sensor->drv_ctx, sensor_intent,
                                     &sensor->source_fmt);
        if (rc != VSC_OK || !vsc_fmt_is_valid(&sensor->source_fmt))
            return rc != VSC_OK ? rc : VSC_ERR_PARAM;
    }
    else
    {
        sensor->source_fmt = *sensor_intent;
    }

    const vsc_mbus_fmt_t *upstream = &sensor->source_fmt;

    /* ── Stage 1..N-2: processing stages ── */
    for (uint8_t i = 1; i < pipe->count - 1; i++)
    {
        vsc_lite_stage_t *st = &pipe->stages[i];
        ops = st->driver ? &st->driver->ops : NULL;

        if (ops && ops->try_fmt_sink)
        {
            int rc = ops->try_fmt_sink(st->drv_ctx, upstream, &st->sink_fmt);
            if (rc != VSC_OK || !vsc_fmt_is_valid(&st->sink_fmt))
                return rc != VSC_OK ? rc : VSC_ERR_PARAM;
        }
        else
        {
            st->sink_fmt = *upstream;
        }

        if (ops && ops->try_fmt_source)
        {
            int rc = ops->try_fmt_source(st->drv_ctx, &st->sink_fmt,
                                         &st->source_fmt);
            if (rc != VSC_OK || !vsc_fmt_is_valid(&st->source_fmt))
                return rc != VSC_OK ? rc : VSC_ERR_PARAM;
        }
        else
        {
            st->source_fmt = st->sink_fmt;
        }

        /* 时序从 sink 透传到 source，active 同步到当前输出尺寸 */
        vsc_lite_copy_timing(&st->sink_fmt.timing, &st->source_fmt.timing);
        st->source_fmt.timing.h_active = st->source_fmt.spatial.width;
        st->source_fmt.timing.v_active = st->source_fmt.spatial.height;

        upstream = &st->source_fmt;
    }

    /* ── Last stage: endpoint (sink only) ── */
    vsc_lite_stage_t *ep = &pipe->stages[pipe->count - 1];
    ops = ep->driver ? &ep->driver->ops : NULL;

    if (ops && ops->try_fmt_sink)
    {
        int rc = ops->try_fmt_sink(ep->drv_ctx, upstream, &ep->sink_fmt);
        if (rc != VSC_OK || !vsc_fmt_is_valid(&ep->sink_fmt))
            return rc != VSC_OK ? rc : VSC_ERR_PARAM;
        *out_endpoint_sink = ep->sink_fmt;
    }
    else
    {
        *out_endpoint_sink = *upstream;
    }
    return VSC_OK;
}

/* ================================================================
 *  阶段 B — 时序聚合
 * ================================================================ */

static int vsc_lite_aggregate_timing(const vsc_lite_pipeline_t *pipe,
                                     vsc_timing_fmt_t *out_timing,
                                     uint32_t *out_max_h_total,
                                     uint32_t *out_max_v_total)
{
    vsc_timing_req_t agg;
    uint32_t latency_lines = 0;

    memset(&agg, 0, sizeof(agg));

    for (uint8_t i = 0; i < pipe->count; i++)
    {
        const vsc_lite_stage_t *st = &pipe->stages[i];
        const vsc_ip_ops_t *ops = st->driver ? &st->driver->ops : NULL;
        if (!ops || !ops->get_timing_req)
            continue;

        vsc_timing_req_t req;
        memset(&req, 0, sizeof(req));

        const vsc_mbus_fmt_t *src_ref = &st->source_fmt;
        if (i == pipe->count - 1 && !vsc_fmt_is_valid(src_ref))
            src_ref = &st->sink_fmt;

        int rc = ops->get_timing_req(st->drv_ctx,
                                     &st->sink_fmt, src_ref, &req);
        if (rc != VSC_OK) return rc;

        if (req.min_h_total  > agg.min_h_total)  agg.min_h_total  = req.min_h_total;
        if (req.min_h_blank  > agg.min_h_blank)  agg.min_h_blank  = req.min_h_blank;
        if (req.min_v_total  > agg.min_v_total)  agg.min_v_total  = req.min_v_total;
        if (req.min_v_blank  > agg.min_v_blank)  agg.min_v_blank  = req.min_v_blank;

        latency_lines += req.pipeline_lines;

        if (i == 0)
        {
            *out_max_h_total = req.reserved[0];
            *out_max_v_total = req.reserved[1];
        }

#if VSC_LITE_DEBUG_TIMING
        printf("[VSC-Lite timing] stage[%u]%-16s ip_clk=%-8u h_t=%-6u h_b=%-6u v_t=%-6u v_b=%-6u pipe=%u\n",
               i, st->driver ? st->driver->name : "(null)",
               req.ip_clock_hz, req.min_h_total, req.min_h_blank,
               req.min_v_total, req.min_v_blank, req.pipeline_lines);
#endif
    }

    /* ── 合并 sensor 基准 ── */
    const vsc_timing_fmt_t *base = &pipe->stages[0].source_fmt.timing;

    *out_timing = *base;

    if (agg.min_h_total > out_timing->h_total)
        out_timing->h_total = agg.min_h_total;

    {
        uint32_t hb = out_timing->h_total - out_timing->h_active;
        if (base->h_blank > hb)      hb = base->h_blank;
        if (agg.min_h_blank > hb)    hb = agg.min_h_blank;
        out_timing->h_blank = hb;
        out_timing->h_total = out_timing->h_active + out_timing->h_blank;
    }

    {
        uint32_t vb = base->v_blank;
        if (agg.min_v_blank > vb)    vb = agg.min_v_blank;
        if (latency_lines   > vb)    vb = latency_lines;
        out_timing->v_blank = vb;
    }

    {
        uint32_t vt = base->v_total;
        if (agg.min_v_total > vt)                  vt = agg.min_v_total;
        if (out_timing->v_active + out_timing->v_blank > vt)
            vt = out_timing->v_active + out_timing->v_blank;
        out_timing->v_total = vt;
    }

    return VSC_OK;
}

/* ================================================================
 *  公开 API
 * ================================================================ */

int vsc_lite_pipeline_init(vsc_lite_pipeline_t *pipe,
                           const vsc_lite_stage_def_t *stages, uint8_t count)
{
    if (!pipe || !stages || count < 2 || count > VSC_LITE_MAX_STAGES)
        return VSC_ERR_PARAM;

    memset(pipe, 0, sizeof(*pipe));
    pipe->count = count;

    for (uint8_t i = 0; i < count; i++)
    {
        const vsc_driver_t *drv = stages[i].driver;
        pipe->stages[i].driver  = drv;
        pipe->stages[i].drv_ctx = stages[i].inst;   /* 编译期静态实例 */

        if (drv && drv->ops.init)
        {
            int rc = drv->ops.init(stages[i].inst);
            if (rc != VSC_OK) return rc;
        }
    }
    return VSC_OK;
}

int vsc_lite_try_fmt(vsc_lite_pipeline_t *pipe,
                     const vsc_mbus_fmt_t *intent,
                     vsc_resolver_result_t *result)
{
    if (!pipe || !intent || !result || pipe->count < 2)
        return VSC_ERR_PARAM;

    memset(result, 0, sizeof(*result));

    /* ── 清零所有 stage 格式 ── */
    for (uint8_t i = 0; i < pipe->count; i++)
    {
        memset(&pipe->stages[i].sink_fmt,   0, sizeof(vsc_mbus_fmt_t));
        memset(&pipe->stages[i].source_fmt, 0, sizeof(vsc_mbus_fmt_t));
    }

    /* ================================================================
     *  阶段 0 — 反向可行性预检
     * ================================================================ */

    uint32_t sensor_w, sensor_h;
    int rc = vsc_lite_feasibility_check(pipe, intent, &sensor_w, &sensor_h);
    if (rc != VSC_OK)
    {
        result->status = VSC_NEGOTIATE_FAILED;
        return rc;
    }

    /* 构造 sensor 的 intent：用逆推出的最小尺寸（不小于用户原始意图） */
    vsc_mbus_fmt_t sensor_intent = *intent;
    if (sensor_w > sensor_intent.spatial.width)
        sensor_intent.spatial.width  = sensor_w;
    if (sensor_h > sensor_intent.spatial.height)
        sensor_intent.spatial.height = sensor_h;

    /* ================================================================
     *  阶段 A — 前向空间传播
     * ================================================================ */

    vsc_mbus_fmt_t endpoint_sink;
    rc = vsc_lite_forward_spatial(pipe, &sensor_intent, &endpoint_sink);
    if (rc != VSC_OK)
    {
        result->status = VSC_NEGOTIATE_FAILED;
        return rc;
    }
    result->primary_fmt = endpoint_sink;

    /* ================================================================
     *  阶段 B — 时序聚合
     * ================================================================ */

    vsc_timing_fmt_t final_timing;
    uint32_t sensor_max_h_total = 0;
    uint32_t sensor_max_v_total = 0;

    rc = vsc_lite_aggregate_timing(pipe, &final_timing,
                                   &sensor_max_h_total, &sensor_max_v_total);
    if (rc != VSC_OK)
    {
        result->status = VSC_NEGOTIATE_FAILED;
        return rc;
    }

    if ((sensor_max_h_total > 0 && final_timing.h_total > sensor_max_h_total) ||
        (sensor_max_v_total > 0 && final_timing.v_total > sensor_max_v_total))
    {
        result->status = VSC_NEGOTIATE_FAILED;
        result->reachable_max.spatial = endpoint_sink.spatial;
        result->reachable_max.timing  = final_timing;
        return VSC_ERR_UNREACHABLE;
    }

    /* ================================================================
     *  阶段 C — 收敛
     * ================================================================ */

    result->primary_fmt.timing = final_timing;
    result->primary_fmt.timing.h_active = result->primary_fmt.spatial.width;
    result->primary_fmt.timing.v_active = result->primary_fmt.spatial.height;

    if (final_timing.pixel_clock_hz > 0 && final_timing.h_total > 0
        && final_timing.v_total > 0)
    {
        result->primary_fmt.spatial.frame_rate_num = final_timing.pixel_clock_hz;
        result->primary_fmt.spatial.frame_rate_den =
            final_timing.h_total * final_timing.v_total;
    }

    {
        bool exact = (result->primary_fmt.spatial.width        == intent->spatial.width)
                  && (result->primary_fmt.spatial.height       == intent->spatial.height)
                  && (result->primary_fmt.spatial.pixel_format == intent->spatial.pixel_format);
        result->status = exact ? VSC_NEGOTIATE_EXACT : VSC_NEGOTIATE_ADJUSTED;
    }

    return VSC_OK;
}

int vsc_lite_commit_fmt(vsc_lite_pipeline_t *pipe,
                        const vsc_mbus_fmt_t *final_fmt)
{
    if (!pipe || !final_fmt)
        return VSC_ERR_PARAM;

    for (int i = pipe->count - 1; i >= 0; i--)
    {
        vsc_lite_stage_t *st = &pipe->stages[i];
        if (st->driver && st->driver->ops.commit_fmt)
        {
            int rc = st->driver->ops.commit_fmt(st->drv_ctx, final_fmt);
            if (rc != VSC_OK) return rc;
        }
    }
    return VSC_OK;
}

/* ================================================================
 *  能力查询
 * ================================================================ */

int vsc_lite_query_cap(vsc_lite_pipeline_t *pipe, uint32_t cap_id,
                       void *out, uint8_t *out_len)
{
    if (!pipe || !out || !out_len || *out_len == 0)
        return VSC_ERR_PARAM;

    for (uint8_t i = 0; i < pipe->count; i++)
    {
        const vsc_ip_ops_t *ops = pipe->stages[i].driver
                                  ? &pipe->stages[i].driver->ops : NULL;
        if (!ops || !ops->query_cap)
            continue;

        int rc = ops->query_cap(pipe->stages[i].drv_ctx, cap_id, out, out_len);
        if (rc == VSC_OK)
            return VSC_OK;
    }
    return VSC_ERR_NOT_SUPPORTED;
}

/* ================================================================
 *  控制接口 — 按 pipeline 顺序路由到提供该能力的 stage
 * ================================================================ */

int vsc_lite_set_ctrl(vsc_lite_pipeline_t *pipe, uint32_t cap_id,
                      uint32_t ctrl_id, uint32_t value)
{
    if (!pipe) return VSC_ERR_PARAM;

    for (uint8_t i = 0; i < pipe->count; i++)
    {
        const vsc_ip_ops_t *ops = pipe->stages[i].driver
                                  ? &pipe->stages[i].driver->ops : NULL;
        if (!ops || !ops->set_ctrl)
            continue;

        /* 先确认该 stage 提供此能力，再转发控制 */
        if (ops->query_cap) {
            uint8_t cap_buf[64];
            uint8_t len = sizeof(cap_buf);
            memset(cap_buf, 0, sizeof(cap_buf));
            if (ops->query_cap(pipe->stages[i].drv_ctx, cap_id, cap_buf, &len) != VSC_OK
                || !((vsc_cap_header_t *)cap_buf)->available)
                continue;
        }

        int rc = ops->set_ctrl(pipe->stages[i].drv_ctx, ctrl_id, value);
        if (rc == VSC_OK) return VSC_OK;
    }
    return VSC_ERR_NOT_SUPPORTED;
}

int vsc_lite_get_ctrl(vsc_lite_pipeline_t *pipe, uint32_t cap_id,
                      uint32_t ctrl_id, uint32_t *value)
{
    if (!pipe || !value) return VSC_ERR_PARAM;

    for (uint8_t i = 0; i < pipe->count; i++)
    {
        const vsc_ip_ops_t *ops = pipe->stages[i].driver
                                  ? &pipe->stages[i].driver->ops : NULL;
        if (!ops || !ops->get_ctrl)
            continue;

        if (ops->query_cap) {
            uint8_t cap_buf[64];
            uint8_t len = sizeof(cap_buf);
            memset(cap_buf, 0, sizeof(cap_buf));
            if (ops->query_cap(pipe->stages[i].drv_ctx, cap_id, cap_buf, &len) != VSC_OK
                || !((vsc_cap_header_t *)cap_buf)->available)
                continue;
        }

        int rc = ops->get_ctrl(pipe->stages[i].drv_ctx, ctrl_id, value);
        if (rc == VSC_OK) return VSC_OK;
    }
    return VSC_ERR_NOT_SUPPORTED;
}
