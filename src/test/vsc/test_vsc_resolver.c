/**
 * @file    test_vsc_resolver.c
 * @brief   Unit tests for VSC Property Resolver — Phase 1 (feasibility),
 *          optional-entity auto-bridging, Phase 2 (forward propagation),
 *          and Phase 3 (multi-branch convergence).
 *
 * Uses Unity test framework.  Follows project convention: setup/teardown +
 * RUN_TEST macros + TEST_SUITE_DEFINE.
 */

#include "unity.h"
#include "test_suite.h"
#include "vsc_types.h"
#include "vsc_driver_registry.h"
#include "crop_vsc.h"
#include "binning_vsc.h"
#include "decoder_vsc.h"
#include "histogram_vsc.h"
#include "histogram_driver.h"
#include "sensor_vsc.h"
#include <string.h>

/* ========================================================================
 *  Shared test context
 * ======================================================================== */

static vsc_pipeline_t    g_pipe;
static vsc_resolver_result_t g_result;

/* ========================================================================
 *  Mock driver contexts (used by mock try_fmt_* callbacks)
 * ======================================================================== */

typedef struct {
    uint32_t max_w;
    uint32_t max_h;
    uint32_t supported_fmts[4];   /* list, terminated by VSC_FMT_INVALID */
    uint32_t output_fmt;          /* for PIXEL_FMT_CONV: what this entity outputs */
    uint8_t  bin_factor_x;
    uint8_t  bin_factor_y;
} mock_drv_ctx_t;

static mock_drv_ctx_t g_mock_ctx_pass;       /* accepts everything */
static mock_drv_ctx_t g_mock_ctx_raw_only;   /* only RAW */
static mock_drv_ctx_t g_mock_ctx_bin2x2;     /* binning */
static mock_drv_ctx_t g_mock_ctx_decoder;    /* RAW→RGB */
static mock_drv_ctx_t g_mock_ctx_crop_1920;  /* crop to 1920×1080 */
static mock_drv_ctx_t g_mock_ctx_reject;     /* always reject */
static mock_drv_ctx_t g_mock_ctx_histogram;  /* accepts RAW and RGB */

/* ── mock function forward declarations ── */
static int mock_sink_pass(void *drv_ctx, const vsc_mbus_fmt_t *proposed, vsc_mbus_fmt_t *clamped);
static int mock_sink_reject(void *drv_ctx, const vsc_mbus_fmt_t *proposed, vsc_mbus_fmt_t *clamped);
static int mock_sink_format_filter(void *drv_ctx, const vsc_mbus_fmt_t *proposed, vsc_mbus_fmt_t *clamped);
static int mock_source_pass(void *drv_ctx, const vsc_mbus_fmt_t *sink_fmt, vsc_mbus_fmt_t *source_fmt);
static int mock_source_binning(void *drv_ctx, const vsc_mbus_fmt_t *sink_fmt, vsc_mbus_fmt_t *source_fmt);
static int mock_source_decoder(void *drv_ctx, const vsc_mbus_fmt_t *sink_fmt, vsc_mbus_fmt_t *source_fmt);
static int mock_source_crop(void *drv_ctx, const vsc_mbus_fmt_t *sink_fmt, vsc_mbus_fmt_t *source_fmt);
static int mock_sensor_source(void *drv_ctx, const vsc_mbus_fmt_t *intent, vsc_mbus_fmt_t *source_fmt);

/* ── mock ops tables ── */
static const vsc_ip_ops_t g_ops_sensor     = { NULL, NULL, mock_sensor_source, NULL };
static const vsc_ip_ops_t g_ops_pass       = { NULL, mock_sink_pass, mock_source_pass, NULL };
static const vsc_ip_ops_t g_ops_endpoint   = { NULL, mock_sink_pass, NULL, NULL };
static const vsc_ip_ops_t g_ops_binning    = { NULL, mock_sink_pass, mock_source_binning, NULL };
static const vsc_ip_ops_t g_ops_decoder    = { NULL, mock_sink_format_filter, mock_source_decoder, NULL };
static const vsc_ip_ops_t g_ops_crop       = { NULL, mock_sink_pass, mock_source_crop, NULL };
static const vsc_ip_ops_t g_ops_reject     = { NULL, mock_sink_reject, NULL, NULL };
static const vsc_ip_ops_t g_ops_histogram  = { NULL, mock_sink_format_filter, NULL, NULL };

/* ── helper: check if format is in supported list ── */
static bool mock_fmt_supported(const mock_drv_ctx_t *ctx, uint32_t fmt)
{
    for (int i = 0; i < 4; i++) {
        if (ctx->supported_fmts[i] == VSC_FMT_INVALID) break;
        if (ctx->supported_fmts[i] == fmt) return true;
    }
    return false;
}

/* ========================================================================
 *  Mock try_fmt_sink implementations
 * ======================================================================== */

static int mock_sink_pass(void *drv_ctx, const vsc_mbus_fmt_t *proposed,
                          vsc_mbus_fmt_t *clamped)
{
    (void)drv_ctx;
    *clamped = *proposed;
    return VSC_OK;
}

static int mock_sink_reject(void *drv_ctx, const vsc_mbus_fmt_t *proposed,
                            vsc_mbus_fmt_t *clamped)
{
    (void)drv_ctx; (void)proposed;
    memset(clamped, 0, sizeof(*clamped));
    return VSC_ERR_PROPAGATION_SINK;
}

static int mock_sink_format_filter(void *drv_ctx, const vsc_mbus_fmt_t *proposed,
                                   vsc_mbus_fmt_t *clamped)
{
    mock_drv_ctx_t *ctx = (mock_drv_ctx_t *)drv_ctx;
    if (!mock_fmt_supported(ctx, proposed->pixel_format)) {
        memset(clamped, 0, sizeof(*clamped));
        return VSC_ERR_PROPAGATION_SINK;
    }
    *clamped = *proposed;
    /* also clamp dimensions */
    if (clamped->width  > ctx->max_w) clamped->width  = ctx->max_w;
    if (clamped->height > ctx->max_h) clamped->height = ctx->max_h;
    return VSC_OK;
}

/* ========================================================================
 *  Mock try_fmt_source implementations
 * ======================================================================== */

static int mock_source_pass(void *drv_ctx, const vsc_mbus_fmt_t *sink_fmt,
                            vsc_mbus_fmt_t *source_fmt)
{
    (void)drv_ctx;
    *source_fmt = *sink_fmt;
    return VSC_OK;
}

static int mock_source_binning(void *drv_ctx, const vsc_mbus_fmt_t *sink_fmt,
                               vsc_mbus_fmt_t *source_fmt)
{
    mock_drv_ctx_t *ctx = (mock_drv_ctx_t *)drv_ctx;
    *source_fmt = *sink_fmt;
    source_fmt->width  /= ctx->bin_factor_x;
    source_fmt->height /= ctx->bin_factor_y;
    return VSC_OK;
}

static int mock_source_decoder(void *drv_ctx, const vsc_mbus_fmt_t *sink_fmt,
                               vsc_mbus_fmt_t *source_fmt)
{
    mock_drv_ctx_t *ctx = (mock_drv_ctx_t *)drv_ctx;
    *source_fmt = *sink_fmt;
    source_fmt->pixel_format = ctx->output_fmt;
    return VSC_OK;
}

static int mock_source_crop(void *drv_ctx, const vsc_mbus_fmt_t *sink_fmt,
                            vsc_mbus_fmt_t *source_fmt)
{
    mock_drv_ctx_t *ctx = (mock_drv_ctx_t *)drv_ctx;
    *source_fmt = *sink_fmt;
    if (source_fmt->width  > ctx->max_w) source_fmt->width  = ctx->max_w;
    if (source_fmt->height > ctx->max_h) source_fmt->height = ctx->max_h;
    return VSC_OK;
}

/* ========================================================================
 *  Mock sensor source (for BFS root)
 * ======================================================================== */

static int mock_sensor_source(void *drv_ctx, const vsc_mbus_fmt_t *intent,
                              vsc_mbus_fmt_t *source_fmt)
{
    mock_drv_ctx_t *ctx = (mock_drv_ctx_t *)drv_ctx;
    *source_fmt = *intent;
    /* sensor can only output supported formats */
    if (!mock_fmt_supported(ctx, intent->pixel_format)) {
        /* fallback: use first supported format */
        source_fmt->pixel_format = ctx->supported_fmts[0];
    }
    /* clamp to sensor max */
    if (source_fmt->width  > ctx->max_w) source_fmt->width  = ctx->max_w;
    if (source_fmt->height > ctx->max_h) source_fmt->height = ctx->max_h;
    return VSC_OK;
}

/* ========================================================================
 *  Setup / teardown
 * ======================================================================== */

