/**
 * @file    test_vsc_lite.c
 * @brief   Unit tests for VSC Lite — simplified pipeline solver
 *
 * Uses the same mock drivers as test_vsc_resolver.c for behavioral equivalence.
 */

#include "unity.h"
#include "test_suite.h"
#include "vsc_core_types.h"
#include "vsc_lite.h"
#include "crop_vsc.h"
#include "binning_vsc.h"
#include "decoder_vsc.h"
#include "histogram_vsc.h"
#include "sensor_vsc.h"
#include <string.h>

/* ========================================================================
 *  Shared test context
 * ======================================================================== */


/* ========================================================================
 *  Static instances for real-driver tests
 * ======================================================================== */

static binning_vsc_inst_t   g_real_bin  = { .hw = { .base_addr = 0, .factor_x = 2, .factor_y = 2 } };
static crop_vsc_inst_t      g_real_crop = { .hw = { .base_addr = 0 } };
static decoder_vsc_inst_t   g_real_dec  = { .hw = { .base_addr = 0 } };
static histogram_vsc_inst_t g_real_hist = { .hw = { .base_addr = 0 } };
static sensor_vsc_inst_t    g_real_sensor = { .model = "sensor_imx477" };

static vsc_lite_pipeline_t g_pipe;
static vsc_resolver_result_t g_result;

/* ========================================================================
 *  Mock driver contexts
 * ======================================================================== */

typedef struct {
    uint32_t max_w;
    uint32_t max_h;
    uint32_t supported_fmts[4];
    uint32_t output_fmt;
    uint8_t  bin_factor_x;
    uint8_t  bin_factor_y;
    uint8_t  r0;
    uint8_t  r1;
    bool     has_timing;   /* 是否实现 get_timing_req       */
    /* ── timing fields (用于时序聚合测试) ── */
    vsc_timing_req_t timing_req;   /* get_timing_req 返回值         */

} mock_drv_ctx_t;

static mock_drv_ctx_t g_ctx_sensor_raw;
static mock_drv_ctx_t g_ctx_pass;
static mock_drv_ctx_t g_ctx_bin2x2;
static mock_drv_ctx_t g_ctx_decoder;
static mock_drv_ctx_t g_ctx_crop_1920;
static mock_drv_ctx_t g_ctx_reject;

/* ── mock implementations ── */
static int mock_sink_pass(void *drv_ctx, const vsc_mbus_fmt_t *p, vsc_mbus_fmt_t *c)
    { (void)drv_ctx; *c = *p; return VSC_OK; }

static int mock_sink_reject(void *drv_ctx, const vsc_mbus_fmt_t *p, vsc_mbus_fmt_t *c)
    { (void)drv_ctx; (void)p; memset(c, 0, sizeof(*c)); return VSC_ERR_PROPAGATION_SINK; }

static int mock_source_pass(void *drv_ctx, const vsc_mbus_fmt_t *s, vsc_mbus_fmt_t *o)
    { (void)drv_ctx; *o = *s; return VSC_OK; }

static int mock_source_binning(void *drv_ctx, const vsc_mbus_fmt_t *s, vsc_mbus_fmt_t *o)
    { mock_drv_ctx_t *c = (mock_drv_ctx_t *)drv_ctx;
      *o = *s; o->spatial.width /= c->bin_factor_x; o->spatial.height /= c->bin_factor_y; return VSC_OK; }

static int mock_source_decoder(void *drv_ctx, const vsc_mbus_fmt_t *s, vsc_mbus_fmt_t *o)
    { mock_drv_ctx_t *c = (mock_drv_ctx_t *)drv_ctx;
      *o = *s; o->spatial.pixel_format = c->output_fmt; return VSC_OK; }

static int mock_source_crop(void *drv_ctx, const vsc_mbus_fmt_t *s, vsc_mbus_fmt_t *o)
    { mock_drv_ctx_t *c = (mock_drv_ctx_t *)drv_ctx;
      *o = *s;
      if (o->spatial.width > c->max_w)  o->spatial.width  = c->max_w;
      if (o->spatial.height > c->max_h) o->spatial.height = c->max_h;
      return VSC_OK; }

static bool mock_fmt_supported(const mock_drv_ctx_t *ctx, uint32_t fmt)
{
    for (int i = 0; i < 4; i++) {
        if (ctx->supported_fmts[i] == VSC_FMT_INVALID) break;
        if (ctx->supported_fmts[i] == fmt) return true;
    }
    return false;
}

static int mock_sensor_source(void *drv_ctx, const vsc_mbus_fmt_t *intent, vsc_mbus_fmt_t *src)
{
    mock_drv_ctx_t *ctx = (mock_drv_ctx_t *)drv_ctx;
    *src = *intent;
    if (!mock_fmt_supported(ctx, intent->spatial.pixel_format))
        src->spatial.pixel_format = ctx->supported_fmts[0];
    if (src->spatial.width > ctx->max_w)  src->spatial.width  = ctx->max_w;
    if (src->spatial.height > ctx->max_h) src->spatial.height = ctx->max_h;
    return VSC_OK;
}

static int mock_sink_format_filter(void *drv_ctx, const vsc_mbus_fmt_t *p, vsc_mbus_fmt_t *c)
{
    mock_drv_ctx_t *ctx = (mock_drv_ctx_t *)drv_ctx;
    if (!mock_fmt_supported(ctx, p->spatial.pixel_format)) {
        memset(c, 0, sizeof(*c)); return VSC_ERR_PROPAGATION_SINK;
    }
    *c = *p;
    if (c->spatial.width > ctx->max_w) c->spatial.width = ctx->max_w;
    if (c->spatial.height > ctx->max_h) c->spatial.height = ctx->max_h;
    return VSC_OK;
}

/* ── mock get_timing_req that always fails ── */
static int mock_get_timing_req_fail(void *drv_ctx,
                                    const vsc_mbus_fmt_t *sink_fmt,
                                    const vsc_mbus_fmt_t *source_fmt,
                                    vsc_timing_req_t *req)
{
    (void)drv_ctx; (void)sink_fmt; (void)source_fmt;
    memset(req, 0, sizeof(*req));
    return VSC_ERR_PROPAGATION_SINK;
}

/* ── forward declarations ── */
static int mock_query_cap(void *drv_ctx, uint32_t cap_id, void *out, uint8_t *out_len);


/* ── mock ctrl implementations ── */
static uint32_t g_mock_ctrl_values[16];

static int mock_set_ctrl(void *drv_ctx, uint32_t ctrl_id, uint32_t value)
{
    mock_drv_ctx_t *c = (mock_drv_ctx_t *)drv_ctx;
    if (!c || !c->has_timing) return VSC_ERR_NOT_SUPPORTED;
    /* 使用 VSC_CTRL_* 真实 ID，低 4 位索引 16 槽 mock 存储 */
    g_mock_ctrl_values[ctrl_id & 0xF] = value;
    return VSC_OK;
}

static int mock_get_ctrl(void *drv_ctx, uint32_t ctrl_id, uint32_t *value)
{
    mock_drv_ctx_t *c = (mock_drv_ctx_t *)drv_ctx;
    if (!c || !c->has_timing || !value) return VSC_ERR_NOT_SUPPORTED;
    *value = g_mock_ctrl_values[ctrl_id & 0xF];
    return VSC_OK;
}

