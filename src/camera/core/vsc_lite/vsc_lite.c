/**
 * @file vsc_lite.c
 * @brief VSC Lite 实现 — 透传逆推 + sensor 源头解算 + 前向能力认领
 *
 * 协商分为四个阶段:
 *   阶段 0 — 反向 spatial 分发: endpoint → sensor 透传 intent spatial
 *           不做数值解算，bin/crop 标记原样保留
 *           每个 stage 的逆推目标写入 stage->source_fmt.spatial
 *   阶段 A — 前向空间传播: sensor 源头做坐标翻译到 pre-bin 物理空间
 *           sensor → endpoint 逐级 try_fmt，driver 按能力认领标记
 *           sensor 填充时序基准值
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
 *  阶段 0 — 反向 spatial 分发
 *
 *  endpoint → sensor 透传 intent 的 spatial 字段（不做数值解算）。
 *  bin/crop/format 标记原样保留，由 Phase A sensor 源头统一解算。
 *  每 stage 的 source_fmt.spatial 记录各自应产出的目标分辨率。
 *  可行性由 Phase A 各 stage 的 try_fmt 自行判断。
 * ================================================================ */

static int vsc_lite_reverse_spatial(vsc_lite_pipeline_t *pipe,
                                    const vsc_mbus_fmt_t *intent)
{
    vsc_mbus_fmt_t working = *intent;

    for (int i = pipe->count - 2; i >= 0; i--)
    {
        /* ── 粗粒度边界检查（off + w ≤ max），实际可行性由 Phase A 判定 ── */
        const vsc_driver_t *drv = pipe->stages[i].driver;
        if (drv && drv->transform_template
            && drv->transform_template->type == VSC_TRANSFORM_CROP)
        {
            uint32_t max_w = drv->transform_template->params.crop.max_w;
            uint32_t max_h = drv->transform_template->params.crop.max_h;
            uint32_t needed_w = working.spatial.width + working.spatial.offsetx;
            uint32_t needed_h = working.spatial.height + working.spatial.offsety;
            if (max_w > 0 && (needed_w > max_w || needed_h > max_h))
                return VSC_ERR_UNREACHABLE;
        }

        /* ── 透传写入（sensor 源头在 Phase A 做坐标翻译）─── */
        pipe->stages[i].source_fmt.spatial = working.spatial;
    }

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
    /* ── Stage 0: sensor ──
     * 框架将 intent 的 post-scaling 坐标翻译到 pre-scaling 物理空间，
     * sensor 驱动无需理解 bin/dec 语义。 */
    vsc_lite_stage_t *sensor = &pipe->stages[0];
    const vsc_ip_ops_t *ops = sensor->driver ? &sensor->driver->ops : NULL;

    vsc_mbus_fmt_t sensor_req = *sensor_intent;
    uint8_t total_sx = (uint8_t)(sensor_req.spatial.bin_x * sensor_req.spatial.dec_x);
    uint8_t total_sy = (uint8_t)(sensor_req.spatial.bin_y * sensor_req.spatial.dec_y);
    if (total_sx < 1) total_sx = 1;
    if (total_sy < 1) total_sy = 1;
    /* 坐标翻译到 pre-scaling 物理空间（仅乘因子，是否求和由 sensor 决定） */
    sensor_req.spatial.width   *= total_sx;
    sensor_req.spatial.height  *= total_sy;
    sensor_req.spatial.offsetx *= total_sx;
    sensor_req.spatial.offsety *= total_sy;

    if (ops && ops->try_fmt_source)
    {
        int rc = ops->try_fmt_source(sensor->drv_ctx, &sensor_req,
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

        /* ── crop stage: 框架将逆推目标乘上待认领缩放因子 ── */
        if (st->driver && (st->driver->capabilities & VSC_CAP_CROP))
        {
            uint8_t psx = (uint8_t)(st->sink_fmt.spatial.bin_x * st->sink_fmt.spatial.dec_x);
            uint8_t psy = (uint8_t)(st->sink_fmt.spatial.bin_y * st->sink_fmt.spatial.dec_y);
            if (psx < 1) psx = 1;
            if (psy < 1) psy = 1;
            st->source_fmt.spatial.width  *= psx;
            st->source_fmt.spatial.height *= psy;
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
     *  阶段 0 — 反向空间传播（写 stage->source_fmt.spatial）
     * ================================================================ */

    int rc = vsc_lite_reverse_spatial(pipe, intent);
    if (rc != VSC_OK)
    {
        result->status = VSC_NEGOTIATE_FAILED;
        return rc;
    }

    /* sensor_intent = 逆推到达 sensor 时的完整格式（bin/crop 标记保留） */
    vsc_mbus_fmt_t sensor_intent = pipe->stages[0].source_fmt;

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
        /* 仅用户可见字段参与 EXACT/ADJUSTED 判定；
         * bin_x/y、crop_left/top 是内部协商标记，排除在外。 */
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
