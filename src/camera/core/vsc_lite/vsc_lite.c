/**
 * @file vsc_lite.c
 * @brief VSC Lite 实现 — 固定管线前向传播
 *
 * 与完整 VSC resolver 的 forward_propagate 逻辑等价:
 *   1. sensor:   intent → try_fmt_source → source_fmt
 *   2. stage N:  upstream.source_fmt → try_fmt_sink → try_fmt_source
 *   3. endpoint: upstream.source_fmt → result.primary_fmt
 *
 * 简化:
 *   - 无 BFS / 邻接表（线性遍历）
 *   - 无反向传播 / 多轮收敛
 *   - 无 TAP 支持
 */

#include "vsc_lite.h"
#include <string.h>

#define VSC_ERR_PARAM (-1)

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

        /* try_fmt_sink */
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

        /* try_fmt_source */
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

        upstream = &st->source_fmt;
    }

    /* ── Last stage: endpoint (sink only, no source) ── */
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

    result->status = VSC_NEGOTIATE_EXACT;
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