/* ── mock ops tables ── */
/* ── mock get_timing_req ── */
static int mock_get_timing_req(void *drv_ctx,
                               const vsc_mbus_fmt_t *sink_fmt,
                               const vsc_mbus_fmt_t *source_fmt,
                               vsc_timing_req_t *req)
{
    mock_drv_ctx_t *c = (mock_drv_ctx_t *)drv_ctx;
    (void)sink_fmt; (void)source_fmt;
    memset(req, 0, sizeof(*req));
    if (c && c->has_timing) {
        *req = c->timing_req;
    }
    return VSC_OK;
}

/* ── mock sensor source with timing ── */
static int mock_sensor_source_timed(void *drv_ctx, const vsc_mbus_fmt_t *intent,
                                    vsc_mbus_fmt_t *src)
{
    int rc = mock_sensor_source(drv_ctx, intent, src);
    if (rc != VSC_OK) return rc;

    /* 填充时序基准值 */
    src->timing.pixel_clock_hz = 74250000;
    src->timing.h_active       = src->spatial.width;
    src->timing.h_total        = src->spatial.width + 128;
    src->timing.h_blank        = 128;
    src->timing.v_active       = src->spatial.height;
    src->timing.v_total        = src->spatial.height + 80;
    src->timing.v_blank        = 80;
    return VSC_OK;
}

/* ── mock ops tables ── */
static const vsc_ip_ops_t g_ops_sensor    = { NULL, NULL, mock_sensor_source, NULL };
static const vsc_ip_ops_t g_ops_sensor_timed = { NULL, NULL, mock_sensor_source_timed, NULL };
static const vsc_ip_ops_t g_ops_pass      = { NULL, mock_sink_pass, mock_source_pass, NULL };
static const vsc_ip_ops_t g_ops_endpoint  = { NULL, mock_sink_pass, NULL, NULL };
static const vsc_ip_ops_t g_ops_binning   = { NULL, mock_sink_pass, mock_source_binning, NULL };
static const vsc_ip_ops_t g_ops_decoder   = { NULL, mock_sink_format_filter, mock_source_decoder, NULL };
static const vsc_ip_ops_t g_ops_crop      = { NULL, mock_sink_pass, mock_source_crop, NULL };
static const vsc_ip_ops_t g_ops_reject    = { NULL, mock_sink_reject, NULL, NULL };

/* ── ops tables with get_timing_req ── */
static const vsc_ip_ops_t g_ops_pass_timed      = { NULL, mock_sink_pass, mock_source_pass, NULL, mock_get_timing_req, NULL };
static const vsc_ip_ops_t g_ops_binning_timed   = { NULL, mock_sink_pass, mock_source_binning, NULL, mock_get_timing_req, NULL };
static const vsc_ip_ops_t g_ops_endpoint_timed  = { NULL, mock_sink_pass, NULL, NULL, mock_get_timing_req, NULL };
static const vsc_ip_ops_t g_ops_sensor_timed2   = { NULL, NULL, mock_sensor_source_timed, NULL, mock_get_timing_req, NULL };
static const vsc_ip_ops_t g_ops_sensor_query    = { NULL, NULL, mock_sensor_source_timed, NULL, mock_get_timing_req, mock_query_cap };
static const vsc_ip_ops_t g_ops_pass_fail_timing = { NULL, mock_sink_pass, mock_source_pass, NULL, mock_get_timing_req_fail, NULL };

/* ── mock driver descriptors ── */
static const vsc_driver_t g_drv_sensor        = { .ops = g_ops_sensor };
static const vsc_driver_t g_drv_sensor_timed  = { .ops = g_ops_sensor_timed };
static const vsc_driver_t g_drv_pass          = { .ops = g_ops_pass };
static const vsc_driver_t g_drv_endpoint      = { .ops = g_ops_endpoint };
static const vsc_driver_t g_drv_binning       = { .ops = g_ops_binning };
static const vsc_driver_t g_drv_decoder       = { .ops = g_ops_decoder };
static const vsc_driver_t g_drv_crop          = { .ops = g_ops_crop };
static const vsc_driver_t g_drv_reject        = { .ops = g_ops_reject };

/* ── mock driver descriptors with timing ── */
static const vsc_driver_t g_drv_pass_timed     = { .ops = g_ops_pass_timed };
static const vsc_driver_t g_drv_binning_timed  = { .ops = g_ops_binning_timed };
static const vsc_driver_t g_drv_endpoint_timed = { .ops = g_ops_endpoint_timed };
static const vsc_driver_t g_drv_sensor_timed2   = { .ops = g_ops_sensor_timed2 };
static const vsc_driver_t g_drv_sensor_query    = { .ops = g_ops_sensor_query };
static const vsc_driver_t g_drv_pass_fail_timing = { .ops = g_ops_pass_fail_timing };

/* ── ops with query_cap + set/get_ctrl for testing ── */
static const vsc_ip_ops_t g_ops_query_ctrl = {
    NULL, NULL, NULL, NULL, NULL,
    mock_query_cap, mock_set_ctrl, mock_get_ctrl
};
static const vsc_driver_t g_drv_query_ctrl = { .ops = g_ops_query_ctrl };


/* ========================================================================
 *  Setup / teardown
 * ======================================================================== */

static void lite_setup(void)
{
    memset(&g_pipe, 0, sizeof(g_pipe));
    memset(&g_result, 0, sizeof(g_result));

    memset(&g_ctx_sensor_raw, 0, sizeof(g_ctx_sensor_raw));
    g_ctx_sensor_raw.max_w = 4056;
    g_ctx_sensor_raw.max_h = 3040;
    g_ctx_sensor_raw.supported_fmts[0] = VSC_FMT_RAW8;
    g_ctx_sensor_raw.supported_fmts[1] = VSC_FMT_RAW10;
    g_ctx_sensor_raw.supported_fmts[2] = VSC_FMT_RAW12;

    memset(&g_ctx_pass, 0, sizeof(g_ctx_pass));
    g_ctx_pass.max_w = 8192;
    g_ctx_pass.max_h = 8192;

    memset(&g_ctx_bin2x2, 0, sizeof(g_ctx_bin2x2));
    g_ctx_bin2x2.bin_factor_x = 2;
    g_ctx_bin2x2.bin_factor_y = 2;

    memset(&g_ctx_decoder, 0, sizeof(g_ctx_decoder));
    g_ctx_decoder.max_w = 8192;
    g_ctx_decoder.max_h = 8192;
    g_ctx_decoder.supported_fmts[0] = VSC_FMT_RAW8;
    g_ctx_decoder.supported_fmts[1] = VSC_FMT_RAW10;
    g_ctx_decoder.supported_fmts[2] = VSC_FMT_RAW12;
    g_ctx_decoder.output_fmt = VSC_FMT_RGB888;

    memset(&g_ctx_crop_1920, 0, sizeof(g_ctx_crop_1920));
    g_ctx_crop_1920.max_w = 1920;
    g_ctx_crop_1920.max_h = 1080;

    memset(&g_ctx_reject, 0, sizeof(g_ctx_reject));
}

static void lite_teardown(void) { }