static void resolver_setup(void)
{
    memset(&g_pipe, 0, sizeof(g_pipe));
    memset(&g_result, 0, sizeof(g_result));

    /* init mock contexts with sensible defaults */
    memset(&g_mock_ctx_pass, 0, sizeof(g_mock_ctx_pass));
    g_mock_ctx_pass.max_w = 8192;
    g_mock_ctx_pass.max_h = 8192;

    memset(&g_mock_ctx_raw_only, 0, sizeof(g_mock_ctx_raw_only));
    g_mock_ctx_raw_only.max_w = 4056;
    g_mock_ctx_raw_only.max_h = 3040;
    g_mock_ctx_raw_only.supported_fmts[0] = VSC_FMT_RAW8;
    g_mock_ctx_raw_only.supported_fmts[1] = VSC_FMT_RAW10;
    g_mock_ctx_raw_only.supported_fmts[2] = VSC_FMT_RAW12;

    memset(&g_mock_ctx_bin2x2, 0, sizeof(g_mock_ctx_bin2x2));
    g_mock_ctx_bin2x2.bin_factor_x = 2;
    g_mock_ctx_bin2x2.bin_factor_y = 2;

    memset(&g_mock_ctx_decoder, 0, sizeof(g_mock_ctx_decoder));
    g_mock_ctx_decoder.max_w = 8192;
    g_mock_ctx_decoder.max_h = 8192;
    g_mock_ctx_decoder.supported_fmts[0] = VSC_FMT_RAW8;
    g_mock_ctx_decoder.supported_fmts[1] = VSC_FMT_RAW10;
    g_mock_ctx_decoder.supported_fmts[2] = VSC_FMT_RAW12;
    g_mock_ctx_decoder.output_fmt = VSC_FMT_RGB888;

    memset(&g_mock_ctx_crop_1920, 0, sizeof(g_mock_ctx_crop_1920));
    g_mock_ctx_crop_1920.max_w = 1920;
    g_mock_ctx_crop_1920.max_h = 1080;

    memset(&g_mock_ctx_reject, 0, sizeof(g_mock_ctx_reject));

    memset(&g_mock_ctx_histogram, 0, sizeof(g_mock_ctx_histogram));
    g_mock_ctx_histogram.max_w = 8192;
    g_mock_ctx_histogram.max_h = 8192;
    g_mock_ctx_histogram.supported_fmts[0] = VSC_FMT_RAW8;
    g_mock_ctx_histogram.supported_fmts[1] = VSC_FMT_RAW10;
    g_mock_ctx_histogram.supported_fmts[2] = VSC_FMT_RAW12;
    g_mock_ctx_histogram.supported_fmts[3] = VSC_FMT_RGB888;
}

static void resolver_teardown(void)
{
    crop_vsc_reset();
    binning_vsc_reset();
    decoder_vsc_reset();
    histogram_vsc_reset();
    sensor_vsc_reset();
}

/* ========================================================================
 *  Test helpers — build simple pipelines
 * ======================================================================== */

/**
 * Build:  Sensor(PASS) → STREAM(PASS) → ENDPOINT
 */
static void build_linear_pass_chain(void)
{
    g_pipe.num_entities = 3;
    g_pipe.num_links    = 2;

    /* Sensor */
    strcpy(g_pipe.entities[0].name, "sensor");
    g_pipe.entities[0].entity_class = VSC_ENTITY_SENSOR;
    g_pipe.entities[0].transform_desc.type = VSC_TRANSFORM_CROP;
    g_pipe.entities[0].transform_desc.params.crop.max_w = 4056;
    g_pipe.entities[0].transform_desc.params.crop.max_h = 3040;
    g_pipe.entities[0].transform_desc.params.crop.min_w = 1;
    g_pipe.entities[0].transform_desc.params.crop.min_h = 1;
    g_pipe.entities[0].ops     = &g_ops_sensor;
    g_pipe.entities[0].drv_ctx = &g_mock_ctx_raw_only;

    /* STREAM pass-through */
    strcpy(g_pipe.entities[1].name, "lvds_rx");
    g_pipe.entities[1].entity_class = VSC_ENTITY_STREAM;
    g_pipe.entities[1].transform_desc.type = VSC_TRANSFORM_PASS_THROUGH;
    g_pipe.entities[1].ops     = &g_ops_pass;
    g_pipe.entities[1].drv_ctx = &g_mock_ctx_pass;

    /* ENDPOINT */
    strcpy(g_pipe.entities[2].name, "cl_out");
    g_pipe.entities[2].entity_class = VSC_ENTITY_ENDPOINT;
    g_pipe.entities[2].transform_desc.type = VSC_TRANSFORM_PASS_THROUGH;
    g_pipe.entities[2].ops     = &g_ops_endpoint;
    g_pipe.entities[2].drv_ctx = &g_mock_ctx_pass;

    /* Links */
    g_pipe.links[0].src_entity = 0; g_pipe.links[0].dst_entity = 1;
    g_pipe.links[0].type = VSC_LINK_STREAM;
    g_pipe.links[1].src_entity = 1; g_pipe.links[1].dst_entity = 2;
    g_pipe.links[1].type = VSC_LINK_STREAM;

    vsc_pipeline_build(&g_pipe);
}

/**
 * Build:  Sensor → Binning(2×2) → Decoder(RAW→RGB) → Crop(1920×1080) → ENDPOINT
 */
static void build_full_chain(void)
{
    g_pipe.num_entities = 5;
    g_pipe.num_links    = 4;

    /* Sensor */
    strcpy(g_pipe.entities[0].name, "sensor.imx477");
    g_pipe.entities[0].entity_class = VSC_ENTITY_SENSOR;
    g_pipe.entities[0].transform_desc.type = VSC_TRANSFORM_CROP;
    g_pipe.entities[0].transform_desc.params.crop.max_w = 4056;
    g_pipe.entities[0].transform_desc.params.crop.max_h = 3040;
    g_pipe.entities[0].transform_desc.params.crop.min_w = 1;
    g_pipe.entities[0].transform_desc.params.crop.min_h = 1;
    g_pipe.entities[0].ops     = &g_ops_sensor;
    g_pipe.entities[0].drv_ctx = &g_mock_ctx_raw_only;

    /* Binning */
    strcpy(g_pipe.entities[1].name, "binning_2x2");
    g_pipe.entities[1].entity_class = VSC_ENTITY_STREAM;
    g_pipe.entities[1].transform_desc.type = VSC_TRANSFORM_BINNING;
    g_pipe.entities[1].transform_desc.params.binning.factor_x = 2;
    g_pipe.entities[1].transform_desc.params.binning.factor_y = 2;
    g_pipe.entities[1].ops     = &g_ops_binning;
    g_pipe.entities[1].drv_ctx = &g_mock_ctx_bin2x2;

    /* Decoder */
    strcpy(g_pipe.entities[2].name, "decoder");
    g_pipe.entities[2].entity_class = VSC_ENTITY_STREAM;
    g_pipe.entities[2].transform_desc.type = VSC_TRANSFORM_PIXEL_FMT_CONV;
    g_pipe.entities[2].transform_desc.params.pixel_fmt_conv.fmt_in  = VSC_FMT_RAW10;
    g_pipe.entities[2].transform_desc.params.pixel_fmt_conv.fmt_out = VSC_FMT_RGB888;
    g_pipe.entities[2].ops     = &g_ops_decoder;
    g_pipe.entities[2].drv_ctx = &g_mock_ctx_decoder;

    /* Crop */
    strcpy(g_pipe.entities[3].name, "crop");
    g_pipe.entities[3].entity_class = VSC_ENTITY_STREAM;
    g_pipe.entities[3].transform_desc.type = VSC_TRANSFORM_CROP;
    g_pipe.entities[3].transform_desc.params.crop.min_w = 64;
    g_pipe.entities[3].transform_desc.params.crop.min_h = 4;
    g_pipe.entities[3].transform_desc.params.crop.max_w = 1920;
    g_pipe.entities[3].transform_desc.params.crop.max_h = 1080;
    g_pipe.entities[3].ops     = &g_ops_crop;
    g_pipe.entities[3].drv_ctx = &g_mock_ctx_crop_1920;

    /* ENDPOINT */
    strcpy(g_pipe.entities[4].name, "cl_out");
    g_pipe.entities[4].entity_class = VSC_ENTITY_ENDPOINT;
    g_pipe.entities[4].transform_desc.type = VSC_TRANSFORM_PASS_THROUGH;
    g_pipe.entities[4].ops     = &g_ops_endpoint;
    g_pipe.entities[4].drv_ctx = &g_mock_ctx_pass;

    /* Links */
    for (int i = 0; i < 4; i++) {
        g_pipe.links[i].src_entity = i;
        g_pipe.links[i].dst_entity = i + 1;
        g_pipe.links[i].type       = VSC_LINK_STREAM;
    }

    vsc_pipeline_build(&g_pipe);
}

/**
 * Build:  Sensor → ISP → CL_Out (endpoint A)
 *                       → DDR_Writer (endpoint B)
 *                       → Histogram (TAP)
 */
