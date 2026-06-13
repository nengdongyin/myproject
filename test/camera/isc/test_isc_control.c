/**
 * @file    test_isc_control.c
 * @brief   ISC 单元测试 — 控制框架 (query/get/set/ext_ctrls/stream)
 */

#include "test_isc_common.h"
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════════
 *  isc_query_ctrl / isc_query_next_ctrl
 * ═══════════════════════════════════════════════════════════════════════════ */

void test_query_ctrl_by_cid(void)
{
    isc_dev_t *dev = NULL;
    isc_open("mock_sensor", &dev);

    isc_ctrl_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.cid = ISC_CID_EXPOSURE;

    int rc = isc_query_ctrl(dev, &desc);
    TEST_ASSERT_ISC_OK(rc);
    TEST_ASSERT_EQUAL_UINT(ISC_CID_EXPOSURE, desc.cid);
    TEST_ASSERT_EQUAL_INT(ISC_CTRL_TYPE_INTEGER, desc.type);
    TEST_ASSERT_TRUE(desc.flags & ISC_CTRL_FLAG_STREAMABLE);

    isc_close(dev);
}

void test_query_ctrl_unsupported_returns_error(void)
{
    isc_dev_t *dev = NULL;
    isc_open("mock_sensor", &dev);

    isc_ctrl_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.cid = ISC_CID_DIGITAL_GAIN;  /* mock 不支持 */

    int rc = isc_query_ctrl(dev, &desc);
    TEST_ASSERT_ISC_ERR(rc, ISC_ERR_NOT_SUPPORTED);

    isc_close(dev);
}

void test_query_next_ctrl_enumerates_all(void)
{
    isc_dev_t *dev = NULL;
    isc_open("mock_sensor", &dev);

    int count = 0;
    isc_ctrl_desc_t desc;
    while (isc_query_next_ctrl(dev, &desc) == ISC_OK)
        count++;

    /* Mock sensor provides 8 controls */
    TEST_ASSERT_EQUAL_INT(8, count);

    isc_close(dev);
}

void test_query_next_ctrl_terminates(void)
{
    isc_dev_t *dev = NULL;
    isc_open("mock_sensor", &dev);

    /* 枚举到穷尽 */
    isc_ctrl_desc_t desc;
    int rc;
    while ((rc = isc_query_next_ctrl(dev, &desc)) == ISC_OK);
    TEST_ASSERT_EQUAL_INT(ISC_ENUM_END, rc);

    /* 再次调用 — 游标归零后应能重新开始 */
    rc = isc_query_next_ctrl(dev, &desc);
    TEST_ASSERT_ISC_OK(rc);

    isc_close(dev);
}

void test_query_next_ctrl_resets_after_query_ctrl(void)
{
    isc_dev_t *dev = NULL;
    isc_open("mock_sensor", &dev);

    /* 枚举两个 */
    isc_ctrl_desc_t desc;
    isc_query_next_ctrl(dev, &desc);
    isc_query_next_ctrl(dev, &desc);
    TEST_ASSERT_EQUAL_UINT(ISC_CID_VFLIP, desc.cid);

    /* 直接查询一个 CID — 重置游标 */
    memset(&desc, 0, sizeof(desc));
    desc.cid = ISC_CID_TEMPERATURE;
    isc_query_ctrl(dev, &desc);

    /* 再次枚举 — 应从第一个开始 */
    isc_query_next_ctrl(dev, &desc);
    TEST_ASSERT_EQUAL_UINT(ISC_CID_HFLIP, desc.cid);

    isc_close(dev);
}

void test_query_menu(void)
{
    isc_dev_t *dev = NULL;
    isc_open("mock_sensor", &dev);

    char name[ISC_MAX_MENU_NAME];
    int rc = isc_query_menu(dev, ISC_CID_TEST_PATTERN, 0, name);
    TEST_ASSERT_ISC_OK(rc);
    TEST_ASSERT_EQUAL_STRING("Disabled", name);

    rc = isc_query_menu(dev, ISC_CID_TEST_PATTERN, 1, name);
    TEST_ASSERT_ISC_OK(rc);
    TEST_ASSERT_EQUAL_STRING("Color Bars", name);

    isc_close(dev);
}

void test_query_menu_out_of_range(void)
{
    isc_dev_t *dev = NULL;
    isc_open("mock_sensor", &dev);

    char name[ISC_MAX_MENU_NAME];
    int rc = isc_query_menu(dev, ISC_CID_TEST_PATTERN, 99, name);
    TEST_ASSERT_ISC_ERR(rc, ISC_ERR_CTRL_RANGE);

    isc_close(dev);
}