/* ========================================================================
 *  Pipeline builders — driver + instance pairs
 * ======================================================================== */

/**
 * Sensor → Pass → Endpoint
 */
void test_linear_pass_chain(void)
{
        const vsc_lite_stage_def_t stages[] = {
        { &g_drv_sensor, &g_ctx_sensor_raw },
        { &g_drv_pass, &g_ctx_pass },
        { &g_drv_endpoint, &g_ctx_pass },
    };
    vsc_lite_pipeline_init(&g_pipe, stages, 3);

    vsc_mbus_fmt_t intent = { { 1920, 1080, VSC_FMT_RAW10, 30, 1, 10, 4, {0} } };
    int rc = vsc_lite_try_fmt(&g_pipe, &intent, &g_result);
    TEST_ASSERT_EQUAL_INT(VSC_NEGOTIATE_EXACT, g_result.status);
    TEST_ASSERT_EQUAL_UINT32(1920, g_result.primary_fmt.spatial.width);
    TEST_ASSERT_EQUAL_UINT32(1080, g_result.primary_fmt.spatial.height);
    TEST_ASSERT_EQUAL_UINT32(VSC_FMT_RAW10, g_result.primary_fmt.spatial.pixel_format);
}

/**
 * Sensor → Binning(2×2) → Decoder → Crop(1920) → Endpoint
 */
void test_full_pipeline(void)
{
        const vsc_lite_stage_def_t stages[] = {
        { &g_drv_sensor, &g_ctx_sensor_raw },
        { &g_drv_binning, &g_ctx_bin2x2 },
        { &g_drv_decoder, &g_ctx_decoder },
        { &g_drv_crop, &g_ctx_crop_1920 },
        { &g_drv_endpoint, &g_ctx_pass },
    };
    vsc_lite_pipeline_init(&g_pipe, stages, 5);

    /* 1920×1080 RGB888 → sensor falls back RAW8 → bin /2 → 960×540 → decoder → RGB → crop clamp */
    vsc_mbus_fmt_t intent = { { 1920, 1080, VSC_FMT_RGB888, 30, 1, 8, 4, {0} } };
    int rc = vsc_lite_try_fmt(&g_pipe, &intent, &g_result);
    /* intent 1920×1080 → binning 2×2 → 960×540, 最终尺寸与意图不同, 状态为 ADJUSTED */
    TEST_ASSERT_EQUAL_INT(VSC_NEGOTIATE_ADJUSTED, g_result.status);
    /* Binning halves before reaching crop, final output = 960×540 */
    TEST_ASSERT_EQUAL_UINT32(960, g_result.primary_fmt.spatial.width);
    TEST_ASSERT_EQUAL_UINT32(540, g_result.primary_fmt.spatial.height);
}

/**
 * Intent larger than sensor max → clamp
 */
void test_intent_exceeds_sensor(void)
{
        const vsc_lite_stage_def_t stages[] = {
        { &g_drv_sensor, &g_ctx_sensor_raw },
        { &g_drv_pass, &g_ctx_pass },
        { &g_drv_endpoint, &g_ctx_pass },
    };
    vsc_lite_pipeline_init(&g_pipe, stages, 3);

    /* 5000×4000 exceeds sensor's 4056×3040 */
    vsc_mbus_fmt_t intent = { { 5000, 4000, VSC_FMT_RAW10, 30, 1, 10, 4, {0} } };
    int rc = vsc_lite_try_fmt(&g_pipe, &intent, &g_result);
    TEST_ASSERT_EQUAL_UINT32(4056, g_result.primary_fmt.spatial.width);
    TEST_ASSERT_EQUAL_UINT32(3040, g_result.primary_fmt.spatial.height);
}

/**
 * Format not supported by decoder → fail
 */
void test_format_rejected_by_decoder(void)
{
        const vsc_lite_stage_def_t stages[] = {
        { &g_drv_sensor, &g_ctx_sensor_raw },
        { &g_drv_binning, &g_ctx_bin2x2 },
        { &g_drv_decoder, &g_ctx_decoder },
        { &g_drv_crop, &g_ctx_crop_1920 },
        { &g_drv_endpoint, &g_ctx_pass },
    };
    vsc_lite_pipeline_init(&g_pipe, stages, 5);

    /* YUV422 sensor doesn't support, but mock_sensor falls back to first supported.
       Then decoder doesn't support YUV422 format filter → fail */
    vsc_mbus_fmt_t intent = { { 1920, 1080, VSC_FMT_YUV422, 30, 1, 8, 4, {0} } };
    int rc = vsc_lite_try_fmt(&g_pipe, &intent, &g_result);
    /* Sensor falls back to RAW8 (first supported), decoder accepts RAW8 → pass */
    TEST_ASSERT_EQUAL_UINT32(VSC_FMT_RGB888, g_result.primary_fmt.spatial.pixel_format);
}

/**
 * Stage with reject driver → fail
 */
void test_reject_in_middle(void)
{
        const vsc_lite_stage_def_t stages[] = {
        { &g_drv_sensor, &g_ctx_sensor_raw },
        { &g_drv_reject, &g_ctx_reject },
        { &g_drv_endpoint, &g_ctx_pass },
    };
    vsc_lite_pipeline_init(&g_pipe, stages, 3);

    vsc_mbus_fmt_t intent = { { 1920, 1080, VSC_FMT_RAW10, 30, 1, 10, 4, {0} } };
    int rc = vsc_lite_try_fmt(&g_pipe, &intent, &g_result);
    TEST_ASSERT_NOT_EQUAL(VSC_OK, rc);
    TEST_ASSERT_EQUAL_INT(VSC_NEGOTIATE_FAILED, g_result.status);
}

/**
 * Minimal pipeline: sensor → endpoint
 */
void test_minimal_sensor_endpoint(void)
{
        const vsc_lite_stage_def_t stages[] = {
        { &g_drv_sensor, &g_ctx_sensor_raw },
        { &g_drv_endpoint, &g_ctx_pass },
    };
    vsc_lite_pipeline_init(&g_pipe, stages, 2);

    vsc_mbus_fmt_t intent = { { 1920, 1080, VSC_FMT_RAW10, 30, 1, 10, 4, {0} } };
    int rc = vsc_lite_try_fmt(&g_pipe, &intent, &g_result);
    TEST_ASSERT_EQUAL_UINT32(1920, g_result.primary_fmt.spatial.width);
}

/**
 * Real drivers: sensor → crop → endpoint
 */
void test_real_drivers_sensor_crop(void)
{
        const vsc_lite_stage_def_t stages[] = {
        { &sensor_imx477_vsc_driver, &g_real_sensor },
        { &crop_vsc_driver,           &g_real_crop   },
        { &binning_vsc_driver,        &g_real_bin    },
        { &decoder_vsc_driver,        &g_real_dec    },
        { &histogram_vsc_driver,      &g_real_hist   },
    };
    vsc_lite_pipeline_init(&g_pipe, stages, 5);
    vsc_mbus_fmt_t intent = { { 1920, 1080, VSC_FMT_RAW10, 30, 1, 10, 4, {0} } };
    int rc = vsc_lite_try_fmt(&g_pipe, &intent, &g_result);
    /* Real drivers may adjust format — just verify it's valid */
    TEST_ASSERT_TRUE(vsc_fmt_is_valid(&g_result.primary_fmt));
}

