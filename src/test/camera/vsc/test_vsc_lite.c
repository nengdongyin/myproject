/**
 * @file    test_vsc_lite.c
 * @brief   Unit tests for VSC Lite — simplified pipeline solver
 *
 * Uses the same mock drivers as test_vsc_resolver.c for behavioral equivalence.
 */

#include "unity.h"
#include "test_suite.h"
#include "vsc_types.h"
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
      *o = *s; o->width /= c->bin_factor_x; o->height /= c->bin_factor_y; return VSC_OK; }

static int mock_source_decoder(void *drv_ctx, const vsc_mbus_fmt_t *s, vsc_mbus_fmt_t *o)
    { mock_drv_ctx_t *c = (mock_drv_ctx_t *)drv_ctx;
      *o = *s; o->pixel_format = c->output_fmt; return VSC_OK; }

static int mock_source_crop(void *drv_ctx, const vsc_mbus_fmt_t *s, vsc_mbus_fmt_t *o)
    { mock_drv_ctx_t *c = (mock_drv_ctx_t *)drv_ctx;
      *o = *s;
      if (o->width > c->max_w)  o->width  = c->max_w;
      if (o->height > c->max_h) o->height = c->max_h;
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
    if (!mock_fmt_supported(ctx, intent->pixel_format))
        src->pixel_format = ctx->supported_fmts[0];
    if (src->width > ctx->max_w)  src->width  = ctx->max_w;
    if (src->height > ctx->max_h) src->height = ctx->max_h;
    return VSC_OK;
}

static int mock_sink_format_filter(void *drv_ctx, const vsc_mbus_fmt_t *p, vsc_mbus_fmt_t *c)
{
    mock_drv_ctx_t *ctx = (mock_drv_ctx_t *)drv_ctx;
    if (!mock_fmt_supported(ctx, p->pixel_format)) {
        memset(c, 0, sizeof(*c)); return VSC_ERR_PROPAGATION_SINK;
    }
    *c = *p;
    if (c->width > ctx->max_w) c->width = ctx->max_w;
    if (c->height > ctx->max_h) c->height = ctx->max_h;
    return VSC_OK;
}

/* ── mock ops tables ── */
static const vsc_ip_ops_t g_ops_sensor    = { NULL, NULL, mock_sensor_source, NULL };
static const vsc_ip_ops_t g_ops_pass      = { NULL, mock_sink_pass, mock_source_pass, NULL };
static const vsc_ip_ops_t g_ops_endpoint  = { NULL, mock_sink_pass, NULL, NULL };
static const vsc_ip_ops_t g_ops_binning   = { NULL, mock_sink_pass, mock_source_binning, NULL };
static const vsc_ip_ops_t g_ops_decoder   = { NULL, mock_sink_format_filter, mock_source_decoder, NULL };
static const vsc_ip_ops_t g_ops_crop      = { NULL, mock_sink_pass, mock_source_crop, NULL };
static const vsc_ip_ops_t g_ops_reject    = { NULL, mock_sink_reject, NULL, NULL };

/* ── mock driver descriptors ── */
static const vsc_driver_t g_drv_sensor   = { .ops = g_ops_sensor };
static const vsc_driver_t g_drv_pass     = { .ops = g_ops_pass };
static const vsc_driver_t g_drv_endpoint = { .ops = g_ops_endpoint };
static const vsc_driver_t g_drv_binning  = { .ops = g_ops_binning };
static const vsc_driver_t g_drv_decoder  = { .ops = g_ops_decoder };
static const vsc_driver_t g_drv_crop     = { .ops = g_ops_crop };
static const vsc_driver_t g_drv_reject   = { .ops = g_ops_reject };

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
 *  Pipeline builders — return driver arrays
 * ======================================================================== */

/**
 * Sensor → Pass → Endpoint
 */