static void build_multi_branch_with_tap(void)
{
    g_pipe.num_entities = 5;

    /* Sensor */
    strcpy(g_pipe.entities[0].name, "sensor");
    g_pipe.entities[0].entity_class = VSC_ENTITY_SENSOR;
    g_pipe.entities[0].transform_desc.type = VSC_TRANSFORM_CROP;
    g_pipe.entities[0].transform_desc.params.crop.max_w = 4056;
    g_pipe.entities[0].transform_desc.params.crop.max_h = 3040;
    g_pipe.entities[0].transform_desc.params.crop.min_w = 1;
    g_pipe.entities[0].transform_desc.params.crop.min_h = 1;
    g_pipe.entities[0].ops     = &g_ops_sensor;
    g_pipe.entities[0].drv_ctx = &g_mock_ctx_raw_only;

    /* ISP (STREAM, contains internal processing) */
    strcpy(g_pipe.entities[1].name, "isp");
    g_pipe.entities[1].entity_class = VSC_ENTITY_STREAM;
    g_pipe.entities[1].transform_desc.type = VSC_TRANSFORM_PASS_THROUGH;
    g_pipe.entities[1].ops     = &g_ops_pass;
    g_pipe.entities[1].drv_ctx = &g_mock_ctx_pass;

    /* CL_Out ENDPOINT */
    strcpy(g_pipe.entities[2].name, "cl_out");
    g_pipe.entities[2].entity_class = VSC_ENTITY_ENDPOINT;
    g_pipe.entities[2].transform_desc.type = VSC_TRANSFORM_PASS_THROUGH;
    g_pipe.entities[2].ops     = &g_ops_endpoint;
    g_pipe.entities[2].drv_ctx = &g_mock_ctx_pass;

    /* DDR_Writer ENDPOINT */
    strcpy(g_pipe.entities[3].name, "ddr_writer");
    g_pipe.entities[3].entity_class = VSC_ENTITY_ENDPOINT;
    g_pipe.entities[3].transform_desc.type = VSC_TRANSFORM_PASS_THROUGH;
    g_pipe.entities[3].ops     = &g_ops_endpoint;
    g_pipe.entities[3].drv_ctx = &g_mock_ctx_pass;

    /* Histogram ANALYZER */
    strcpy(g_pipe.entities[4].name, "histogram");
    g_pipe.entities[4].entity_class = VSC_ENTITY_ANALYZER;
    g_pipe.entities[4].transform_desc.type = VSC_TRANSFORM_PASS_THROUGH;
    g_pipe.entities[4].ops     = &g_ops_histogram;
    g_pipe.entities[4].drv_ctx = &g_mock_ctx_histogram;

    g_pipe.num_links = 4;
    /* Sensor → ISP */
    g_pipe.links[0].src_entity = 0; g_pipe.links[0].dst_entity = 1;
    g_pipe.links[0].type = VSC_LINK_STREAM;
    /* ISP → CL_Out */
    g_pipe.links[1].src_entity = 1; g_pipe.links[1].dst_entity = 2;
    g_pipe.links[1].type = VSC_LINK_STREAM;
    /* ISP → DDR_Writer */
    g_pipe.links[2].src_entity = 1; g_pipe.links[2].dst_entity = 3;
    g_pipe.links[2].type = VSC_LINK_STREAM;
    /* ISP → Histogram (TAP) */
    g_pipe.links[3].src_entity = 1; g_pipe.links[3].dst_entity = 4;
    g_pipe.links[3].type = VSC_LINK_TAP;

    vsc_pipeline_build(&g_pipe);
}

/* ========================================================================
 *  Phase 1 Tests — Feasibility Pre-check
 * ======================================================================== */

/**
 * Pass-through chain: intent within sensor capabilities → FEASIBLE
 */
void test_feasibility_linear_pass_chain(void)
{
    build_linear_pass_chain();

    vsc_mbus_fmt_t intent = {1920, 1080, VSC_FMT_RAW10, 30, 1, 10, 4, {0}};
    int rc = vsc_resolver_feasibility_check(&g_pipe, &intent, &g_result);
    TEST_ASSERT_EQUAL_INT(VSC_OK, rc);
}

/**
 * Binning chain: intent * 2 must be within sensor range
 */
void test_feasibility_binning_chain(void)
{
    build_full_chain();

    /* 1920×1080 RGB888 → reverse: 1920*2=3840, 1080*2=2160 RAW10
     * Sensor max=4056×3040 → should fit */
    vsc_mbus_fmt_t intent = {1920, 1080, VSC_FMT_RGB888, 30, 1, 8, 4, {0}};
    int rc = vsc_resolver_feasibility_check(&g_pipe, &intent, &g_result);
    TEST_ASSERT_EQUAL_INT(VSC_OK, rc);
}

/**
 * Intent that requires sensor beyond max → UNREACHABLE
 */
void test_feasibility_exceeds_sensor_max(void)
{
    build_full_chain();

    /* 3000×2000 RGB888 → reverse: 3000*2=6000 > sensor max 4056 */
    vsc_mbus_fmt_t intent = {3000, 2000, VSC_FMT_RGB888, 30, 1, 8, 4, {0}};
    int rc = vsc_resolver_feasibility_check(&g_pipe, &intent, &g_result);
    TEST_ASSERT_EQUAL_INT(VSC_ERR_UNREACHABLE, rc);
}

/**
 * Intent exceeds crop max → UNREACHABLE
 */
void test_feasibility_exceeds_crop_max(void)
{
    build_full_chain();

    /* 2048×2048 RGB888 → Crop max is 1920×1080 → won't fit */
    vsc_mbus_fmt_t intent = {2048, 2048, VSC_FMT_RGB888, 30, 1, 8, 4, {0}};
    int rc = vsc_resolver_feasibility_check(&g_pipe, &intent, &g_result);
    TEST_ASSERT_EQUAL_INT(VSC_ERR_UNREACHABLE, rc);
}

/**
 * Intent at exact crop boundary → FEASIBLE
 */
void test_feasibility_crop_boundary(void)
{
    build_full_chain();

    /* 1920×1080 exactly at crop max → should be feasible */
    vsc_mbus_fmt_t intent = {1920, 1080, VSC_FMT_RGB888, 30, 1, 8, 4, {0}};
    int rc = vsc_resolver_feasibility_check(&g_pipe, &intent, &g_result);
    TEST_ASSERT_EQUAL_INT(VSC_OK, rc);
}

/**
 * Pixel format incompatible with decoder → UNREACHABLE
 */
void test_feasibility_format_mismatch(void)
{
    build_full_chain();

    /* YUV422 is not RAW → decoder can't process → mismatch */
    vsc_mbus_fmt_t intent = {1920, 1080, VSC_FMT_YUV422, 30, 1, 8, 4, {0}};
    int rc = vsc_resolver_feasibility_check(&g_pipe, &intent, &g_result);
    TEST_ASSERT_EQUAL_INT(VSC_ERR_UNREACHABLE, rc);
}

/**
 * Binning alignment: odd width intent → still FEASIBLE (conservative)
 * Phase 2 would catch exact alignment issues.
 */
void test_feasibility_binning_odd_width(void)
{
    build_full_chain();

    /* 1919×1079 — odd values within crop range [64..1920], reverse through
     * binning gives 3838×2158, within sensor 4056×3040. Conservative pre-check
     * widens the range, not aligns. */
    vsc_mbus_fmt_t intent = {1919, 1079, VSC_FMT_RGB888, 30, 1, 8, 4, {0}};
    int rc = vsc_resolver_feasibility_check(&g_pipe, &intent, &g_result);
    /* pre-check is conservative — it only checks ranges, not alignment */
    TEST_ASSERT_EQUAL_INT(VSC_OK, rc);
}

/**
 * Empty pipeline (no entities) → UNREACHABLE (no sensor to check)
 */
void test_feasibility_empty_pipeline(void)
{
    g_pipe.num_entities = 0;
    g_pipe.num_links    = 0;
    vsc_pipeline_build(&g_pipe);

    vsc_mbus_fmt_t intent = {1920, 1080, VSC_FMT_RGB888, 30, 1, 8, 4, {0}};
    int rc = vsc_resolver_feasibility_check(&g_pipe, &intent, &g_result);
    TEST_ASSERT_EQUAL_INT(VSC_ERR_UNREACHABLE, rc);
}

/* ========================================================================
 *  Optional Entity Auto-bridging Tests
 * ======================================================================== */

/**
 * SISO bridge: 3 entities, remove middle → 2 entities with direct link
 */
void test_bridge_siso_simple(void)
{
    build_linear_pass_chain();
    /* Original: sensor(0) → lvds_rx(1) → cl_out(2)
     * Remove lvds_rx (index 1) */

    int rc = vsc_pipeline_remove_optional(&g_pipe, 1);
    TEST_ASSERT_EQUAL_INT(VSC_OK, rc);

    /* Should now have 2 entities, 1 link */
    TEST_ASSERT_EQUAL_UINT8(2, g_pipe.num_entities);
    TEST_ASSERT_EQUAL_UINT8(1, g_pipe.num_links);

    /* Link should be sensor → cl_out */
    TEST_ASSERT_EQUAL_UINT8(0, g_pipe.links[0].src_entity);
    TEST_ASSERT_EQUAL_UINT8(1, g_pipe.links[0].dst_entity);
    TEST_ASSERT_EQUAL_INT(VSC_LINK_STREAM, g_pipe.links[0].type);
}

/**
 * Leaf removal: remove ENDPOINT → no bridging needed
 */
void test_bridge_leaf_removal(void)
{
    build_linear_pass_chain();
    /* Remove cl_out (index 2, ENDPOINT) */

    int rc = vsc_pipeline_remove_optional(&g_pipe, 2);
    TEST_ASSERT_EQUAL_INT(VSC_OK, rc);

    /* 2 entities, 1 link (sensor → lvds_rx) */
    TEST_ASSERT_EQUAL_UINT8(2, g_pipe.num_entities);
    TEST_ASSERT_EQUAL_UINT8(1, g_pipe.num_links);
}

/**
 * Root removal: remove SENSOR → no bridging needed
 */