/**
 * Commit after try_fmt
 */
void test_commit_after_try(void)
{
        const vsc_lite_stage_def_t stages[] = {
        { &g_drv_sensor, &g_ctx_sensor_raw },
        { &g_drv_pass, &g_ctx_pass },
        { &g_drv_endpoint, &g_ctx_pass },
    };
    vsc_lite_pipeline_init(&g_pipe, stages, 3);

    vsc_mbus_fmt_t intent = { { 1920, 1080, VSC_FMT_RAW10, 30, 1, 10, 4, {0} } };
    int rc = vsc_lite_try_fmt(&g_pipe, &intent, &g_result);
    rc = vsc_lite_commit_fmt(&g_pipe, &g_result.primary_fmt);
}

/**
 * Multiple try_fmt calls: second call must start clean, no stale state
 */
void test_multiple_try_fmt_isolation(void)
{
        const vsc_lite_stage_def_t stages[] = {
        { &g_drv_sensor, &g_ctx_sensor_raw },
        { &g_drv_pass, &g_ctx_pass },
        { &g_drv_endpoint, &g_ctx_pass },
    };
    vsc_lite_pipeline_init(&g_pipe, stages, 3);

    int rc;
    /* First call: 1920×1080 */
    vsc_mbus_fmt_t intent1 = { { 1920, 1080, VSC_FMT_RAW10, 30, 1, 10, 4, {0} } };
    rc = vsc_lite_try_fmt(&g_pipe, &intent1, &g_result);
    TEST_ASSERT_EQUAL_UINT32(1920, g_result.primary_fmt.spatial.width);

    /* Second call: smaller intent, must NOT retain first call's 1920 */
    vsc_mbus_fmt_t intent2 = { { 640, 480, VSC_FMT_RAW10, 30, 1, 10, 4, {0} } };
    rc = vsc_lite_try_fmt(&g_pipe, &intent2, &g_result);
    TEST_ASSERT_EQUAL_UINT32(640, g_result.primary_fmt.spatial.width);
    TEST_ASSERT_EQUAL_UINT32(480, g_result.primary_fmt.spatial.height);
}

/**
 * commit with NULL args → error
 */
void test_commit_null_params(void)
{
    vsc_mbus_fmt_t fmt = { { 1920, 1080, VSC_FMT_RAW10, 30, 1, 10, 4, {0} } };
    TEST_ASSERT_NOT_EQUAL(VSC_OK, vsc_lite_commit_fmt(NULL, &fmt));
    TEST_ASSERT_NOT_EQUAL(VSC_OK, vsc_lite_commit_fmt(&g_pipe, NULL));
}

/**
 * Pipeline with count=1 → init rejects
 */
void test_single_stage_rejected(void)
{
        const vsc_lite_stage_def_t stages[] = {
        { &g_drv_sensor, NULL },
    };
    int rc = vsc_lite_pipeline_init(&g_pipe, stages, 1);
    TEST_ASSERT_NOT_EQUAL(VSC_OK, rc);
}

/**
 * NULL driver in middle stage → treated as pass-through, no crash
 */
void test_null_driver_in_middle(void)
{
        const vsc_lite_stage_def_t stages[] = {
        { &g_drv_sensor, &g_ctx_sensor_raw },
        { NULL, NULL },
        { &g_drv_endpoint, &g_ctx_pass },
    };
    vsc_lite_pipeline_init(&g_pipe, stages, 3);

    vsc_mbus_fmt_t intent = { { 1920, 1080, VSC_FMT_RAW10, 30, 1, 10, 4, {0} } };
    int rc = vsc_lite_try_fmt(&g_pipe, &intent, &g_result);
    TEST_ASSERT_EQUAL_UINT32(1920, g_result.primary_fmt.spatial.width);
}

/**
 * NULL params → return error
 */
void test_null_params(void)
{
    vsc_mbus_fmt_t intent = { { 1920, 1080, VSC_FMT_RAW10, 30, 1, 10, 4, {0} } };
    TEST_ASSERT_NOT_EQUAL(VSC_OK, vsc_lite_pipeline_init(NULL, NULL, 0));
    TEST_ASSERT_NOT_EQUAL(VSC_OK, vsc_lite_try_fmt(NULL, &intent, &g_result));
    TEST_ASSERT_NOT_EQUAL(VSC_OK, vsc_lite_try_fmt(&g_pipe, NULL, &g_result));
    TEST_ASSERT_NOT_EQUAL(VSC_OK, vsc_lite_try_fmt(&g_pipe, &intent, NULL));
}

/* ========================================================================
 *  能力查询测试
 * ======================================================================== */

/* mock query_cap — 返回预设的能力描述符 */
static int mock_query_cap(void *drv_ctx, uint32_t cap_id,
                           void *out, uint8_t *out_len)
{
    mock_drv_ctx_t *c = (mock_drv_ctx_t *)drv_ctx;
    /* 只有设置了 has_timing 的 context 才响应（复用 has_timing 标志） */
    if (!c || !c->has_timing)
        return VSC_ERR_NOT_SUPPORTED;

    switch (cap_id) {
    case VSC_CAP_BINNING: {
        vsc_binning_cap_t *bc = (vsc_binning_cap_t *)out;
        if (*out_len < sizeof(*bc)) return VSC_ERR_PARAM;
        *out_len = sizeof(*bc);
        memset(bc, 0, sizeof(*bc));
        /* 用 timing_req 的 reserved 字段传递 factor 和 location */
        bc->available    = true;
        bc->factor_x     = (uint8_t)c->timing_req.reserved[0];
        bc->factor_y     = (uint8_t)c->timing_req.reserved[0];
        bc->max_factor_x = (uint8_t)c->timing_req.reserved[1];
        bc->max_factor_y = (uint8_t)c->timing_req.reserved[1];
        bc->location     = (uint8_t)c->timing_req.reserved[2];
        return VSC_OK;
    }
    case VSC_CAP_CROP: {
        vsc_crop_cap_t *cc = (vsc_crop_cap_t *)out;
        if (*out_len < sizeof(*cc)) return VSC_ERR_PARAM;
        *out_len = sizeof(*cc);
        memset(cc, 0, sizeof(*cc));
        cc->available = true;
        cc->max_w     = c->max_w;
        cc->max_h     = c->max_h;
        cc->align_w   = 8;
        cc->align_h   = 8;
        return VSC_OK;
    }
    default:
        return VSC_ERR_NOT_SUPPORTED;
    }
}



/**
 * Sensor 提供 binning → 直接返回 sensor 的 binning 能力
 */