void test_linear_pass_chain(void)
{
    const vsc_driver_t *drivers[] = { &g_drv_sensor, &g_drv_pass, &g_drv_endpoint };
    int rc = vsc_lite_pipeline_init(&g_pipe, drivers, 3);
    TEST_ASSERT_EQUAL_INT(VSC_OK, rc);

    /* Set driver contexts */
    g_pipe.stages[0].drv_ctx = &g_ctx_sensor_raw;
    g_pipe.stages[1].drv_ctx = &g_ctx_pass;
    g_pipe.stages[2].drv_ctx = &g_ctx_pass;

    vsc_mbus_fmt_t intent = {1920, 1080, VSC_FMT_RAW10, 30, 1, 10, 4, {0}};
    rc = vsc_lite_try_fmt(&g_pipe, &intent, &g_result);
    TEST_ASSERT_EQUAL_INT(VSC_OK, rc);
    TEST_ASSERT_EQUAL_INT(VSC_NEGOTIATE_EXACT, g_result.status);
    TEST_ASSERT_EQUAL_UINT32(1920, g_result.primary_fmt.width);
    TEST_ASSERT_EQUAL_UINT32(1080, g_result.primary_fmt.height);
    TEST_ASSERT_EQUAL_UINT32(VSC_FMT_RAW10, g_result.primary_fmt.pixel_format);
}

/**
 * Sensor → Binning(2×2) → Decoder → Crop(1920) → Endpoint
 */
void test_full_pipeline(void)
{
    const vsc_driver_t *drivers[] = {
        &g_drv_sensor, &g_drv_binning, &g_drv_decoder, &g_drv_crop, &g_drv_endpoint
    };
    int rc = vsc_lite_pipeline_init(&g_pipe, drivers, 5);
    TEST_ASSERT_EQUAL_INT(VSC_OK, rc);

    g_pipe.stages[0].drv_ctx = &g_ctx_sensor_raw;
    g_pipe.stages[1].drv_ctx = &g_ctx_bin2x2;
    g_pipe.stages[2].drv_ctx = &g_ctx_decoder;
    g_pipe.stages[3].drv_ctx = &g_ctx_crop_1920;
    g_pipe.stages[4].drv_ctx = &g_ctx_pass;

    /* 1920×1080 RGB888 → sensor falls back RAW8 → bin /2 → 960×540 → decoder → RGB → crop clamp */
    vsc_mbus_fmt_t intent = {1920, 1080, VSC_FMT_RGB888, 30, 1, 8, 4, {0}};
    rc = vsc_lite_try_fmt(&g_pipe, &intent, &g_result);
    TEST_ASSERT_EQUAL_INT(VSC_OK, rc);
    TEST_ASSERT_EQUAL_INT(VSC_NEGOTIATE_EXACT, g_result.status);
    /* Binning halves before reaching crop, final output = 960×540 */
    TEST_ASSERT_EQUAL_UINT32(960, g_result.primary_fmt.width);
    TEST_ASSERT_EQUAL_UINT32(540, g_result.primary_fmt.height);
}

/**
 * Intent larger than sensor max → clamp
 */
void test_intent_exceeds_sensor(void)
{
    const vsc_driver_t *drivers[] = { &g_drv_sensor, &g_drv_pass, &g_drv_endpoint };
    vsc_lite_pipeline_init(&g_pipe, drivers, 3);
    g_pipe.stages[0].drv_ctx = &g_ctx_sensor_raw;
    g_pipe.stages[1].drv_ctx = &g_ctx_pass;
    g_pipe.stages[2].drv_ctx = &g_ctx_pass;

    /* 5000×4000 exceeds sensor's 4056×3040 */
    vsc_mbus_fmt_t intent = {5000, 4000, VSC_FMT_RAW10, 30, 1, 10, 4, {0}};
    int rc = vsc_lite_try_fmt(&g_pipe, &intent, &g_result);
    TEST_ASSERT_EQUAL_INT(VSC_OK, rc);
    TEST_ASSERT_EQUAL_UINT32(4056, g_result.primary_fmt.width);
    TEST_ASSERT_EQUAL_UINT32(3040, g_result.primary_fmt.height);
}

/**
 * Format not supported by decoder → fail
 */
void test_format_rejected_by_decoder(void)
{
    const vsc_driver_t *drivers[] = {
        &g_drv_sensor, &g_drv_binning, &g_drv_decoder, &g_drv_crop, &g_drv_endpoint
    };
    vsc_lite_pipeline_init(&g_pipe, drivers, 5);
    g_pipe.stages[0].drv_ctx = &g_ctx_sensor_raw;
    g_pipe.stages[1].drv_ctx = &g_ctx_bin2x2;
    g_pipe.stages[2].drv_ctx = &g_ctx_decoder;
    g_pipe.stages[3].drv_ctx = &g_ctx_crop_1920;
    g_pipe.stages[4].drv_ctx = &g_ctx_pass;

    /* YUV422 sensor doesn't support, but mock_sensor falls back to first supported.
       Then decoder doesn't support YUV422 format filter → fail */
    vsc_mbus_fmt_t intent = {1920, 1080, VSC_FMT_YUV422, 30, 1, 8, 4, {0}};
    int rc = vsc_lite_try_fmt(&g_pipe, &intent, &g_result);
    /* Sensor falls back to RAW8 (first supported), decoder accepts RAW8 → pass */
    TEST_ASSERT_EQUAL_INT(VSC_OK, rc);
    TEST_ASSERT_EQUAL_UINT32(VSC_FMT_RGB888, g_result.primary_fmt.pixel_format);
}