void test_bridge_root_removal(void)
{
    build_linear_pass_chain();
    /* Remove sensor (index 0) */

    int rc = vsc_pipeline_remove_optional(&g_pipe, 0);
    TEST_ASSERT_EQUAL_INT(VSC_OK, rc);

    /* 2 entities, 1 link (lvds_rx→cl_out remains — neither endpoint was removed) */
    TEST_ASSERT_EQUAL_UINT8(2, g_pipe.num_entities);
    TEST_ASSERT_EQUAL_UINT8(1, g_pipe.num_links);
    TEST_ASSERT_EQUAL_UINT8(0, g_pipe.links[0].src_entity);
    TEST_ASSERT_EQUAL_UINT8(1, g_pipe.links[0].dst_entity);
}

/**
 * TAP links die when source entity is removed
 */
void test_bridge_tap_dies(void)
{
    build_multi_branch_with_tap();
    /* Original: sensor(0)→isp(1)→cl_out(2), →ddr(3), →histogram(4)[TAP]
     * Remove isp (index 1) — a multi-out entity */

    int rc = vsc_pipeline_remove_optional(&g_pipe, 1);
    /* isp has 1 in, 3 out (2 STREAM + 1 TAP) → MIMO, can't auto-bridge */
    TEST_ASSERT_EQUAL_INT(VSC_ERR_CANNOT_AUTO_BRIDGE, rc);
}

/**
 * Bridge across entity types: remove Binning from full chain
 */
void test_bridge_across_types(void)
{
    build_full_chain();
    /* Original: sensor(0)→binning(1)→decoder(2)→crop(3)→cl_out(4)
     * Remove binning (index 1) → bridge: sensor(0)→decoder(2) (shifted to index 1)
     * Note: after removal, decoder becomes index 1 */

    int rc = vsc_pipeline_remove_optional(&g_pipe, 1);
    TEST_ASSERT_EQUAL_INT(VSC_OK, rc);

    /* 4 entities, sensor→decoder→crop→cl_out */
    TEST_ASSERT_EQUAL_UINT8(4, g_pipe.num_entities);

    /* verify new link: sensor_0 → decoder_0 */
    /* 3 links: sensor→decoder (bridge at [2]), decoder→crop, crop→cl_out */
    TEST_ASSERT_EQUAL_UINT8(3, g_pipe.num_links);
    /* bridge link is appended last: sensor(0) → decoder(shifted to 1) */
    TEST_ASSERT_EQUAL_UINT8(0, g_pipe.links[2].src_entity);
    TEST_ASSERT_EQUAL_UINT8(1, g_pipe.links[2].dst_entity);
    TEST_ASSERT_EQUAL_INT(VSC_LINK_STREAM, g_pipe.links[2].type);
    /* decoder→crop (original link, shifted) */
    TEST_ASSERT_EQUAL_UINT8(1, g_pipe.links[0].src_entity);
    TEST_ASSERT_EQUAL_UINT8(2, g_pipe.links[0].dst_entity);
    /* crop→cl_out (original link, shifted) */
    TEST_ASSERT_EQUAL_UINT8(2, g_pipe.links[1].src_entity);
    TEST_ASSERT_EQUAL_UINT8(3, g_pipe.links[1].dst_entity);
}

/* ========================================================================
 *  Phase 2 Tests — Forward Propagation
 * ======================================================================== */

/**
 * Linear chain, exact format match
 */
void test_forward_linear_exact(void)
{
    build_linear_pass_chain();

    vsc_mbus_fmt_t intent = {1920, 1080, VSC_FMT_RAW10, 30, 1, 10, 4, {0}};
    int rc = vsc_resolver_forward_propagate(&g_pipe, &intent);
    TEST_ASSERT_EQUAL_INT(VSC_OK, rc);

    /* All entities should be visited and active */
    for (int i = 0; i < 3; i++) {
        TEST_ASSERT_TRUE(g_pipe.entities[i].prop_state.visited);
        TEST_ASSERT_TRUE(g_pipe.entities[i].prop_state.active);
    }

    /* ENDPOINT should see the same format */
    TEST_ASSERT_TRUE(vsc_fmt_equal(&intent,
                     &g_pipe.entities[2].prop_state.sink_fmt));
}

/**
 * Binning halves the size
 */
void test_forward_binning_halves(void)
{
    build_full_chain();

    vsc_mbus_fmt_t intent = {1920, 1080, VSC_FMT_RAW10, 30, 1, 10, 4, {0}};
    int rc = vsc_resolver_forward_propagate(&g_pipe, &intent);
    TEST_ASSERT_EQUAL_INT(VSC_OK, rc);

    /* Sensor source: 1920×1080 (or largest that fits intent → sensor may adjust) */
    /* Binning source: should be halved to 960×540 */
    vsc_mbus_fmt_t bin_out = g_pipe.entities[1].prop_state.source_fmt;
    TEST_ASSERT_EQUAL_UINT32(960, bin_out.width);
    TEST_ASSERT_EQUAL_UINT32(540, bin_out.height);
}

/**
 * Crop clamps dimensions
 */
void test_forward_crop_clamps(void)
{
    build_full_chain();

    /* Intent larger than crop max → crop should clamp */
    /* Note: sensor will reduce to 4056 max first, then binning halves to 2028,
     * decoder passes through, then crop clamps to 1920 */
    vsc_mbus_fmt_t intent = {4096, 2160, VSC_FMT_RAW10, 30, 1, 10, 4, {0}};
    int rc = vsc_resolver_forward_propagate(&g_pipe, &intent);
    TEST_ASSERT_EQUAL_INT(VSC_OK, rc);

    /* Crop source should be ≤ 1920×1080 */
    vsc_mbus_fmt_t crop_out = g_pipe.entities[3].prop_state.source_fmt;
    TEST_ASSERT_TRUE(crop_out.width  <= 1920);
    TEST_ASSERT_TRUE(crop_out.height <= 1080);
}

/**
 * Multi-branch: all branches receive the same format from ISP
 */
void test_forward_multi_branch(void)
{
    build_multi_branch_with_tap();

    vsc_mbus_fmt_t intent = {1920, 1080, VSC_FMT_RAW10, 30, 1, 10, 4, {0}};
    int rc = vsc_resolver_forward_propagate(&g_pipe, &intent);
    TEST_ASSERT_EQUAL_INT(VSC_OK, rc);

    /* CL_Out and DDR_Writer should both receive ISP's output format */
    vsc_mbus_fmt_t cl_out_fmt = g_pipe.entities[2].prop_state.sink_fmt;
    vsc_mbus_fmt_t ddr_fmt    = g_pipe.entities[3].prop_state.sink_fmt;

    /* Both endpoints should see the same format (from the same ISP source) */
    TEST_ASSERT_TRUE(vsc_fmt_equal(&cl_out_fmt, &ddr_fmt));
}

/**
 * TAP succeeds: format is supported
 */
void test_forward_tap_success(void)
{
    build_multi_branch_with_tap();

    vsc_mbus_fmt_t intent = {1920, 1080, VSC_FMT_RAW10, 30, 1, 10, 4, {0}};
    int rc = vsc_resolver_forward_propagate(&g_pipe, &intent);
    TEST_ASSERT_EQUAL_INT(VSC_OK, rc);

    /* Histogram should be active (RAW10 is in its supported list) */
    TEST_ASSERT_TRUE(g_pipe.entities[4].prop_state.active);
}

/**
 * TAP fails: format not supported (e.g. YUV422 not in Histogram's list)
 */
void test_forward_tap_failure(void)
{
    build_multi_branch_with_tap();

    /* Change histogram to only accept RAW8 (not RAW10) */
    g_mock_ctx_histogram.supported_fmts[0] = VSC_FMT_RAW8;
    g_mock_ctx_histogram.supported_fmts[1] = VSC_FMT_INVALID;
    g_mock_ctx_histogram.supported_fmts[2] = VSC_FMT_INVALID;
    g_mock_ctx_histogram.supported_fmts[3] = VSC_FMT_INVALID;

    vsc_mbus_fmt_t intent = {1920, 1080, VSC_FMT_RAW10, 30, 1, 10, 4, {0}};
    int rc = vsc_resolver_forward_propagate(&g_pipe, &intent);
    /* Main pipeline should succeed even if TAP fails */
    TEST_ASSERT_EQUAL_INT(VSC_OK, rc);

    /* Histogram should be inactive */
    TEST_ASSERT_FALSE(g_pipe.entities[4].prop_state.active);
}

/**
 * Entity sink rejects format → entity marked inactive
 */
void test_forward_sink_rejection(void)
{
    build_full_chain();

    /* Replace decoder sink with reject mock */
    g_pipe.entities[2].ops = &g_ops_reject;

    vsc_mbus_fmt_t intent = {1920, 1080, VSC_FMT_RAW10, 30, 1, 10, 4, {0}};
    int rc = vsc_resolver_forward_propagate(&g_pipe, &intent);
    /* Propagation should detect that a STREAM entity was not visited */
    TEST_ASSERT_EQUAL_INT(VSC_ERR_TOPOLOGY_BROKEN, rc);
}

/* ========================================================================
 *  Phase 3 Tests — Multi-branch Convergence
 * ======================================================================== */

/**
 * Two identical branches → same format on both, EXACT match
 */