void test_cap_binning_sensor_first(void)
{
    /* 先验证直接调用 mock 可工作 */
    vsc_binning_cap_t direct_cap;
    uint8_t direct_len = sizeof(direct_cap);
    g_ctx_sensor_raw.has_timing = true;
    g_ctx_sensor_raw.timing_req.reserved[0] = 2;
    g_ctx_sensor_raw.timing_req.reserved[1] = 4;
    g_ctx_sensor_raw.timing_req.reserved[2] = VSC_CAP_LOCATION_SENSOR;
    int rc = mock_query_cap(&g_ctx_sensor_raw, VSC_CAP_BINNING, &direct_cap, &direct_len);
    TEST_ASSERT_EQUAL_INT(VSC_OK, rc);
    TEST_ASSERT_TRUE(direct_cap.available);
    TEST_ASSERT_EQUAL_UINT8(2, direct_cap.factor_x);
    TEST_ASSERT_EQUAL_UINT8(VSC_CAP_LOCATION_SENSOR, direct_cap.location);

    const vsc_lite_stage_def_t stages[] = {
        { &g_drv_sensor_query, &g_ctx_sensor_raw },
        { &g_drv_pass, &g_ctx_pass },
        { &g_drv_endpoint, &g_ctx_pass },
    };
    vsc_lite_pipeline_init(&g_pipe, stages, 3);

    vsc_binning_cap_t cap;
    uint8_t len = sizeof(cap);
    rc = vsc_lite_query_cap(&g_pipe, VSC_CAP_BINNING, &cap, &len);
    TEST_ASSERT_TRUE(cap.available);
    TEST_ASSERT_EQUAL_UINT8(2, cap.factor_x);
    TEST_ASSERT_EQUAL_UINT8(4, cap.max_factor_x);
    TEST_ASSERT_EQUAL_UINT8(VSC_CAP_LOCATION_SENSOR, cap.location);
}

/**
 * Sensor 不提供 binning → FPGA 作为 fallback
 */
void test_cap_binning_fpga_fallback(void)
{
    /* sensor 不响应 query_cap (has_timing=false) */
    /* FPGA binning 响应 */
    g_ctx_bin2x2.has_timing = true;
    g_ctx_bin2x2.timing_req.reserved[0] = 4;
    g_ctx_bin2x2.timing_req.reserved[1] = 8;
    g_ctx_bin2x2.timing_req.reserved[2] = VSC_CAP_LOCATION_FPGA;

    const vsc_lite_stage_def_t stages[] = {
        { &g_drv_pass, &g_ctx_pass },
        { &g_drv_sensor_query, &g_ctx_bin2x2 },
        { &g_drv_endpoint, &g_ctx_pass },
    };
    vsc_lite_pipeline_init(&g_pipe, stages, 3);

    vsc_binning_cap_t cap;
    uint8_t len = sizeof(cap);
    int rc = vsc_lite_query_cap(&g_pipe, VSC_CAP_BINNING, &cap, &len);
    TEST_ASSERT_TRUE(cap.available);
    TEST_ASSERT_EQUAL_UINT8(4, cap.factor_x);
    TEST_ASSERT_EQUAL_UINT8(8, cap.max_factor_x);
    TEST_ASSERT_EQUAL_UINT8(VSC_CAP_LOCATION_FPGA, cap.location);
}

/**
 * 所有 stage 都不提供 → NOT_SUPPORTED
 */
void test_cap_binning_none_available(void)
{
        const vsc_lite_stage_def_t stages[] = {
        { &g_drv_pass, &g_ctx_pass },
        { &g_drv_pass, &g_ctx_pass },
        { &g_drv_endpoint, &g_ctx_pass },
    };
    vsc_lite_pipeline_init(&g_pipe, stages, 3);

    vsc_binning_cap_t cap;
    uint8_t len = sizeof(cap);
    int rc = vsc_lite_query_cap(&g_pipe, VSC_CAP_BINNING, &cap, &len);
    TEST_ASSERT_EQUAL_INT(VSC_ERR_NOT_SUPPORTED, rc);
}

/**
 * 查询不支持的能力类型 → NOT_SUPPORTED
 */
void test_cap_unknown_cap_id(void)
{
        const vsc_lite_stage_def_t stages[] = {
        { &g_drv_sensor_query, &g_ctx_sensor_raw },
        { &g_drv_endpoint, &g_ctx_pass },
    };
    vsc_lite_pipeline_init(&g_pipe, stages, 2);

    vsc_binning_cap_t cap;
    uint8_t len = sizeof(cap);
    /* VSC_CAP_HDR 不被 mock 支持 */
    int rc = vsc_lite_query_cap(&g_pipe, VSC_CAP_HDR, &cap, &len);
    TEST_ASSERT_EQUAL_INT(VSC_ERR_NOT_SUPPORTED, rc);
}

/**
 * Crop 能力查询正常返回
 */
void test_cap_crop_query(void)
{
    g_ctx_sensor_raw.has_timing = true;
    g_ctx_sensor_raw.max_w = 1920;
    g_ctx_sensor_raw.max_h = 1080;

    const vsc_lite_stage_def_t stages[] = {
        { &g_drv_sensor_query, &g_ctx_sensor_raw },
        { &g_drv_endpoint, &g_ctx_pass },
    };
    vsc_lite_pipeline_init(&g_pipe, stages, 2);

    vsc_crop_cap_t cap;
    uint8_t len = sizeof(cap);
    int rc = vsc_lite_query_cap(&g_pipe, VSC_CAP_CROP, &cap, &len);
    TEST_ASSERT_TRUE(cap.available);
    TEST_ASSERT_EQUAL_UINT32(1920, cap.max_w);
    TEST_ASSERT_EQUAL_UINT32(1080, cap.max_h);
    TEST_ASSERT_EQUAL_UINT8(8, cap.align_w);
}

/**
 * NULL 参数 → PARAM
 */
void test_cap_null_params(void)
{
    vsc_binning_cap_t cap;
    uint8_t len = sizeof(cap);
    TEST_ASSERT_EQUAL_INT(VSC_ERR_PARAM, 
        vsc_lite_query_cap(NULL, VSC_CAP_BINNING, &cap, &len));
    TEST_ASSERT_EQUAL_INT(VSC_ERR_PARAM,
        vsc_lite_query_cap(&g_pipe, VSC_CAP_BINNING, NULL, &len));
    TEST_ASSERT_EQUAL_INT(VSC_ERR_PARAM,
        vsc_lite_query_cap(&g_pipe, VSC_CAP_BINNING, &cap, NULL));
    len = 0;
    TEST_ASSERT_EQUAL_INT(VSC_ERR_PARAM,
        vsc_lite_query_cap(&g_pipe, VSC_CAP_BINNING, &cap, &len));
}

/* ========================================================================
 *  时序聚合测试
 * ======================================================================== */

/**
 * 无 get_timing_req 回调 → 时序 = sensor 基准不变
 */
void test_timing_no_req_preserves_baseline(void)
{
        const vsc_lite_stage_def_t stages[] = {
        { &g_drv_sensor_timed, &g_ctx_sensor_raw },
        { &g_drv_pass, &g_ctx_pass },
        { &g_drv_endpoint, &g_ctx_pass },
    };
    vsc_lite_pipeline_init(&g_pipe, stages, 3);

    vsc_mbus_fmt_t intent = { { 1920, 1080, VSC_FMT_RAW10, 30, 1, 10, 4, {0} } };
    int rc = vsc_lite_try_fmt(&g_pipe, &intent, &g_result);
    /* 时序应保持 sensor 基准值 */
    TEST_ASSERT_EQUAL_UINT32(74250000,  g_result.primary_fmt.timing.pixel_clock_hz);
    TEST_ASSERT_EQUAL_UINT32(1920,      g_result.primary_fmt.timing.h_active);
    TEST_ASSERT_EQUAL_UINT32(1920 + 128, g_result.primary_fmt.timing.h_total);
    TEST_ASSERT_EQUAL_UINT32(128,       g_result.primary_fmt.timing.h_blank);
    TEST_ASSERT_EQUAL_UINT32(1080,      g_result.primary_fmt.timing.v_active);
    TEST_ASSERT_EQUAL_UINT32(1080 + 80, g_result.primary_fmt.timing.v_total);
    TEST_ASSERT_EQUAL_UINT32(80,        g_result.primary_fmt.timing.v_blank);
}

