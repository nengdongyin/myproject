/**
 * @file vsc_lite.c
 * @brief VSC Lite 实现 — 固定管线前向传播 + 独立时序聚合
 *
 * 协商分为三个阶段:
 *   阶段 A — 前向传播: sensor → stage1 → ... → endpoint
 *           仅协商 width / height / format / frame_rate
 *           sensor 在 source_fmt 中填写时序基准值
 *           中间 stage 透传时序字段，不修改
 *   阶段 B — 时序聚合: 遍历所有 stage，调用 get_timing_req
 *           并行约束取 max，串行延迟取 sum，合并 sensor 基准
 *   阶段 C — 收敛: 将聚合后的时序写入 result.primary_fmt
 *
 * 简化（相对于完整 VSC）:
 *   - 无 BFS / 邻接表（线性遍历）
 *   - 无反向传播 / 多轮收敛
 *   - 无 TAP 支持
 */

#include "vsc_lite.h"
#include <string.h>
#include <stdio.h>

/* ── 调试开关 ── */
#ifndef VSC_LITE_DEBUG_TIMING
#define VSC_LITE_DEBUG_TIMING 0
#endif

/* ================================================================
 *  内部 helper — 将时序字段从 sink 复制到 source
 *
 *  阶段 A 约定: 中间 stage 不修改时序字段。为保证即使 driver
 *  忘记此约定时序也不丢失，每次 try_fmt_source 之后强制执行此复制。
 * ================================================================ */

static void vsc_lite_copy_timing(const vsc_mbus_fmt_t *src,
                                 vsc_mbus_fmt_t *dst)
{
    /* 行/帧周期和消隐从前级透传（timing 维度不变） */
    dst->pixel_clock_hz = src->pixel_clock_hz;
    dst->h_total        = src->h_total;
    dst->h_blank        = src->h_blank;
    dst->v_total        = src->v_total;
    dst->v_blank        = src->v_blank;
    /* active 同步到当前 stage 的输出尺寸（可能已被 try_fmt_source 修改） */
    dst->h_active       = dst->width;
    dst->v_active       = dst->height;
    memcpy(dst->reserved_t, src->reserved_t, sizeof(dst->reserved_t));
}

/* ================================================================
 *  阶段 B — 时序聚合
 * ================================================================ */

/**
 * @brief 遍历所有 stage 收集时序需求，并行 max、串行 sum，合并 sensor 基准
 * @param pipe  已完成阶段 A 的管线
 * @param final [out] 聚合后的最终时序（含所有字段）
 * @return VSC_OK 成功
 */
static int vsc_lite_aggregate_timing(const vsc_lite_pipeline_t *pipe,
                                     vsc_mbus_fmt_t *final,
                                     uint32_t *out_max_h_total,
                                     uint32_t *out_max_v_total)
{
    vsc_timing_req_t agg;
    uint32_t latency_lines = 0;

    memset(&agg, 0, sizeof(agg));

    /* ── 收集各 stage 的时序需求 ── */
    for (uint8_t i = 0; i < pipe->count; i++)
    {
        const vsc_lite_stage_t *st = &pipe->stages[i];
        const vsc_ip_ops_t *ops = st->driver ? &st->driver->ops : NULL;
        if (!ops || !ops->get_timing_req)
            continue;

        vsc_timing_req_t req;
        memset(&req, 0, sizeof(req));

        /* endpoint 无 try_fmt_source，source_fmt 全零。
           用 sink_fmt 作为回退，避免 driver 误读零值字段。 */
        const vsc_mbus_fmt_t *src_ref = &st->source_fmt;
        if (i == pipe->count - 1 && !vsc_fmt_is_valid(src_ref))
            src_ref = &st->sink_fmt;

        int rc = ops->get_timing_req(st->drv_ctx,
                                     &st->sink_fmt, src_ref, &req);
        if (rc != VSC_OK)
            return rc;

        /* 并行约束 — max */
        if (req.min_h_total  > agg.min_h_total)  agg.min_h_total  = req.min_h_total;
        if (req.min_h_blank  > agg.min_h_blank)  agg.min_h_blank  = req.min_h_blank;
        if (req.min_v_total  > agg.min_v_total)  agg.min_v_total  = req.min_v_total;
        if (req.min_v_blank  > agg.min_v_blank)  agg.min_v_blank  = req.min_v_blank;

        /* 串行约束 — sum */
        latency_lines += req.pipeline_lines;

        /* sensor 上限 — 通过 reserved 约定传递 */
        if (i == 0)
        {
            *out_max_h_total = req.reserved[0];
            *out_max_v_total = req.reserved[1];
        }

#if VSC_LITE_DEBUG_TIMING
        printf("[VSC-Lite timing] stage[%u]%-16s ip_clk=%-8u h_t=%-6u h_b=%-6u v_t=%-6u v_b=%-6u pipe=%u\n",
               i, st->driver ? st->driver->name : "(null)",
               req.ip_clock_hz,
               req.min_h_total, req.min_h_blank,
               req.min_v_total, req.min_v_blank,
               req.pipeline_lines);
#endif
    }

    /* ── 合并 sensor 基准 ── */
    const vsc_mbus_fmt_t *base = &pipe->stages[0].source_fmt;

    memset(final, 0, sizeof(*final));

    /* 空间字段 */
    final->width          = base->width;
    final->height         = base->height;
    final->pixel_format   = base->pixel_format;
    final->frame_rate_num = base->frame_rate_num;
    final->frame_rate_den = base->frame_rate_den;
    final->bit_depth      = base->bit_depth;
    final->lanes          = base->lanes;

    /* 时序字段 — sensor 基准 ⊕ IP 需求 */
    final->pixel_clock_hz = base->pixel_clock_hz;
    final->h_active       = base->h_active;
    final->h_total        = base->h_total;
    if (agg.min_h_total > final->h_total)
        final->h_total = agg.min_h_total;

    /* h_blank = max(sensor_h_blank, IP_min_h_blank, h_total - h_active) */
    {
        uint32_t hb = final->h_total - final->h_active;
        if (base->h_blank > hb)      hb = base->h_blank;
        if (agg.min_h_blank > hb)    hb = agg.min_h_blank;
        final->h_blank = hb;
        final->h_total = final->h_active + final->h_blank;  /* 保持不变量 */
    }

    final->v_active       = base->v_active;

    /* v_blank = max(sensor_v_blank, IP_min_v_blank, pipeline_latency) */
    {
        uint32_t vb = base->v_blank;
        if (agg.min_v_blank > vb)    vb = agg.min_v_blank;
        if (latency_lines   > vb)    vb = latency_lines;
        final->v_blank = vb;
    }

    /* v_total = max(sensor_v_total, IP_min_v_total, v_active + v_blank) */
    {
        uint32_t vt = base->v_total;
        if (agg.min_v_total > vt)                  vt = agg.min_v_total;
        if (final->v_active + final->v_blank > vt) vt = final->v_active + final->v_blank;
        final->v_total = vt;
    }

    /* reserved 透传 */
    memcpy(final->reserved_t, base->reserved_t, sizeof(final->reserved_t));

    return VSC_OK;
}