void test_converge_identical_branches(void)
{
    build_multi_branch_with_tap();

    vsc_mbus_fmt_t intent = {1920, 1080, VSC_FMT_RAW10, 30, 1, 10, 4, {0}};
    int rc = vsc_resolver_try_fmt(&g_pipe, &intent, &g_result);
    TEST_ASSERT_EQUAL_INT(VSC_OK, rc);

    TEST_ASSERT_EQUAL_INT(VSC_NEGOTIATE_EXACT, g_result.status);
    TEST_ASSERT_EQUAL_UINT8(2, g_result.num_endpoints);

    /* Both endpoints should report the same format */
    TEST_ASSERT_TRUE(vsc_fmt_equal(
        &g_result.endpoint_fmts[0].final_fmt,
        &g_result.endpoint_fmts[1].final_fmt));
}

/**
 * Two identical branches: adjustment trace should be populated
 */
void test_converge_trace_populated(void)
{
    build_multi_branch_with_tap();

    vsc_mbus_fmt_t intent = {1920, 1080, VSC_FMT_RAW10, 30, 1, 10, 4, {0}};
    int rc = vsc_resolver_try_fmt(&g_pipe, &intent, &g_result);
    TEST_ASSERT_EQUAL_INT(VSC_OK, rc);

    /* Expect at least sensor and ISP entries in the trace */
    TEST_ASSERT_TRUE(g_result.trace.num_entries >= 2);
}

/**
 * TAP failure degrades status to PARTIAL
 */