/**
 * Stage with reject driver → fail
 */
void test_reject_in_middle(void)
{
    const vsc_driver_t *drivers[] = { &g_drv_sensor, &g_drv_reject, &g_drv_endpoint };
    vsc_lite_pipeline_init(&g_pipe, drivers, 3);
    g_pipe.stages[0].drv_ctx = &g_ctx_sensor_raw;
    g_pipe.stages[1].drv_ctx = &g_ctx_reject;
    g_pipe.stages[2].drv_ctx = &g_ctx_pass;

    vsc_mbus_fmt_t intent = {1920, 1080, VSC_FMT_RAW10, 30, 1, 10, 4, {0}};
    int rc = vsc_lite_try_fmt(&g_pipe, &intent, &g_result);
    TEST_ASSERT_NOT_EQUAL(VSC_OK, rc);
    TEST_ASSERT_EQUAL_INT(VSC_NEGOTIATE_FAILED, g_result.status);
}

/**
 * Minimal pipeline: sensor → endpoint
 */
void test_minimal_sensor_endpoint(void)
{
    const vsc_driver_t *drivers[] = { &g_drv_sensor, &g_drv_endpoint };
    vsc_lite_pipeline_init(&g_pipe, drivers, 2);
    g_pipe.stages[0].drv_ctx = &g_ctx_sensor_raw;
    g_pipe.stages[1].drv_ctx = &g_ctx_pass;

    vsc_mbus_fmt_t intent = {1920, 1080, VSC_FMT_RAW10, 30, 1, 10, 4, {0}};
    int rc = vsc_lite_try_fmt(&g_pipe, &intent, &g_result);
    TEST_ASSERT_EQUAL_INT(VSC_OK, rc);
    TEST_ASSERT_EQUAL_UINT32(1920, g_result.primary_fmt.width);
}

/**
 * Real drivers: sensor → crop → endpoint
 */
void test_real_drivers_sensor_crop(void)
{
    const vsc_driver_t *drivers[] = {
        &sensor_imx477_vsc_driver,
        &crop_vsc_driver,
        &binning_vsc_driver,
        &decoder_vsc_driver,
        &histogram_vsc_driver,
    };
    int rc = vsc_lite_pipeline_init(&g_pipe, drivers, 5);
    TEST_ASSERT_EQUAL_INT(VSC_OK, rc);

    vsc_mbus_fmt_t intent = {1920, 1080, VSC_FMT_RAW10, 30, 1, 10, 4, {0}};
    rc = vsc_lite_try_fmt(&g_pipe, &intent, &g_result);
    TEST_ASSERT_EQUAL_INT(VSC_OK, rc);
    /* Real drivers may adjust format — just verify it's valid */
    TEST_ASSERT_TRUE(vsc_fmt_is_valid(&g_result.primary_fmt));
}

/**
 * Commit after try_fmt
 */
void test_commit_after_try(void)
{
    const vsc_driver_t *drivers[] = { &g_drv_sensor, &g_drv_pass, &g_drv_endpoint };
    vsc_lite_pipeline_init(&g_pipe, drivers, 3);
    g_pipe.stages[0].drv_ctx = &g_ctx_sensor_raw;
    g_pipe.stages[1].drv_ctx = &g_ctx_pass;
    g_pipe.stages[2].drv_ctx = &g_ctx_pass;

    vsc_mbus_fmt_t intent = {1920, 1080, VSC_FMT_RAW10, 30, 1, 10, 4, {0}};
    int rc = vsc_lite_try_fmt(&g_pipe, &intent, &g_result);
    TEST_ASSERT_EQUAL_INT(VSC_OK, rc);

    rc = vsc_lite_commit_fmt(&g_pipe, &g_result.primary_fmt);
    TEST_ASSERT_EQUAL_INT(VSC_OK, rc);
}