/* ================================================================
 *  公开 API
 * ================================================================ */

int vsc_lite_pipeline_init(vsc_lite_pipeline_t *pipe,
                           const vsc_driver_t **drivers, uint8_t count)
{
    if (!pipe || !drivers || count < 2 || count > VSC_LITE_MAX_STAGES)
        return VSC_ERR_PARAM;

    memset(pipe, 0, sizeof(*pipe));
    pipe->count = count;

    for (uint8_t i = 0; i < count; i++)
    {
        pipe->stages[i].driver = drivers[i];

        if (drivers[i] && drivers[i]->ops.init)
        {
            int rc = drivers[i]->ops.init(&pipe->stages[i].drv_ctx, 0, NULL, 0);
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

    /* ── reset all stage formats ── */
    for (uint8_t i = 0; i < pipe->count; i++)
    {
        memset(&pipe->stages[i].sink_fmt, 0, sizeof(vsc_mbus_fmt_t));
        memset(&pipe->stages[i].source_fmt, 0, sizeof(vsc_mbus_fmt_t));
    }

    /* ================================================================
     *  阶段 A — 前向传播（仅协商空间维度）
     * ================================================================ */

    /* ── Stage 0: sensor ── */
    vsc_lite_stage_t *sensor = &pipe->stages[0];
    const vsc_ip_ops_t *ops = sensor->driver ? &sensor->driver->ops : NULL;

    if (ops && ops->try_fmt_source)
    {
        int rc = ops->try_fmt_source(sensor->drv_ctx, intent, &sensor->source_fmt);
        if (rc != VSC_OK || !vsc_fmt_is_valid(&sensor->source_fmt))
        {
            result->status = VSC_NEGOTIATE_FAILED;
            return rc != VSC_OK ? rc : VSC_ERR_PARAM;
        }
    }
    else
    {
        sensor->source_fmt = *intent;
    }

    const vsc_mbus_fmt_t *upstream = &sensor->source_fmt;

    /* ── Stage 1..N-2: processing stages ── */
    for (uint8_t i = 1; i < pipe->count - 1; i++)
    {
        vsc_lite_stage_t *st = &pipe->stages[i];
        ops = st->driver ? &st->driver->ops : NULL;

        /* try_fmt_sink — 空间格式校验 */
        if (ops && ops->try_fmt_sink)
        {
            int rc = ops->try_fmt_sink(st->drv_ctx, upstream, &st->sink_fmt);
            if (rc != VSC_OK || !vsc_fmt_is_valid(&st->sink_fmt))
            {
                result->status = VSC_NEGOTIATE_FAILED;
                return rc != VSC_OK ? rc : VSC_ERR_PARAM;
            }
        }
        else
        {
            st->sink_fmt = *upstream;
        }

        /* try_fmt_source — 空间格式变换 */
        if (ops && ops->try_fmt_source)
        {
            int rc = ops->try_fmt_source(st->drv_ctx, &st->sink_fmt, &st->source_fmt);
            if (rc != VSC_OK || !vsc_fmt_is_valid(&st->source_fmt))
            {
                result->status = VSC_NEGOTIATE_FAILED;
                return rc != VSC_OK ? rc : VSC_ERR_PARAM;
            }
        }
        else
        {
            st->source_fmt = st->sink_fmt;
        }

        /* 阶段 A 约定: 时序字段从 sink 透传到 source（即使 driver 忘记） */
        vsc_lite_copy_timing(&st->sink_fmt, &st->source_fmt);

        upstream = &st->source_fmt;
    }

    /* ── Last stage: endpoint (sink only) ── */
    if (pipe->count >= 2)
    {
        vsc_lite_stage_t *ep = &pipe->stages[pipe->count - 1];
        ops = ep->driver ? &ep->driver->ops : NULL;

        if (ops && ops->try_fmt_sink)
        {
            int rc = ops->try_fmt_sink(ep->drv_ctx, upstream, &ep->sink_fmt);
            if (rc != VSC_OK || !vsc_fmt_is_valid(&ep->sink_fmt))
            {
                result->status = VSC_NEGOTIATE_FAILED;
                return rc != VSC_OK ? rc : VSC_ERR_PARAM;
            }
            result->primary_fmt = ep->sink_fmt;
        }
        else
        {
            result->primary_fmt = *upstream;
        }
    }

    /* ================================================================
     *  阶段 B — 时序聚合
     * ================================================================ */

    vsc_mbus_fmt_t final_timing;
    uint32_t sensor_max_h_total = 0;
    uint32_t sensor_max_v_total = 0;

    int rc = vsc_lite_aggregate_timing(pipe, &final_timing,
                                       &sensor_max_h_total, &sensor_max_v_total);
    if (rc != VSC_OK)
    {
        result->status = VSC_NEGOTIATE_FAILED;
        return rc;
    }

    /* 检查 sensor 物理上限 */
    if ((sensor_max_h_total > 0 && final_timing.h_total > sensor_max_h_total) ||
        (sensor_max_v_total > 0 && final_timing.v_total > sensor_max_v_total))
    {
        result->status = VSC_NEGOTIATE_FAILED;
        result->reachable_max = final_timing;
        return VSC_ERR_UNREACHABLE;
    }

    /* ================================================================
     *  阶段 C — 收敛：用聚合后的时序覆盖 result.primary_fmt
     * ================================================================ */

    result->primary_fmt.pixel_clock_hz = final_timing.pixel_clock_hz;
    result->primary_fmt.h_total        = final_timing.h_total;
    result->primary_fmt.h_active       = result->primary_fmt.width;
    result->primary_fmt.h_blank        = final_timing.h_blank;
    result->primary_fmt.v_total        = final_timing.v_total;
    result->primary_fmt.v_active       = result->primary_fmt.height;
    result->primary_fmt.v_blank        = final_timing.v_blank;
    memcpy(result->primary_fmt.reserved_t, final_timing.reserved_t,
           sizeof(final_timing.reserved_t));

    /* 时序收紧后重算物理帧率 */
    if (final_timing.pixel_clock_hz > 0 && final_timing.h_total > 0
        && final_timing.v_total > 0) {
        result->primary_fmt.frame_rate_num = final_timing.pixel_clock_hz;
        result->primary_fmt.frame_rate_den = final_timing.h_total
                                             * final_timing.v_total;
    }

    /* 判定协商状态: 空间字段精确匹配 intent 则为 EXACT，否则 ADJUSTED */
    {
        bool exact = (result->primary_fmt.width         == intent->width)
                  && (result->primary_fmt.height        == intent->height)
                  && (result->primary_fmt.pixel_format  == intent->pixel_format);
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
        if (rc == VSC_OK && *(bool *)out)
        {
            return VSC_OK;
        }
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
        uint8_t dummy[80];
        uint8_t len = sizeof(dummy);
        if (ops->query_cap &&
            ops->query_cap(pipe->stages[i].drv_ctx, cap_id, dummy, &len) != VSC_OK)
            continue;

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

        uint8_t dummy[80];
        uint8_t len = sizeof(dummy);
        if (ops->query_cap &&
            ops->query_cap(pipe->stages[i].drv_ctx, cap_id, dummy, &len) != VSC_OK)
            continue;

        int rc = ops->get_ctrl(pipe->stages[i].drv_ctx, ctrl_id, value);
        if (rc == VSC_OK) return VSC_OK;
    }
    return VSC_ERR_NOT_SUPPORTED;
}