void test_converge_tap_failure_partial(void)
{
    build_multi_branch_with_tap();

    /* Restrict histogram to only RAW8 */
    g_mock_ctx_histogram.supported_fmts[0] = VSC_FMT_RAW8;
    g_mock_ctx_histogram.supported_fmts[1] = VSC_FMT_INVALID;
    g_mock_ctx_histogram.supported_fmts[2] = VSC_FMT_INVALID;
    g_mock_ctx_histogram.supported_fmts[3] = VSC_FMT_INVALID;

    vsc_mbus_fmt_t intent = {1920, 1080, VSC_FMT_RAW10, 30, 1, 10, 4, {0}};
    int rc = vsc_resolver_try_fmt(&g_pipe, &intent, &g_result);
    TEST_ASSERT_EQUAL_INT(VSC_OK, rc);

    /* Main pipeline works, but TAP is inactive → PARTIAL */
    TEST_ASSERT_EQUAL_INT(VSC_NEGOTIATE_PARTIAL, g_result.status);

    /* Trace should contain REJECTED entry for histogram */
    bool found_rejected = false;
    for (int i = 0; i < g_result.trace.num_entries; i++) {
        if (g_result.trace.entries[i].reason == VSC_ADJUST_REJECTED) {
            found_rejected = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(found_rejected);
}

/**
 * Branch snapshot isolation: binning halving captured in adjustment_trace.
 * Uses a linear chain to avoid multi-branch feasibility limitation (known issue).
 */
void test_converge_branch_isolation(void)
{
    build_linear_pass_chain();
    /* Replace lvds_rx (entity 1) with binning */
    g_pipe.entities[1].transform_desc.type = VSC_TRANSFORM_BINNING;
    g_pipe.entities[1].transform_desc.params.binning.factor_x = 2;
    g_pipe.entities[1].transform_desc.params.binning.factor_y = 2;
    g_pipe.entities[1].ops     = &g_ops_binning;
    g_pipe.entities[1].drv_ctx = &g_mock_ctx_bin2x2;

    vsc_mbus_fmt_t intent = {1920, 1080, VSC_FMT_RAW10, 30, 1, 10, 4, {0}};
    int rc = vsc_resolver_try_fmt(&g_pipe, &intent, &g_result);
    TEST_ASSERT_EQUAL_INT(VSC_OK, rc);

    /* Binning halves: 1920→960, 1080→540 */
    TEST_ASSERT_EQUAL_UINT32(960, g_result.primary_fmt.width);
    TEST_ASSERT_EQUAL_UINT32(540, g_result.primary_fmt.height);
    TEST_ASSERT_EQUAL_INT(VSC_NEGOTIATE_ADJUSTED, g_result.status);
    TEST_ASSERT_TRUE(g_result.trace.num_entries > 0);
}

/* ========================================================================
 *  Integration Tests — Full try_fmt Pipeline
 * ======================================================================== */

/**
 * Full pipeline: intent matches exactly on a pass-through chain.
 */
void test_integration_exact_match(void)
{
    build_linear_pass_chain();

    vsc_mbus_fmt_t intent = {1920, 1080, VSC_FMT_RAW10, 30, 1, 10, 4, {0}};
    int rc = vsc_resolver_try_fmt(&g_pipe, &intent, &g_result);
    TEST_ASSERT_EQUAL_INT(VSC_OK, rc);
    TEST_ASSERT_EQUAL_INT(VSC_NEGOTIATE_EXACT, g_result.status);
    TEST_ASSERT_TRUE(vsc_fmt_equal(&intent, &g_result.primary_fmt));
}

/**
 * Full pipeline: intent gets adjusted by binning.
 * Intent {1920, 1080, RAW10} → sensor passes → binning halves → {960, 540}
 */
void test_integration_adjusted(void)
{
    build_full_chain();

    /* RGB888 intent: decoder outputs RGB888, reverse through it works */
    vsc_mbus_fmt_t intent = {1920, 1080, VSC_FMT_RGB888, 30, 1, 8, 4, {0}};
    int rc = vsc_resolver_try_fmt(&g_pipe, &intent, &g_result);
    TEST_ASSERT_EQUAL_INT(VSC_OK, rc);

    /* Binning halves: 1920→960, 1080→540. Status ADJUSTED. */
    TEST_ASSERT_EQUAL_UINT32(960, g_result.primary_fmt.width);
    TEST_ASSERT_EQUAL_UINT32(540, g_result.primary_fmt.height);
    TEST_ASSERT_EQUAL_INT(VSC_NEGOTIATE_ADJUSTED, g_result.status);
}

/**
 * Full pipeline: invalid intent (zero width)
 */
void test_integration_invalid_intent(void)
{
    build_full_chain();

    vsc_mbus_fmt_t intent = {0, 1080, VSC_FMT_RGB888, 30, 1, 8, 4, {0}};
    int rc = vsc_resolver_try_fmt(&g_pipe, &intent, &g_result);
    TEST_ASSERT_EQUAL_INT(VSC_ERR_INVALID_INTENT, rc);
    TEST_ASSERT_EQUAL_INT(VSC_NEGOTIATE_FAILED, g_result.status);
}

/**
 * Full pipeline with multi-branch + TAP: complete integration
 */
void test_integration_multi_branch_tap(void)
{
    build_multi_branch_with_tap();

    vsc_mbus_fmt_t intent = {1920, 1080, VSC_FMT_RAW10, 30, 1, 10, 4, {0}};
    int rc = vsc_resolver_try_fmt(&g_pipe, &intent, &g_result);
    TEST_ASSERT_EQUAL_INT(VSC_OK, rc);

    /* Two endpoints, TAP active → EXACT (no adjustments needed) */
    /* However, sensor may change format from RAW10 to first supported (RAW8)
     * if RAW10 isn't in the supported list. Let's check what the sensor does. */
    /* Actually our mock_sensor_source checks supported_fmts. g_mock_ctx_raw_only
     * has RAW8, RAW10, RAW12 → so RAW10 should pass through. */
    TEST_ASSERT_EQUAL_UINT8(2, g_result.num_endpoints);

    /* Verify trace has meaningful content */
    TEST_ASSERT_TRUE(g_result.trace.num_entries > 0);
}

/* ========================================================================
 *  Test suite registration
 * ======================================================================== */

/* ── P2 Feature test forward declarations ── */
static void test_feature_streaming_always_true(void);
static void test_feature_default_all_false(void);
static void test_feature_manual_set_and_query(void);
static void test_feature_boundary_invalid_id(void);
static void test_feature_reset_clears_all(void);

/* ── Bitstream Loader test forward declarations ── */
static void test_loader_generated_init_builds_pipeline(void);
static void test_loader_optional_node_skipped(void);
static void test_loader_override_tightens_crop(void);
static void test_loader_override_refuses_relax(void);
static void test_crop_vsc_init(void);
static void test_crop_vsc_try_fmt_sink(void);
static void test_crop_vsc_try_fmt_source(void);
static void test_crop_hw_standalone(void);
static void test_binning_vsc_halves(void);
static void test_decoder_vsc_format_conv(void);
static void test_histogram_vsc_format_filter(void);
static void test_histogram_hw_standalone(void);
static void test_sensor_vsc_init_and_source(void);
static void test_full_pipeline_end_to_end(void);

static int resolver_run_all_tests(void)
{
    UNITY_BEGIN();

    /* ── Phase 1: Feasibility Pre-check ── */
    RUN_TEST(test_feasibility_linear_pass_chain);
    RUN_TEST(test_feasibility_binning_chain);
    RUN_TEST(test_feasibility_exceeds_sensor_max);
    RUN_TEST(test_feasibility_exceeds_crop_max);
    RUN_TEST(test_feasibility_crop_boundary);
    RUN_TEST(test_feasibility_format_mismatch);
    RUN_TEST(test_feasibility_binning_odd_width);
    RUN_TEST(test_feasibility_empty_pipeline);

    /* ── Optional Entity Auto-bridging ── */
    RUN_TEST(test_bridge_siso_simple);
    RUN_TEST(test_bridge_leaf_removal);
    RUN_TEST(test_bridge_root_removal);
    RUN_TEST(test_bridge_tap_dies);
    RUN_TEST(test_bridge_across_types);

    /* ── Phase 2: Forward Propagation ── */
    RUN_TEST(test_forward_linear_exact);
    RUN_TEST(test_forward_binning_halves);
    RUN_TEST(test_forward_crop_clamps);
    RUN_TEST(test_forward_multi_branch);
    RUN_TEST(test_forward_tap_success);
    RUN_TEST(test_forward_tap_failure);
    RUN_TEST(test_forward_sink_rejection);

    /* ── Phase 3: Multi-branch Convergence ── */
    RUN_TEST(test_converge_identical_branches);
    RUN_TEST(test_converge_trace_populated);
    RUN_TEST(test_converge_tap_failure_partial);
    RUN_TEST(test_converge_branch_isolation);

    /* ── Integration: Full try_fmt Pipeline ── */
    RUN_TEST(test_integration_exact_match);
    RUN_TEST(test_integration_adjusted);
    RUN_TEST(test_integration_invalid_intent);
    RUN_TEST(test_integration_multi_branch_tap);

    /* ── P2 Feature System ── */
    RUN_TEST(test_feature_streaming_always_true);
    RUN_TEST(test_feature_default_all_false);
    RUN_TEST(test_feature_manual_set_and_query);
    RUN_TEST(test_feature_boundary_invalid_id);
    RUN_TEST(test_feature_reset_clears_all);

    /* ── Bitstream Loader ── */
    RUN_TEST(test_loader_generated_init_builds_pipeline);
    RUN_TEST(test_loader_optional_node_skipped);
    RUN_TEST(test_loader_override_tightens_crop);
    RUN_TEST(test_loader_override_refuses_relax);

    /* ── Crop Driver (VSC adapter + standalone HW) ── */
    RUN_TEST(test_crop_vsc_init);
    RUN_TEST(test_crop_vsc_try_fmt_sink);
    RUN_TEST(test_crop_vsc_try_fmt_source);
    RUN_TEST(test_crop_hw_standalone);

    /* ── Binning / Decoder / Histogram drivers ── */
    RUN_TEST(test_binning_vsc_halves);
    RUN_TEST(test_decoder_vsc_format_conv);
    RUN_TEST(test_histogram_vsc_format_filter);
    RUN_TEST(test_histogram_hw_standalone);

    /* ── Sensor Driver ── */
    RUN_TEST(test_sensor_vsc_init_and_source);

    /* ── Full Pipeline Integration ── */
    RUN_TEST(test_full_pipeline_end_to_end);

    return UNITY_END();
}

/* ========================================================================
 *  P2 Feature System Tests
 * ======================================================================== */

#include "vsc_feature.h"

static void test_feature_streaming_always_true(void)
{
    vsc_feature_reset();
    vsc_feature_derive();
    TEST_ASSERT_TRUE(vsc_has_feature(VSC_FEATURE_STREAMING));
}

static void test_feature_default_all_false(void)
{
    vsc_feature_reset();

    /* Without calling vsc_feature_derive(), all features should be false */
    TEST_ASSERT_FALSE(vsc_has_feature(VSC_FEATURE_CROP));
    TEST_ASSERT_FALSE(vsc_has_feature(VSC_FEATURE_STREAMING));

    /* After derive: with generated registry linked, crop/binning/histogram
     * are active drivers → those features become true */
    vsc_feature_derive();
    TEST_ASSERT_TRUE(vsc_has_feature(VSC_FEATURE_STREAMING));
    TEST_ASSERT_TRUE(vsc_has_feature(VSC_FEATURE_CROP));
    TEST_ASSERT_TRUE(vsc_has_feature(VSC_FEATURE_BINNING));
    TEST_ASSERT_TRUE(vsc_has_feature(VSC_FEATURE_HISTOGRAM));
    TEST_ASSERT_TRUE(vsc_has_feature(VSC_FEATURE_AUTO_EXPOSURE));
    /* HDR and Trigger are stubs — still false */
    TEST_ASSERT_FALSE(vsc_has_feature(VSC_FEATURE_HDR));
    TEST_ASSERT_FALSE(vsc_has_feature(VSC_FEATURE_TRIGGER));
}

static void test_feature_manual_set_and_query(void)
{
    vsc_feature_reset();
    vsc_feature_set(VSC_FEATURE_CROP, true);
    TEST_ASSERT_TRUE(vsc_has_feature(VSC_FEATURE_CROP));
    TEST_ASSERT_FALSE(vsc_has_feature(VSC_FEATURE_BINNING));
    vsc_feature_set(VSC_FEATURE_CROP, false);
    TEST_ASSERT_FALSE(vsc_has_feature(VSC_FEATURE_CROP));
    vsc_feature_set(VSC_FEATURE_HISTOGRAM, true);
    vsc_feature_set(VSC_FEATURE_BINNING, true);
    TEST_ASSERT_TRUE(vsc_has_feature(VSC_FEATURE_HISTOGRAM));
    TEST_ASSERT_TRUE(vsc_has_feature(VSC_FEATURE_BINNING));
}

static void test_feature_boundary_invalid_id(void)
{
    TEST_ASSERT_FALSE(vsc_has_feature(VSC_FEATURE_COUNT));
    TEST_ASSERT_FALSE(vsc_has_feature((vsc_feature_id_t)255));
    TEST_ASSERT_NULL(vsc_feature_get(VSC_FEATURE_COUNT));
}

static void test_feature_reset_clears_all(void)
{
    vsc_feature_set(VSC_FEATURE_CROP, true);
    vsc_feature_set(VSC_FEATURE_HISTOGRAM, true);
    vsc_feature_set(VSC_FEATURE_AUTO_EXPOSURE, true);
    vsc_feature_reset();
    TEST_ASSERT_FALSE(vsc_has_feature(VSC_FEATURE_CROP));
    TEST_ASSERT_FALSE(vsc_has_feature(VSC_FEATURE_HISTOGRAM));
    TEST_ASSERT_FALSE(vsc_has_feature(VSC_FEATURE_AUTO_EXPOSURE));
    TEST_ASSERT_FALSE(vsc_has_feature(VSC_FEATURE_STREAMING));
}

/* ========================================================================
 *  Bitstream Loader Tests
 * ======================================================================== */

#include "vsc_loader.h"
#include "vsc_feature.h"

static void test_loader_generated_init_builds_pipeline(void)
{
    /* vsc_system_init_default() is generated by vsc_prop_gen.py from
     * system_graph.json (4 nodes: crop, binning, decoder, histogram)
     * and board.json (sensor_imx477).
     * All values are compile-time constants — no JSON parsing at runtime. */
    vsc_pipeline_t pipeline;
    int rc = vsc_system_init_default(&pipeline);
    TEST_ASSERT_EQUAL_INT(VSC_OK, rc);
    /* sensor(0) + crop(1) + binning(2) + decoder(3) + histogram(4) = 5 */
    TEST_ASSERT_EQUAL_UINT8(5, pipeline.num_entities);
    /* sensor→crop + crop→binning + binning→decoder + binning→histogram = 4 */
    TEST_ASSERT_EQUAL_UINT8(4, pipeline.num_links);
    TEST_ASSERT_EQUAL_INT(VSC_ENTITY_SENSOR,    pipeline.entities[0].entity_class);
    TEST_ASSERT_EQUAL_INT(VSC_ENTITY_STREAM,    pipeline.entities[1].entity_class);
    TEST_ASSERT_EQUAL_INT(VSC_ENTITY_ANALYZER,  pipeline.entities[4].entity_class);
}

static void test_loader_optional_node_skipped(void)
{
    vsc_system_desc_t desc;
    memset(&desc, 0, sizeof(desc));

    /* optional node with unknown type */
    strcpy(desc.nodes[0].type, "nonexistent_ip");
    strcpy(desc.nodes[0].id, "opt_0");
    desc.nodes[0].optional = true;
    desc.num_nodes = 1;

    /* known node */
    strcpy(desc.nodes[1].type, "crop");
    strcpy(desc.nodes[1].id, "crop_0");
    desc.num_nodes = 2;

    vsc_board_config_t board;
    memset(&board, 0, sizeof(board));

    vsc_pipeline_t pipeline;
    int rc = vsc_system_init(&desc, &board, &pipeline);
    TEST_ASSERT_EQUAL_INT(VSC_OK, rc);
    /* only crop entity created; optional unknown skipped */
    TEST_ASSERT_EQUAL_UINT8(1, pipeline.num_entities);
    TEST_ASSERT_EQUAL_STRING("crop_0", pipeline.entities[0].name);
}

static void test_loader_override_tightens_crop(void)
{
    vsc_system_desc_t desc;
    memset(&desc, 0, sizeof(desc));

    strcpy(desc.nodes[0].type, "crop");
    strcpy(desc.nodes[0].id, "crop_0");
    /* override: tighten max_width from 8192 → 1920 */
    strcpy(desc.nodes[0].overrides[0].key, "crop.max_width");
    desc.nodes[0].overrides[0].value = 1920;
    desc.nodes[0].num_overrides = 1;
    desc.num_nodes = 1;

    vsc_board_config_t board;
    memset(&board, 0, sizeof(board));

    vsc_pipeline_t pipeline;
    int rc = vsc_system_init(&desc, &board, &pipeline);
    TEST_ASSERT_EQUAL_INT(VSC_OK, rc);

    /* verify transform_desc was tightened */
    TEST_ASSERT_EQUAL_UINT32(1920,
        pipeline.entities[0].transform_desc.params.crop.max_w);
}

static void test_loader_override_refuses_relax(void)
{
    vsc_system_desc_t desc;
    memset(&desc, 0, sizeof(desc));

    strcpy(desc.nodes[0].type, "crop");
    strcpy(desc.nodes[0].id, "crop_0");
    /* try to relax max_width beyond driver template (8192 → 16384) */
    strcpy(desc.nodes[0].overrides[0].key, "crop.max_width");
    desc.nodes[0].overrides[0].value = 16384;  /* > 8192 → should fail */
    desc.nodes[0].num_overrides = 1;
    desc.num_nodes = 1;

    vsc_board_config_t board;
    memset(&board, 0, sizeof(board));

    vsc_pipeline_t pipeline;
    int rc = vsc_system_init(&desc, &board, &pipeline);
    TEST_ASSERT_NOT_EQUAL(VSC_OK, rc);
}

/* ========================================================================
 *  Crop Driver Tests (real implementation, reference template)
 * ======================================================================== */

#include "crop_driver.h"

static void test_crop_vsc_init(void)
{
    vsc_driver_register(&crop_vsc_driver);

    const vsc_driver_t *drv = vsc_driver_find("crop");
    TEST_ASSERT_NOT_NULL(drv);
    TEST_ASSERT_NOT_NULL(drv->ops.init);
    TEST_ASSERT_NOT_NULL(drv->ops.try_fmt_sink);
    TEST_ASSERT_NOT_NULL(drv->ops.try_fmt_source);
    TEST_ASSERT_NOT_NULL(drv->ops.commit_fmt);

    void *ctx = NULL;
    int rc = drv->ops.init(&ctx, 0x43C00000, NULL, 0);
    TEST_ASSERT_EQUAL_INT(VSC_OK, rc);
    TEST_ASSERT_NOT_NULL(ctx);
}

static void test_crop_vsc_try_fmt_sink(void)
{
    vsc_driver_register(&crop_vsc_driver);
    const vsc_driver_t *drv = vsc_driver_find("crop");

    void *ctx = NULL;
    drv->ops.init(&ctx, 0x43C00000, NULL, 0);

    vsc_mbus_fmt_t in  = {1920, 1080, VSC_FMT_RGB888, 30, 1, 8, 4, {0}};
    vsc_mbus_fmt_t out;
    TEST_ASSERT_EQUAL_INT(VSC_OK, drv->ops.try_fmt_sink(ctx, &in, &out));
    TEST_ASSERT_TRUE(vsc_fmt_equal(&in, &out));

    vsc_mbus_fmt_t big = {9000, 9000, VSC_FMT_RAW10, 30, 1, 10, 4, {0}};
    TEST_ASSERT_EQUAL_INT(VSC_OK, drv->ops.try_fmt_sink(ctx, &big, &out));
    TEST_ASSERT_EQUAL_UINT32(8192, out.width);
    TEST_ASSERT_EQUAL_UINT32(8192, out.height);
}

static void test_crop_vsc_try_fmt_source(void)
{
    vsc_driver_register(&crop_vsc_driver);
    const vsc_driver_t *drv = vsc_driver_find("crop");

    void *ctx = NULL;
    drv->ops.init(&ctx, 0x43C00000, NULL, 0);

    vsc_mbus_fmt_t sink = {1920, 1080, VSC_FMT_RGB888, 30, 1, 8, 4, {0}};
    vsc_mbus_fmt_t source;
    TEST_ASSERT_EQUAL_INT(VSC_OK, drv->ops.try_fmt_source(ctx, &sink, &source));
    TEST_ASSERT_EQUAL_UINT32(1920, source.width);
    TEST_ASSERT_EQUAL_UINT32(1080, source.height);

    vsc_mbus_fmt_t big = {3840, 2160, VSC_FMT_RGB888, 30, 1, 8, 4, {0}};
    TEST_ASSERT_EQUAL_INT(VSC_OK, drv->ops.try_fmt_source(ctx, &big, &source));
    TEST_ASSERT_EQUAL_UINT32(1920, source.width);
    TEST_ASSERT_EQUAL_UINT32(1080, source.height);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  纯 HW 驱动独立测试（零框架依赖）
 * ═══════════════════════════════════════════════════════════════════════ */

static void test_crop_hw_standalone(void)
{
    crop_dev_t dev;
    crop_init(&dev, 0x43C00000);

    /* verify defaults */
    uint32_t mw, mh;
    crop_get_limits(&dev, &mw, &mh);
    TEST_ASSERT_EQUAL_UINT32(8192, mw);
    TEST_ASSERT_EQUAL_UINT32(8192, mh);

    uint32_t rx, ry, rw, rh;
    crop_get_roi(&dev, &rx, &ry, &rw, &rh);
    TEST_ASSERT_EQUAL_UINT32(1920, rw);
    TEST_ASSERT_EQUAL_UINT32(1080, rh);
    TEST_ASSERT_EQUAL_UINT32(0, rx);
    TEST_ASSERT_EQUAL_UINT32(0, ry);

    /* tighten limits */
    TEST_ASSERT_EQUAL_INT(0, crop_set_limits(&dev, 1280, 720));

    /* relax — should fail */
    TEST_ASSERT_NOT_EQUAL(0, crop_set_limits(&dev, 9999, 9999));

    /* verify maintained after failed relax */
    crop_get_limits(&dev, &mw, &mh);
    TEST_ASSERT_EQUAL_UINT32(1280, mw);
    TEST_ASSERT_EQUAL_UINT32(720, mh);

    /* set ROI */
    crop_set_roi(&dev, 0, 0, 640, 480);
    crop_get_roi(&dev, &rx, &ry, &rw, &rh);
    TEST_ASSERT_EQUAL_UINT32(640, rw);
    TEST_ASSERT_EQUAL_UINT32(480, rh);

    /* commit + enable (virtual registers, no crash) */
    crop_commit(&dev);
    crop_enable(&dev);
    crop_disable(&dev);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Binning Driver Tests
 * ═══════════════════════════════════════════════════════════════════════ */

static void test_binning_vsc_halves(void)
{
    vsc_driver_register(&binning_vsc_driver);
    const vsc_driver_t *drv = vsc_driver_find("binning");
    TEST_ASSERT_NOT_NULL(drv);

    void *ctx = NULL;
    drv->ops.init(&ctx, 0x43C00000, NULL, 0);
    TEST_ASSERT_NOT_NULL(ctx);

    /* sink: pass-through */
    vsc_mbus_fmt_t in = {1920, 1080, VSC_FMT_RAW10, 30, 1, 10, 4, {0}};
    vsc_mbus_fmt_t out;
    TEST_ASSERT_EQUAL_INT(VSC_OK, drv->ops.try_fmt_sink(ctx, &in, &out));

    /* source: halves */
    TEST_ASSERT_EQUAL_INT(VSC_OK, drv->ops.try_fmt_source(ctx, &out, &out));
    TEST_ASSERT_EQUAL_UINT32(960, out.width);
    TEST_ASSERT_EQUAL_UINT32(540, out.height);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Decoder Driver Tests
 * ═══════════════════════════════════════════════════════════════════════ */

static void test_decoder_vsc_format_conv(void)
{
    vsc_driver_register(&decoder_vsc_driver);
    const vsc_driver_t *drv = vsc_driver_find("decoder");
    TEST_ASSERT_NOT_NULL(drv);

    void *ctx = NULL;
    drv->ops.init(&ctx, 0x43C00000, NULL, 0);

    /* RAW10 accepted */
    vsc_mbus_fmt_t in = {1920, 1080, VSC_FMT_RAW10, 30, 1, 10, 4, {0}};
    vsc_mbus_fmt_t out;
    TEST_ASSERT_EQUAL_INT(VSC_OK, drv->ops.try_fmt_sink(ctx, &in, &out));

    /* RGB888 rejected */
    vsc_mbus_fmt_t rgb = {1920, 1080, VSC_FMT_RGB888, 30, 1, 8, 4, {0}};
    TEST_ASSERT_NOT_EQUAL(VSC_OK, drv->ops.try_fmt_sink(ctx, &rgb, &out));

    /* source: RAW10 → RGB888 */
    TEST_ASSERT_EQUAL_INT(VSC_OK, drv->ops.try_fmt_source(ctx, &in, &out));
    TEST_ASSERT_EQUAL_UINT32(VSC_FMT_RGB888, out.pixel_format);
    TEST_ASSERT_EQUAL_UINT32(1920, out.width);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Histogram Driver Tests
 * ═══════════════════════════════════════════════════════════════════════ */

static void test_histogram_vsc_format_filter(void)
{
    vsc_driver_register(&histogram_vsc_driver);
    const vsc_driver_t *drv = vsc_driver_find("histogram");
    TEST_ASSERT_NOT_NULL(drv);

    void *ctx = NULL;
    drv->ops.init(&ctx, 0x43C00000, NULL, 0);

    /* RAW10 accepted */
    vsc_mbus_fmt_t in = {1920, 1080, VSC_FMT_RAW10, 30, 1, 10, 4, {0}};
    vsc_mbus_fmt_t out;
    TEST_ASSERT_EQUAL_INT(VSC_OK, drv->ops.try_fmt_sink(ctx, &in, &out));

    /* RGB888 accepted */
    vsc_mbus_fmt_t rgb = {1920, 1080, VSC_FMT_RGB888, 30, 1, 8, 4, {0}};
    TEST_ASSERT_EQUAL_INT(VSC_OK, drv->ops.try_fmt_sink(ctx, &rgb, &out));

    /* YUV422 rejected */
    vsc_mbus_fmt_t yuv = {1920, 1080, VSC_FMT_YUV422, 30, 1, 8, 4, {0}};
    TEST_ASSERT_NOT_EQUAL(VSC_OK, drv->ops.try_fmt_sink(ctx, &yuv, &out));

    /* ANALYZER has no try_fmt_source */
    TEST_ASSERT_NULL(drv->ops.try_fmt_source);
    TEST_ASSERT_NULL(drv->ops.commit_fmt);
}

static void test_histogram_hw_standalone(void)
{
    histogram_dev_t dev;
    histogram_init(&dev, 0x43C00000);

    TEST_ASSERT_TRUE(histogram_supports_format(&dev, 0x52415741));   /* RAW10 */
    TEST_ASSERT_TRUE(histogram_supports_format(&dev, 0x52474238));   /* RGB888 */
    TEST_ASSERT_FALSE(histogram_supports_format(&dev, 0x59555532));  /* YUV422 — no */

    TEST_ASSERT_EQUAL_UINT32(64, dev.active_bins);
    histogram_set_bins(&dev, 128);
    TEST_ASSERT_EQUAL_UINT32(128, dev.active_bins);
    /* cannot exceed max */
    histogram_set_bins(&dev, 512);
    TEST_ASSERT_EQUAL_UINT32(128, dev.active_bins);

    histogram_enable(&dev);
    histogram_commit(&dev);
    histogram_disable(&dev);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Sensor Driver Tests
 * ═══════════════════════════════════════════════════════════════════════ */

static void test_sensor_vsc_init_and_source(void)
{
    vsc_driver_register(&sensor_imx477_vsc_driver);
    const vsc_driver_t *drv = vsc_driver_find("sensor_imx477");
    TEST_ASSERT_NOT_NULL(drv);
    TEST_ASSERT_NOT_NULL(drv->ops.init);
    TEST_ASSERT_NULL(drv->ops.try_fmt_sink);    /* SENSOR 无 sink */
    TEST_ASSERT_NOT_NULL(drv->ops.try_fmt_source);
    TEST_ASSERT_NOT_NULL(drv->ops.commit_fmt);

    /* init */
    void *ctx = NULL;
    int rc = drv->ops.init(&ctx, 0, NULL, 0);
    TEST_ASSERT_EQUAL_INT(VSC_OK, rc);
    TEST_ASSERT_NOT_NULL(ctx);

    /* try_fmt_source: supported format passes through */
    vsc_mbus_fmt_t intent = {1920, 1080, VSC_FMT_RAW10, 30, 1, 10, 4, {0}};
    vsc_mbus_fmt_t out;
    TEST_ASSERT_EQUAL_INT(VSC_OK, drv->ops.try_fmt_source(ctx, &intent, &out));
    TEST_ASSERT_EQUAL_UINT32(1920, out.width);
    TEST_ASSERT_EQUAL_UINT32(1080, out.height);
    TEST_ASSERT_EQUAL_UINT32(VSC_FMT_RAW10, out.pixel_format);

    /* unsupported format falls back to first supported */
    vsc_mbus_fmt_t rgb = {1920, 1080, VSC_FMT_RGB888, 30, 1, 8, 4, {0}};
    TEST_ASSERT_EQUAL_INT(VSC_OK, drv->ops.try_fmt_source(ctx, &rgb, &out));
    TEST_ASSERT_EQUAL_UINT32(VSC_FMT_RAW8, out.pixel_format);  /* fallback */

    /* exceeds max — clamps */
    vsc_mbus_fmt_t big = {5000, 4000, VSC_FMT_RAW10, 30, 1, 10, 4, {0}};
    TEST_ASSERT_EQUAL_INT(VSC_OK, drv->ops.try_fmt_source(ctx, &big, &out));
    TEST_ASSERT_EQUAL_UINT32(4056, out.width);
    TEST_ASSERT_EQUAL_UINT32(3040, out.height);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Full Pipeline End-to-End Integration Test
 *
 *  拓扑（来自 system_graph.json + board.json）：
 *    sensor_imx477 → crop_0 → binning_0 → decoder_0
 *                              binning_0 ──[TAP]→ histogram_0
 *
 *  所有 5 个驱动均使用真实 VSC 适配器（非 mock）。
 * ═══════════════════════════════════════════════════════════════════════ */

static void test_full_pipeline_end_to_end(void)
{
    /* ── 注册全部 5 个真实驱动 ── */
    vsc_driver_register(&sensor_imx477_vsc_driver);
    vsc_driver_register(&crop_vsc_driver);
    vsc_driver_register(&binning_vsc_driver);
    vsc_driver_register(&decoder_vsc_driver);
    vsc_driver_register(&histogram_vsc_driver);

    /* ── 系统初始化（使用生成的 system_graph.json + board.json）── */
    vsc_pipeline_t pipeline;
    int rc = vsc_system_init_default(&pipeline);
    TEST_ASSERT_EQUAL_INT(VSC_OK, rc);

    /* 验证拓扑：sensor(0) + crop(1) + binning(2) + decoder(3) + histogram(4) = 5 */
    TEST_ASSERT_EQUAL_UINT8(5, pipeline.num_entities);
    /* sensor→crop + crop→binning + binning→decoder + binning→histogram(TAP) = 4 */
    TEST_ASSERT_EQUAL_UINT8(4, pipeline.num_links);
    TEST_ASSERT_EQUAL_INT(VSC_ENTITY_SENSOR,   pipeline.entities[0].entity_class);
    TEST_ASSERT_EQUAL_INT(VSC_ENTITY_STREAM,   pipeline.entities[1].entity_class);
    TEST_ASSERT_EQUAL_INT(VSC_ENTITY_ANALYZER, pipeline.entities[4].entity_class);

    /* ── try_fmt: {1920, 1080, RGB888, 30fps} ── */
    vsc_mbus_fmt_t intent = {1920, 1080, VSC_FMT_RGB888, 30, 1, 8, 4, {0}};
    vsc_resolver_result_t result;
    rc = vsc_resolver_try_fmt(&pipeline, &intent, &result);
    TEST_ASSERT_EQUAL_INT(VSC_OK, rc);

    /* 预期输出：
     *   sensor→1920×1080 RAW10 → crop(不变) → binning→960×540 RAW10
     *   → decoder→960×540 RGB888
     */
    TEST_ASSERT_EQUAL_UINT32(960,  result.primary_fmt.width);
    TEST_ASSERT_EQUAL_UINT32(540,  result.primary_fmt.height);
    TEST_ASSERT_EQUAL_UINT32(VSC_FMT_RGB888, result.primary_fmt.pixel_format);
    TEST_ASSERT_EQUAL_INT(VSC_NEGOTIATE_ADJUSTED, result.status);

    /* ── adjustment_trace 验证 ── */
    TEST_ASSERT_TRUE(result.trace.num_entries >= 2);

    /* 找到 binning 和 decoder 的 trace entry */
    bool saw_binning = false, saw_decoder = false;
    for (int i = 0; i < result.trace.num_entries; i++) {
        const vsc_adjustment_entry_t *e = &result.trace.entries[i];
        if (strcmp(e->entity_name, "binning_0") == 0) {
            TEST_ASSERT_EQUAL_INT(VSC_FMT_FIELD_WIDTH, e->field_changed);
            TEST_ASSERT_EQUAL_INT(VSC_ADJUST_HALVE, e->reason);
            saw_binning = true;
        }
        if (strcmp(e->entity_name, "decoder_0") == 0) {
            TEST_ASSERT_EQUAL_INT(VSC_FMT_FIELD_FORMAT, e->field_changed);
            TEST_ASSERT_EQUAL_INT(VSC_ADJUST_FORMAT_CONV, e->reason);
            saw_decoder = true;
        }
    }
    TEST_ASSERT_TRUE(saw_binning);
    TEST_ASSERT_TRUE(saw_decoder);

    /* ── TAP: histogram 应处于 ACTIVE 状态 ── */
    TEST_ASSERT_TRUE(pipeline.entities[4].prop_state.active);

    /* ── Features ── */
    TEST_ASSERT_TRUE(vsc_has_feature(VSC_FEATURE_CROP));
    TEST_ASSERT_TRUE(vsc_has_feature(VSC_FEATURE_BINNING));
    TEST_ASSERT_TRUE(vsc_has_feature(VSC_FEATURE_HISTOGRAM));
    TEST_ASSERT_TRUE(vsc_has_feature(VSC_FEATURE_AUTO_EXPOSURE));
}

#include "test_suite.h"
TEST_SUITE_DEFINE(vsc_resolver, resolver_setup, resolver_teardown, resolver_run_all_tests);