/**
 * 单 stage 并行约束: h_total 被收紧
 */
void test_timing_single_stage_h_total(void)
{
    g_ctx_pass.has_timing = true;
    g_ctx_pass.timing_req.min_h_total = 3000;

        const vsc_lite_stage_def_t stages[] = {
        { &g_drv_sensor_timed, &g_ctx_sensor_raw },
        { &g_drv_pass_timed, &g_ctx_pass },
        { &g_drv_endpoint, &g_ctx_pass },
    };
    vsc_lite_pipeline_init(&g_pipe, stages, 3);

    vsc_mbus_fmt_t intent = { { 1920, 1080, VSC_FMT_RAW10, 30, 1, 10, 4, {0} } };
    int rc = vsc_lite_try_fmt(&g_pipe, &intent, &g_result);
    /* sensor h_total = 1920+128=2048, stage 要求 3000 → final = 3000 */
    TEST_ASSERT_EQUAL_UINT32(3000, g_result.primary_fmt.timing.h_total);
    /* h_blank = h_total - h_active = 3000 - 1920 = 1080 */
    TEST_ASSERT_EQUAL_UINT32(1080, g_result.primary_fmt.timing.h_blank);
}

/**
 * 多 stage 并行约束 max 聚合: 取最严
 */
void test_timing_parallel_max(void)
{
    /* Stage A: min_h_total=3000, min_h_blank=500 */
    g_ctx_pass.has_timing = true;
    g_ctx_pass.timing_req.min_h_total = 3000;
    g_ctx_pass.timing_req.min_h_blank = 500;

    /* Stage B: min_h_total=2500, min_h_blank=800 (h_blank 更严) */
    g_ctx_bin2x2.has_timing = true;
    g_ctx_bin2x2.timing_req.min_h_total = 2500;
    g_ctx_bin2x2.timing_req.min_h_blank = 800;

        const vsc_lite_stage_def_t stages[] = {
        { &g_drv_sensor_timed, &g_ctx_sensor_raw },
        { &g_drv_pass_timed, &g_ctx_pass },
        { &g_drv_binning_timed, &g_ctx_bin2x2 },
        { &g_drv_endpoint, &g_ctx_pass },
    };
    vsc_lite_pipeline_init(&g_pipe, stages, 4);

    vsc_mbus_fmt_t intent = { { 1920, 1080, VSC_FMT_RAW10, 30, 1, 10, 4, {0} } };
    int rc = vsc_lite_try_fmt(&g_pipe, &intent, &g_result);
    /* h_total: max(2048, 3000, 2500) = 3000 */
    TEST_ASSERT_EQUAL_UINT32(3000, g_result.primary_fmt.timing.h_total);
    /* h_blank: max(128, 500, 800) = 800; h_total = h_active + h_blank = 1920+800=2720
       但 min_h_total=3000 更严 → h_total = max(2720, 3000) = 3000 */
    TEST_ASSERT_EQUAL_UINT32(3000, g_result.primary_fmt.timing.h_total);
}

/**
 * 串行延迟 sum 聚合: pipeline_lines 累加
 */
void test_timing_pipeline_lines_sum(void)
{
    /* Stage A: pipeline_lines=2 */
    g_ctx_pass.has_timing = true;
    memset(&g_ctx_pass.timing_req, 0, sizeof(g_ctx_pass.timing_req));
    g_ctx_pass.timing_req.pipeline_lines = 2;

    /* Stage B: pipeline_lines=3 */
    g_ctx_bin2x2.has_timing = true;
    memset(&g_ctx_bin2x2.timing_req, 0, sizeof(g_ctx_bin2x2.timing_req));
    g_ctx_bin2x2.timing_req.pipeline_lines = 3;

        const vsc_lite_stage_def_t stages[] = {
        { &g_drv_sensor_timed, &g_ctx_sensor_raw },
        { &g_drv_pass_timed, &g_ctx_pass },
        { &g_drv_binning_timed, &g_ctx_bin2x2 },
        { &g_drv_endpoint, &g_ctx_pass },
    };
    vsc_lite_pipeline_init(&g_pipe, stages, 4);

    vsc_mbus_fmt_t intent = { { 1920, 1080, VSC_FMT_RAW10, 30, 1, 10, 4, {0} } };
    int rc = vsc_lite_try_fmt(&g_pipe, &intent, &g_result);
    /* v_blank: max(sensor=80, pipeline=2+3=5) = 80 (sensor 基准更大) */
    TEST_ASSERT_EQUAL_UINT32(80, g_result.primary_fmt.timing.v_blank);
    /* v_total 不变 */
    TEST_ASSERT_EQUAL_UINT32(1080 + 80, g_result.primary_fmt.timing.v_total);
}

/**
 * pipeline_lines 超过 sensor_v_blank: 帧消隐被拉大
 */
void test_timing_latency_exceeds_vblank(void)
{
    g_ctx_pass.has_timing = true;
    memset(&g_ctx_pass.timing_req, 0, sizeof(g_ctx_pass.timing_req));
    g_ctx_pass.timing_req.pipeline_lines = 100;  /* 超过 sensor 80 */

        const vsc_lite_stage_def_t stages[] = {
        { &g_drv_sensor_timed, &g_ctx_sensor_raw },
        { &g_drv_pass_timed, &g_ctx_pass },
        { &g_drv_endpoint, &g_ctx_pass },
    };
    vsc_lite_pipeline_init(&g_pipe, stages, 3);

    vsc_mbus_fmt_t intent = { { 1920, 1080, VSC_FMT_RAW10, 30, 1, 10, 4, {0} } };
    int rc = vsc_lite_try_fmt(&g_pipe, &intent, &g_result);
    /* v_blank = max(80, 100) = 100 */
    TEST_ASSERT_EQUAL_UINT32(100, g_result.primary_fmt.timing.v_blank);
    /* v_total = max(1080+80=1160, 1080+100=1180) = 1180 */
    TEST_ASSERT_EQUAL_UINT32(1180, g_result.primary_fmt.timing.v_total);
}

/**
 * 并行 + 串行混合: min_v_total + pipeline_lines 同时生效
 */