void test_query_menu_not_enum_type(void)
{
    isc_dev_t *dev = NULL;
    isc_open("mock_sensor", &dev);

    char name[ISC_MAX_MENU_NAME];
    int rc = isc_query_menu(dev, ISC_CID_EXPOSURE, 0, name);
    TEST_ASSERT_ISC_ERR(rc, ISC_ERR_NOT_SUPPORTED);

    isc_close(dev);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  isc_get_ctrl / isc_set_ctrl
 * ═══════════════════════════════════════════════════════════════════════════ */

void test_get_ctrl_returns_default(void)
{
    isc_dev_t *dev = NULL;
    isc_open("mock_sensor", &dev);

    isc_ctrl_value_t val;
    int rc = isc_get_ctrl(dev, ISC_CID_EXPOSURE, &val);
    TEST_ASSERT_ISC_OK(rc);
    TEST_ASSERT_EQUAL_INT64(15000000, val.i64);

    isc_close(dev);
}

void test_set_ctrl_writes_value(void)
{
    isc_dev_t *dev = NULL;
    isc_open("mock_sensor", &dev);

    isc_ctrl_value_t val;
    val.i64 = 5000000;  /* 5ms */
    int rc = isc_set_ctrl(dev, ISC_CID_EXPOSURE, val);
    TEST_ASSERT_ISC_OK(rc);

    /* 读回验证 */
    isc_ctrl_value_t rd;
    isc_get_ctrl(dev, ISC_CID_EXPOSURE, &rd);
    TEST_ASSERT_EQUAL_INT64(5000000, rd.i64);

    isc_close(dev);
}

void test_set_ctrl_clamps_to_range(void)
{
    isc_dev_t *dev = NULL;
    isc_open("mock_sensor", &dev);

    /* 曝光范围 [1000, 1_000_000_000] */
    isc_ctrl_value_t val;
    val.i64 = 9999999999LL;  /* 超 max */
    int rc = isc_set_ctrl(dev, ISC_CID_EXPOSURE, val);
    TEST_ASSERT_ISC_OK(rc);

    isc_ctrl_value_t rd;
    isc_get_ctrl(dev, ISC_CID_EXPOSURE, &rd);
    TEST_ASSERT_EQUAL_INT64(1000000000LL, rd.i64);  /* 钳位到 max */

    isc_close(dev);
}

void test_set_ctrl_readonly_should_fail(void)
{
    isc_dev_t *dev = NULL;
    isc_open("mock_sensor", &dev);

    isc_ctrl_value_t val;
    val.f = 100.0f;
    int rc = isc_set_ctrl(dev, ISC_CID_TEMPERATURE, val);
    TEST_ASSERT_ISC_ERR(rc, ISC_ERR_NOT_SUPPORTED);

    isc_close(dev);
}

static uint32_t g_cb_cid;
static isc_ctrl_value_t g_cb_val;

static void my_ctrl_cb(isc_dev_t *d, uint32_t cid, isc_ctrl_value_t v, void *u)
{
    (void)d; (void)u;
    g_cb_cid = cid;
    g_cb_val = v;
}

void test_set_ctrl_triggers_callback(void)
{
    isc_dev_t *dev = NULL;
    isc_open("mock_sensor", &dev);

    g_cb_cid = 0;
    memset(&g_cb_val, 0, sizeof(g_cb_val));
    isc_register_ctrl_callback(dev, my_ctrl_cb, NULL);

    isc_ctrl_value_t val;
    val.i64 = 240;
    isc_set_ctrl(dev, ISC_CID_ANALOG_GAIN, val);

    TEST_ASSERT_EQUAL_UINT(ISC_CID_ANALOG_GAIN, g_cb_cid);
    TEST_ASSERT_EQUAL_INT64(240, g_cb_val.i64);

    isc_close(dev);
}

void test_set_ctrl_non_streamable_in_streaming_should_fail(void)
{
    isc_dev_t *dev = NULL;
    isc_open("mock_sensor", &dev);
    isc_stream_on(dev);

    /* FRAME_RATE 不可在流中修改 (mock 没设 STREAMABLE) */
    isc_ctrl_value_t val;
    val.f = 60.0f;
    int rc = isc_set_ctrl(dev, ISC_CID_FRAME_RATE, val);
    TEST_ASSERT_ISC_ERR(rc, ISC_ERR_BUSY);

    isc_stream_off(dev);
    isc_close(dev);
}

void test_set_ctrl_streamable_in_streaming_ok(void)
{
    isc_dev_t *dev = NULL;
    isc_open("mock_sensor", &dev);
    isc_stream_on(dev);

    /* EXPOSURE 标记为 STREAMABLE — 流中可改 */
    isc_ctrl_value_t val;
    val.i64 = 10000000;
    int rc = isc_set_ctrl(dev, ISC_CID_EXPOSURE, val);
    TEST_ASSERT_ISC_OK(rc);

    isc_stream_off(dev);
    isc_close(dev);
}

void test_set_ctrl_null_params_should_fail(void)
{
    isc_dev_t *dev = NULL;
    isc_open("mock_sensor", &dev);

    TEST_ASSERT_ISC_ERR(isc_set_ctrl(NULL, ISC_CID_EXPOSURE,
        (isc_ctrl_value_t){.i64=0}), ISC_ERR_INVALID_ARG);

    isc_close(dev);
}

void test_get_ctrl_null_params_should_fail(void)
{
    isc_dev_t *dev = NULL;
    isc_open("mock_sensor", &dev);

    TEST_ASSERT_ISC_ERR(isc_get_ctrl(NULL, ISC_CID_EXPOSURE,
        &(isc_ctrl_value_t){0}), ISC_ERR_INVALID_ARG);
    TEST_ASSERT_ISC_ERR(isc_get_ctrl(dev, ISC_CID_EXPOSURE, NULL),
        ISC_ERR_INVALID_ARG);

    isc_close(dev);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  isc_get_ext_ctrls / isc_set_ext_ctrls — 批量操作
 * ═══════════════════════════════════════════════════════════════════════════ */

void test_batch_set_get_roundtrip(void)
{
    isc_dev_t *dev = NULL;
    isc_open("mock_sensor", &dev);

    isc_ext_ctrls_t ctrls = { .count = 3 };
    ctrls.items[0].cid       = ISC_CID_EXPOSURE;
    ctrls.items[0].value.i64 = 20000000;
    ctrls.items[1].cid       = ISC_CID_ANALOG_GAIN;
    ctrls.items[1].value.i64 = 100;
    ctrls.items[2].cid       = ISC_CID_HFLIP;
    ctrls.items[2].value.b   = 1;

    int rc = isc_set_ext_ctrls(dev, &ctrls);
    TEST_ASSERT_ISC_OK(rc);

    /* 批量读回 */
    isc_ext_ctrls_t rd = { .count = 3 };
    rd.items[0].cid = ISC_CID_EXPOSURE;
    rd.items[1].cid = ISC_CID_ANALOG_GAIN;
    rd.items[2].cid = ISC_CID_HFLIP;

    rc = isc_get_ext_ctrls(dev, &rd);
    TEST_ASSERT_ISC_OK(rc);
    TEST_ASSERT_EQUAL_INT64(20000000, rd.items[0].value.i64);
    TEST_ASSERT_EQUAL_INT64(100, rd.items[1].value.i64);
    TEST_ASSERT_EQUAL_UINT8(1, rd.items[2].value.b);

    isc_close(dev);
}

void test_batch_set_reports_error_index(void)
{
    isc_dev_t *dev = NULL;
    isc_open("mock_sensor", &dev);

    isc_ext_ctrls_t ctrls = { .count = 3 };
    ctrls.items[0].cid       = ISC_CID_EXPOSURE;
    ctrls.items[0].value.i64 = 10000000;
    ctrls.items[1].cid       = ISC_CID_TEMPERATURE;  /* 只读, 会失败 */
    ctrls.items[1].value.f   = 100.0f;
    ctrls.items[2].cid       = ISC_CID_HFLIP;
    ctrls.items[2].value.b   = 1;

    int rc = isc_set_ext_ctrls(dev, &ctrls);
    TEST_ASSERT_ISC_ERR(rc, ISC_ERR_NOT_SUPPORTED);
    TEST_ASSERT_EQUAL_UINT(1, ctrls.error_idx);  /* 第 2 项(索引 1)失败 */

    isc_close(dev);
}

void test_batch_clears_error_idx_on_success(void)
{
    isc_dev_t *dev = NULL;
    isc_open("mock_sensor", &dev);

    isc_ext_ctrls_t ctrls = { .count = 2, .error_idx = 99 };
    ctrls.items[0].cid       = ISC_CID_EXPOSURE;
    ctrls.items[0].value.i64 = 10000000;
    ctrls.items[1].cid       = ISC_CID_ANALOG_GAIN;
    ctrls.items[1].value.i64 = 200;

    int rc = isc_set_ext_ctrls(dev, &ctrls);
    TEST_ASSERT_ISC_OK(rc);
    TEST_ASSERT_EQUAL_UINT(0, ctrls.error_idx);  /* 成功时清零 */

    isc_close(dev);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  isc_stream_on / isc_stream_off
 * ═══════════════════════════════════════════════════════════════════════════ */

void test_stream_on_notifies_fpga(void)
{
    isc_dev_t *dev = NULL;
    isc_open("mock_sensor", &dev);

    g_mock_fpga.stream_state_count = 0;
    int rc = isc_stream_on(dev);
    TEST_ASSERT_ISC_OK(rc);
    TEST_ASSERT_EQUAL_INT(1, g_mock_fpga.stream_state_count);
    TEST_ASSERT_EQUAL_UINT8(1, g_mock_fpga.last_stream_state);

    isc_stream_off(dev);
    TEST_ASSERT_EQUAL_INT(2, g_mock_fpga.stream_state_count);
    TEST_ASSERT_EQUAL_UINT8(0, g_mock_fpga.last_stream_state);

    isc_close(dev);
}

void test_stream_on_requires_open(void)
{
    isc_dev_t *dev = NULL;
    isc_open("mock_sensor", &dev);
    isc_close(dev);

    int rc = isc_stream_on(dev);  /* 已关闭 */
    TEST_ASSERT_ISC_ERR(rc, ISC_ERR_STATE);
}

void test_stream_double_on_should_fail(void)
{
    isc_dev_t *dev = NULL;
    isc_open("mock_sensor", &dev);
    isc_stream_on(dev);

    int rc = isc_stream_on(dev);  /* 已在流中 */
    TEST_ASSERT_ISC_ERR(rc, ISC_ERR_STATE);

    isc_stream_off(dev);
    isc_close(dev);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  isc_query_cap
 * ═══════════════════════════════════════════════════════════════════════════ */

void test_query_cap_returns_cached_values(void)
{
    isc_dev_t *dev = NULL;
    isc_open("mock_sensor", &dev);

    isc_cap_t cap;
    int rc = isc_query_cap(dev, &cap);
    TEST_ASSERT_ISC_OK(rc);

    TEST_ASSERT_EQUAL_STRING("mock_sensor", cap.model);
    TEST_ASSERT_EQUAL_STRING("MockVendor", cap.vendor);
    TEST_ASSERT_TRUE(cap.capabilities & ISC_CAP_TIMING_QUERY);
    TEST_ASSERT_TRUE(cap.capabilities & ISC_CAP_ROI);
    TEST_ASSERT_EQUAL_UINT8(2, cap.num_formats);
    /* num_ctrls: mock 提供 8 个控制项 */
    TEST_ASSERT_EQUAL_UINT8(8, cap.num_ctrls);

    isc_close(dev);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  isc_query_timing
 * ═══════════════════════════════════════════════════════════════════════════ */

void test_query_timing_computes_derived(void)
{
    isc_dev_t *dev = NULL;
    isc_open("mock_sensor", &dev);

    isc_timing_t t;
    int rc = isc_query_timing(dev, &t);
    TEST_ASSERT_ISC_OK(rc);

    TEST_ASSERT_EQUAL_UINT(74250000, t.pixel_clock_hz);
    TEST_ASSERT_EQUAL_UINT(2200, t.line_length_pclk);
    /* line_period: 1e9 * 2200 / 74250000 ≈ 29629 ns */
    TEST_ASSERT(t.line_period_ns > 0);

    isc_close(dev);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Runner
 * ═══════════════════════════════════════════════════════════════════════════ */

void run_test_isc_control(void)
{
    RUN_TEST(test_query_ctrl_by_cid);
    RUN_TEST(test_query_ctrl_unsupported_returns_error);
    RUN_TEST(test_query_next_ctrl_enumerates_all);
    RUN_TEST(test_query_next_ctrl_terminates);
    RUN_TEST(test_query_next_ctrl_resets_after_query_ctrl);
    RUN_TEST(test_query_menu);
    RUN_TEST(test_query_menu_out_of_range);
    RUN_TEST(test_query_menu_not_enum_type);

    RUN_TEST(test_get_ctrl_returns_default);
    RUN_TEST(test_set_ctrl_writes_value);
    RUN_TEST(test_set_ctrl_clamps_to_range);
    RUN_TEST(test_set_ctrl_readonly_should_fail);
    RUN_TEST(test_set_ctrl_triggers_callback);
    RUN_TEST(test_set_ctrl_non_streamable_in_streaming_should_fail);
    RUN_TEST(test_set_ctrl_streamable_in_streaming_ok);
    RUN_TEST(test_set_ctrl_null_params_should_fail);
    RUN_TEST(test_get_ctrl_null_params_should_fail);

    RUN_TEST(test_batch_set_get_roundtrip);
    RUN_TEST(test_batch_set_reports_error_index);
    RUN_TEST(test_batch_clears_error_idx_on_success);

    RUN_TEST(test_stream_on_notifies_fpga);
    RUN_TEST(test_stream_on_requires_open);
    RUN_TEST(test_stream_double_on_should_fail);

    RUN_TEST(test_query_cap_returns_cached_values);
    RUN_TEST(test_query_timing_computes_derived);
}