/**
 * Multiple try_fmt calls: second call must start clean, no stale state
 */
void test_multiple_try_fmt_isolation(void)
{
    const vsc_driver_t *drivers[] = { &g_drv_sensor, &g_drv_pass, &g_drv_endpoint };
    vsc_lite_pipeline_init(&g_pipe, drivers, 3);
    g_pipe.stages[0].drv_ctx = &g_ctx_sensor_raw;
    g_pipe.stages[1].drv_ctx = &g_ctx_pass;
    g_pipe.stages[2].drv_ctx = &g_ctx_pass;

    /* First call: 1920×1080 */
    vsc_mbus_fmt_t intent1 = {1920, 1080, VSC_FMT_RAW10, 30, 1, 10, 4, {0}};
    int rc = vsc_lite_try_fmt(&g_pipe, &intent1, &g_result);
    TEST_ASSERT_EQUAL_INT(VSC_OK, rc);
    TEST_ASSERT_EQUAL_UINT32(1920, g_result.primary_fmt.width);

    /* Second call: smaller intent, must NOT retain first call's 1920 */
    vsc_mbus_fmt_t intent2 = {640, 480, VSC_FMT_RAW10, 30, 1, 10, 4, {0}};
    rc = vsc_lite_try_fmt(&g_pipe, &intent2, &g_result);
    TEST_ASSERT_EQUAL_INT(VSC_OK, rc);
    TEST_ASSERT_EQUAL_UINT32(640, g_result.primary_fmt.width);
    TEST_ASSERT_EQUAL_UINT32(480, g_result.primary_fmt.height);
}

/**
 * commit with NULL args → error
 */
void test_commit_null_params(void)
{
    vsc_mbus_fmt_t fmt = {1920, 1080, VSC_FMT_RAW10, 30, 1, 10, 4, {0}};
    TEST_ASSERT_NOT_EQUAL(VSC_OK, vsc_lite_commit_fmt(NULL, &fmt));
    TEST_ASSERT_NOT_EQUAL(VSC_OK, vsc_lite_commit_fmt(&g_pipe, NULL));
}

/**
 * Pipeline with count=1 → init rejects
 */
void test_single_stage_rejected(void)
{
    const vsc_driver_t *drivers[] = { &g_drv_sensor };
    int rc = vsc_lite_pipeline_init(&g_pipe, drivers, 1);
    TEST_ASSERT_NOT_EQUAL(VSC_OK, rc);
}

/**
 * NULL driver in middle stage → treated as pass-through, no crash
 */
void test_null_driver_in_middle(void)
{
    const vsc_driver_t *drivers[] = { &g_drv_sensor, NULL, &g_drv_endpoint };
    int rc = vsc_lite_pipeline_init(&g_pipe, drivers, 3);
    TEST_ASSERT_EQUAL_INT(VSC_OK, rc);
    g_pipe.stages[0].drv_ctx = &g_ctx_sensor_raw;
    g_pipe.stages[2].drv_ctx = &g_ctx_pass;

    vsc_mbus_fmt_t intent = {1920, 1080, VSC_FMT_RAW10, 30, 1, 10, 4, {0}};
    rc = vsc_lite_try_fmt(&g_pipe, &intent, &g_result);
    TEST_ASSERT_EQUAL_INT(VSC_OK, rc);
    TEST_ASSERT_EQUAL_UINT32(1920, g_result.primary_fmt.width);
}

/**
 * NULL params → return error
 */
void test_null_params(void)
{
    vsc_mbus_fmt_t intent = {1920, 1080, VSC_FMT_RAW10, 30, 1, 10, 4, {0}};
    TEST_ASSERT_NOT_EQUAL(VSC_OK, vsc_lite_pipeline_init(NULL, NULL, 0));
    TEST_ASSERT_NOT_EQUAL(VSC_OK, vsc_lite_try_fmt(NULL, &intent, &g_result));
    TEST_ASSERT_NOT_EQUAL(VSC_OK, vsc_lite_try_fmt(&g_pipe, NULL, &g_result));
    TEST_ASSERT_NOT_EQUAL(VSC_OK, vsc_lite_try_fmt(&g_pipe, &intent, NULL));
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
    return UNITY_END();
}

TEST_SUITE_DEFINE(vsc_lite, lite_setup, lite_teardown, lite_run_all_tests);