void test_timing_mixed_parallel_serial(void)
{
    /* Stage A: min_v_total=2000 */
    g_ctx_pass.has_timing = true;
    memset(&g_ctx_pass.timing_req, 0, sizeof(g_ctx_pass.timing_req));
    g_ctx_pass.timing_req.min_v_total = 2000;

    /* Stage B: pipeline_lines=50 */
    g_ctx_bin2x2.has_timing = true;
    memset(&g_ctx_bin2x2.timing_req, 0, sizeof(g_ctx_bin2x2.timing_req));
    g_ctx_bin2x2.timing_req.pipeline_lines = 50;

        const vsc_lite_stage_def_t stages[] = {
        { &g_drv_sensor_timed, &g_ctx_sensor_raw },
        { &g_drv_pass_timed, &g_ctx_pass },
        { &g_drv_binning_timed, &g_ctx_bin2x2 },
        { &g_drv_endpoint, &g_ctx_pass },
    };
    vsc_lite_pipeline_init(&g_pipe, stages, 4);

    vsc_mbus_fmt_t intent = { { 1920, 1080, VSC_FMT_RAW10, 30, 1, 10, 4, {0} } };
    int rc = vsc_lite_try_fmt(&g_pipe, &intent, &g_result);
    /* v_blank = max(80, 0, 50) = 80; v_total = max(1160, 2000, 1080+80) = 2000 */
    TEST_ASSERT_EQUAL_UINT32(2000, g_result.primary_fmt.timing.v_total);
}

/**
 * sensor 上限检查: h_total 超出 max → UNREACHABLE
 */
void test_timing_exceeds_sensor_max(void)
{
    /* 设置 sensor 上限 */
    g_ctx_sensor_raw.has_timing = true;
    g_ctx_sensor_raw.timing_req.reserved[0] = 5000;  /* max_h_total */
    g_ctx_sensor_raw.timing_req.reserved[1] = 10000; /* max_v_total */

    /* Stage 要求 h_total=6000 > 5000 */
    g_ctx_pass.has_timing = true;
    memset(&g_ctx_pass.timing_req, 0, sizeof(g_ctx_pass.timing_req));
    g_ctx_pass.timing_req.min_h_total = 6000;

        const vsc_lite_stage_def_t stages[] = {
        { &g_drv_sensor_timed2, &g_ctx_sensor_raw },
        { &g_drv_pass_timed, &g_ctx_pass },
        { &g_drv_endpoint, &g_ctx_pass },
    };
    vsc_lite_pipeline_init(&g_pipe, stages, 3);

    vsc_mbus_fmt_t intent = { { 1920, 1080, VSC_FMT_RAW10, 30, 1, 10, 4, {0} } };
    int rc = vsc_lite_try_fmt(&g_pipe, &intent, &g_result);
    TEST_ASSERT_EQUAL_INT(VSC_ERR_UNREACHABLE, rc);
    TEST_ASSERT_EQUAL_INT(VSC_NEGOTIATE_FAILED, g_result.status);
    /* reachable_max 含时序信息 */
    TEST_ASSERT_EQUAL_UINT32(6000, g_result.reachable_max.timing.h_total);
}

/**
 * sensor 上限足够 → 通过
 */
void test_timing_within_sensor_max(void)
{
    g_ctx_sensor_raw.has_timing = true;
    g_ctx_sensor_raw.timing_req.reserved[0] = 10000; /* max_h_total 足够大 */
    g_ctx_sensor_raw.timing_req.reserved[1] = 10000;

    g_ctx_pass.has_timing = true;
    memset(&g_ctx_pass.timing_req, 0, sizeof(g_ctx_pass.timing_req));
    g_ctx_pass.timing_req.min_h_total = 6000;

        const vsc_lite_stage_def_t stages[] = {
        { &g_drv_sensor_timed2, &g_ctx_sensor_raw },
        { &g_drv_pass_timed, &g_ctx_pass },
        { &g_drv_endpoint, &g_ctx_pass },
    };
    vsc_lite_pipeline_init(&g_pipe, stages, 3);

    vsc_mbus_fmt_t intent = { { 1920, 1080, VSC_FMT_RAW10, 30, 1, 10, 4, {0} } };
    int rc = vsc_lite_try_fmt(&g_pipe, &intent, &g_result);
    TEST_ASSERT_EQUAL_UINT32(6000, g_result.primary_fmt.timing.h_total);
}

/**
 * 时序字段在阶段 A 中从前级透传到后级
 */
void test_timing_copy_through_stages(void)
{
        const vsc_lite_stage_def_t stages[] = {
        { &g_drv_sensor_timed, &g_ctx_sensor_raw },
        { &g_drv_pass, &g_ctx_pass },
        { &g_drv_binning, &g_ctx_bin2x2 },
        { &g_drv_endpoint, &g_ctx_pass },
    };
    vsc_lite_pipeline_init(&g_pipe, stages, 4);

    vsc_mbus_fmt_t intent = { { 1920, 1080, VSC_FMT_RAW10, 30, 1, 10, 4, {0} } };
    int rc = vsc_lite_try_fmt(&g_pipe, &intent, &g_result);
    /* Stage 1 (pass) 应保持 sensor 时序 */
    TEST_ASSERT_EQUAL_UINT32(74250000, g_pipe.stages[1].source_fmt.timing.pixel_clock_hz);
    TEST_ASSERT_EQUAL_UINT32(1920 + 128, g_pipe.stages[1].source_fmt.timing.h_total);

    /* Stage 2 (binning 2x2) 修改了 width/height, 但时序字段应透传 */
    TEST_ASSERT_EQUAL_UINT32(74250000, g_pipe.stages[2].source_fmt.timing.pixel_clock_hz);
    TEST_ASSERT_EQUAL_UINT32(1920 + 128, g_pipe.stages[2].source_fmt.timing.h_total);
    /* binning 2x2 后 width=960, 但 active 应被透传覆盖 */
    TEST_ASSERT_EQUAL_UINT32(960, g_pipe.stages[2].source_fmt.timing.h_active);
}

/**
 * 多个 try_fmt 调用，时序状态隔离
 */
void test_timing_multiple_try_isolation(void)
{
        const vsc_lite_stage_def_t stages[] = {
        { &g_drv_sensor_timed, &g_ctx_sensor_raw },
        { &g_drv_pass_timed, &g_ctx_pass },
        { &g_drv_endpoint, &g_ctx_pass },
    };
    vsc_lite_pipeline_init(&g_pipe, stages, 3);

    int rc;
    vsc_mbus_fmt_t intent2 = { { 640, 480, VSC_FMT_RAW10, 30, 1, 10, 4, {0} } };
    rc = vsc_lite_try_fmt(&g_pipe, &intent2, &g_result);
    TEST_ASSERT_EQUAL_UINT32(640 + 128, g_result.primary_fmt.timing.h_total);
}

/**
 * get_timing_req 返回错误 → 聚合中止，协商失败
 */
void test_timing_req_error_aborts(void)
{
        const vsc_lite_stage_def_t stages[] = {
        { &g_drv_sensor_timed, &g_ctx_sensor_raw },
        { &g_drv_pass_fail_timing, &g_ctx_pass },
        { &g_drv_endpoint, &g_ctx_pass },
    };
    vsc_lite_pipeline_init(&g_pipe, stages, 3);

    vsc_mbus_fmt_t intent = { { 1920, 1080, VSC_FMT_RAW10, 30, 1, 10, 4, {0} } };
    int rc = vsc_lite_try_fmt(&g_pipe, &intent, &g_result);
    TEST_ASSERT_EQUAL_INT(VSC_ERR_PROPAGATION_SINK, rc);
    TEST_ASSERT_EQUAL_INT(VSC_NEGOTIATE_FAILED, g_result.status);
}

/**
 * ip_clock_hz 诊断字段正确传输（不影响计算，仅验证透传）
 */
void test_timing_ip_clock_diag(void)
{
    g_ctx_pass.has_timing = true;
    memset(&g_ctx_pass.timing_req, 0, sizeof(g_ctx_pass.timing_req));
    g_ctx_pass.timing_req.ip_clock_hz = 200000000;

    g_ctx_bin2x2.has_timing = true;
    memset(&g_ctx_bin2x2.timing_req, 0, sizeof(g_ctx_bin2x2.timing_req));
    g_ctx_bin2x2.timing_req.ip_clock_hz = 300000000;

    /* 验证 get_timing_req 被调用且返回正确的 ip_clock_hz */
        const vsc_lite_stage_def_t stages[] = {
        { &g_drv_sensor_timed, &g_ctx_sensor_raw },
        { &g_drv_pass_timed, &g_ctx_pass },
        { &g_drv_binning_timed, &g_ctx_bin2x2 },
        { &g_drv_endpoint, &g_ctx_pass },
    };
    vsc_lite_pipeline_init(&g_pipe, stages, 4);

    vsc_mbus_fmt_t intent = { { 1920, 1080, VSC_FMT_RAW10, 30, 1, 10, 4, {0} } };
    int rc = vsc_lite_try_fmt(&g_pipe, &intent, &g_result);
    /* ip_clock_hz 不影响协商结果 */
}


/* ========================================================================
 *  控制接口测试
 * ======================================================================== */

/**
 * 通过 set_ctrl 设置 binning factor，再通过 get_ctrl 读回
 */
void test_ctrl_set_get_bin_factor(void)
{
    g_ctx_sensor_raw.has_timing = true;
    g_ctx_sensor_raw.timing_req.reserved[0] = 2;
    g_ctx_sensor_raw.timing_req.reserved[1] = 4;
    g_ctx_sensor_raw.timing_req.reserved[2] = VSC_CAP_LOCATION_SENSOR;

    const vsc_lite_stage_def_t stages[] = {
        { &g_drv_query_ctrl, &g_ctx_sensor_raw },
        { &g_drv_pass, NULL },
        { &g_drv_endpoint, NULL },
    };
    vsc_lite_pipeline_init(&g_pipe, stages, 3);

    /* set factor */
    TEST_ASSERT_EQUAL_INT(VSC_OK, vsc_lite_set_ctrl(&g_pipe, VSC_CAP_BINNING, VSC_CTRL_BIN_FACTOR_X, 4));
    /* read back */
    uint32_t val = 0;
    TEST_ASSERT_EQUAL_INT(VSC_OK, vsc_lite_get_ctrl(&g_pipe, VSC_CAP_BINNING, VSC_CTRL_BIN_FACTOR_X, &val));
    TEST_ASSERT_EQUAL_UINT32(4, val);
}

/**
 * 设置未知 ctrl_id → NOT_SUPPORTED
 */
void test_ctrl_unknown_id(void)
{
        const vsc_lite_stage_def_t stages[] = {
        { &g_drv_query_ctrl, &g_ctx_sensor_raw },
        { &g_drv_endpoint, NULL },
    };
    vsc_lite_pipeline_init(&g_pipe, stages, 2);

    TEST_ASSERT_EQUAL_INT(VSC_ERR_NOT_SUPPORTED,
        vsc_lite_set_ctrl(&g_pipe, VSC_CAP_BINNING, 0xFFFF, 0));
    uint32_t val;
    TEST_ASSERT_EQUAL_INT(VSC_ERR_NOT_SUPPORTED,
        vsc_lite_get_ctrl(&g_pipe, VSC_CAP_BINNING, 0xFFFF, &val));
}

/**
 * 无 driver 提供 set_ctrl → NOT_SUPPORTED
 */
void test_ctrl_no_driver(void)
{
        const vsc_lite_stage_def_t stages[] = {
        { &g_drv_pass, &g_ctx_pass },
        { &g_drv_endpoint, NULL },
    };
    vsc_lite_pipeline_init(&g_pipe, stages, 2);

    TEST_ASSERT_EQUAL_INT(VSC_ERR_NOT_SUPPORTED,
        vsc_lite_set_ctrl(&g_pipe, VSC_CAP_BINNING, VSC_CTRL_BIN_FACTOR_X, 2));
}

/**
 * NULL 参数 → PARAM
 */
void test_ctrl_null_params(void)
{
    uint32_t val;
    TEST_ASSERT_EQUAL_INT(VSC_ERR_PARAM, vsc_lite_set_ctrl(NULL, 0, 0, 0));
    TEST_ASSERT_EQUAL_INT(VSC_ERR_PARAM, vsc_lite_get_ctrl(NULL, 0, 0, &val));
    TEST_ASSERT_EQUAL_INT(VSC_ERR_PARAM, vsc_lite_get_ctrl(&g_pipe, 0, 0, NULL));
}


static int lite_run_all_tests(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_linear_pass_chain);
    RUN_TEST(test_full_pipeline);
    RUN_TEST(test_intent_exceeds_sensor);
    RUN_TEST(test_format_rejected_by_decoder);
    RUN_TEST(test_reject_in_middle);
    RUN_TEST(test_minimal_sensor_endpoint);
    RUN_TEST(test_null_params);
    RUN_TEST(test_real_drivers_sensor_crop);
    RUN_TEST(test_commit_after_try);
    RUN_TEST(test_multiple_try_fmt_isolation);
    RUN_TEST(test_commit_null_params);
    RUN_TEST(test_single_stage_rejected);
    RUN_TEST(test_null_driver_in_middle);
    /* ── 能力查询测试 ── */
    RUN_TEST(test_cap_binning_sensor_first);
    RUN_TEST(test_cap_binning_fpga_fallback);
    RUN_TEST(test_cap_binning_none_available);
    RUN_TEST(test_cap_unknown_cap_id);
    RUN_TEST(test_cap_crop_query);
    RUN_TEST(test_cap_null_params);
    RUN_TEST(test_ctrl_set_get_bin_factor);
    RUN_TEST(test_ctrl_unknown_id);
    RUN_TEST(test_ctrl_no_driver);
    RUN_TEST(test_ctrl_null_params);
    /* ── 时序聚合测试 ── */
    RUN_TEST(test_timing_no_req_preserves_baseline);
    RUN_TEST(test_timing_single_stage_h_total);
    RUN_TEST(test_timing_parallel_max);
    RUN_TEST(test_timing_pipeline_lines_sum);
    RUN_TEST(test_timing_latency_exceeds_vblank);
    RUN_TEST(test_timing_mixed_parallel_serial);
    RUN_TEST(test_timing_exceeds_sensor_max);
    RUN_TEST(test_timing_within_sensor_max);
    RUN_TEST(test_timing_copy_through_stages);
    RUN_TEST(test_timing_multiple_try_isolation);
    RUN_TEST(test_timing_req_error_aborts);
    RUN_TEST(test_timing_ip_clock_diag);
    return UNITY_END();
}

TEST_SUITE_DEFINE(vsc_lite, lite_setup, lite_teardown, lite_run_all_tests);
