/**
 * @file    test_isc_lifecycle.c
 * @brief   ISC 单元测试 — 生命周期 (init/deinit/open/close)
 */

#include "test_isc_common.h"

/* ═══════════════════════════════════════════════════════════════════════════
 *  isc_init / isc_deinit
 * ═══════════════════════════════════════════════════════════════════════════ */

void test_init_null_port(void)
{
    isc_deinit();  /* 先退出 setup 创建的实例 */

    /* port=NULL 允许 (所有传感器自带 port) */
    const isc_sensor_ops_t *s[] = { &g_mock_sensor_ops };
    int rc = isc_init(NULL, &g_mock_fpga_ops, s, 1);
    TEST_ASSERT_ISC_OK(rc);
}

void test_init_null_fpga_ops_should_fail(void)
{
    isc_deinit();
    const isc_sensor_ops_t *s[] = { &g_mock_sensor_ops };
    int rc = isc_init(&g_mock_port, NULL, s, 1);
    TEST_ASSERT_ISC_ERR(rc, ISC_ERR_INVALID_ARG);
}

void test_init_null_sensors_should_fail(void)
{
    isc_deinit();
    int rc = isc_init(&g_mock_port, &g_mock_fpga_ops, NULL, 1);
    TEST_ASSERT_ISC_ERR(rc, ISC_ERR_INVALID_ARG);
}

void test_init_zero_sensors_should_fail(void)
{
    isc_deinit();
    const isc_sensor_ops_t *s[] = { &g_mock_sensor_ops };
    int rc = isc_init(&g_mock_port, &g_mock_fpga_ops, s, 0);
    TEST_ASSERT_ISC_ERR(rc, ISC_ERR_INVALID_ARG);
}

void test_init_exceed_max_sensors_should_fail(void)
{
    isc_deinit();
    /* ISC_MAX_SENSORS=4, 传 5 个传感器 */
    const isc_sensor_ops_t *s[5] = {
        &g_mock_sensor_ops, &g_mock_sensor_ops,
        &g_mock_sensor_ops, &g_mock_sensor_ops, &g_mock_sensor_ops
    };
    int rc = isc_init(&g_mock_port, &g_mock_fpga_ops, s, 5);
    TEST_ASSERT_ISC_ERR(rc, ISC_ERR_INVALID_ARG);
}

void test_init_twice_is_idempotent(void)
{
    int rc = isc_init(&g_mock_port, &g_mock_fpga_ops,
                      (const isc_sensor_ops_t *const []){ &g_mock_sensor_ops }, 1);
    TEST_ASSERT_ISC_OK(rc);
}

void test_is_initialized(void)
{
    TEST_ASSERT_EQUAL_INT(1, isc_is_initialized());
    isc_deinit();
    TEST_ASSERT_EQUAL_INT(0, isc_is_initialized());
}

void test_deinit_twice_is_idempotent(void)
{
    isc_deinit();
    int rc = isc_deinit();  /* 第二次 */
    TEST_ASSERT_ISC_OK(rc);
}

void test_deinit_closes_all_devices(void)
{
    /* 打开两个设备 (ISC_MAX_DEVS=2) */
    isc_dev_t *d1 = NULL, *d2 = NULL;
    isc_open("mock_sensor", &d1);
    mock_sensor_reset();
    isc_open("mock_sensor", &d2);
    TEST_ASSERT_NOT_NULL(d1);
    TEST_ASSERT_NOT_NULL(d2);

    isc_deinit();
    /* deinit 后 isc_open 应重新工作 */
    int rc = isc_init(&g_mock_port, &g_mock_fpga_ops,
                      (const isc_sensor_ops_t *const []){ &g_mock_sensor_ops }, 1);
    TEST_ASSERT_ISC_OK(rc);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  isc_open
 * ═══════════════════════════════════════════════════════════════════════════ */

void test_open_by_model(void)
{
    isc_dev_t *dev = NULL;
    int rc = isc_open("mock_sensor", &dev);
    TEST_ASSERT_ISC_OK(rc);
    TEST_ASSERT_NOT_NULL(dev);
    TEST_ASSERT_EQUAL_INT(1, g_mock_sensor.probe_called);
    TEST_ASSERT_EQUAL_INT(1, g_mock_sensor.init_called);
    isc_close(dev);
}

void test_open_null_dev_ptr_should_fail(void)
{
    int rc = isc_open("mock_sensor", NULL);
    TEST_ASSERT_ISC_ERR(rc, ISC_ERR_INVALID_ARG);
}

void test_open_unknown_model_should_fail(void)
{
    isc_dev_t *dev = NULL;
    int rc = isc_open("nonexistent", &dev);
    TEST_ASSERT_ISC_ERR(rc, ISC_ERR_NOT_FOUND);
    TEST_ASSERT_NULL(dev);
}

void test_open_probe_fails_should_fail(void)
{
    mock_sensor_set_probe_ret(ISC_ERR_IO);
    isc_dev_t *dev = NULL;
    int rc = isc_open("mock_sensor", &dev);
    TEST_ASSERT_ISC_ERR(rc, ISC_ERR_IO);
    /* deinit 应在 probe 失败后被调用 */
    TEST_ASSERT_EQUAL_INT(1, g_mock_sensor.deinit_called);
}

void test_open_init_fails_should_fail(void)
{
    mock_sensor_set_init_ret(ISC_ERR_IO);
    isc_dev_t *dev = NULL;
    int rc = isc_open("mock_sensor", &dev);
    TEST_ASSERT_ISC_ERR(rc, ISC_ERR_IO);
}

void test_open_auto_detect(void)
{
    isc_dev_t *dev = NULL;
    int rc = isc_open(NULL, &dev);  /* model=NULL → 自动探测 */
    TEST_ASSERT_ISC_OK(rc);
    TEST_ASSERT_NOT_NULL(dev);
    TEST_ASSERT_EQUAL_INT(1, g_mock_sensor.probe_called);
    isc_close(dev);
}

void test_open_exceed_max_devs_should_fail(void)
{
    isc_dev_t *d1 = NULL, *d2 = NULL, *d3 = NULL;
    isc_open("mock_sensor", &d1);
    mock_sensor_reset();
    isc_open("mock_sensor", &d2);

    /* ISC_MAX_DEVS=2, 第三个应失败 */
    mock_sensor_reset();
    int rc = isc_open("mock_sensor", &d3);
    TEST_ASSERT_ISC_ERR(rc, ISC_ERR_NO_MEM);

    isc_close(d1);
    isc_close(d2);
}

void test_open_sets_initial_fmt(void)
{
    isc_dev_t *dev = NULL;
    isc_open("mock_sensor", &dev);

    isc_fmt_t fmt;
    isc_get_fmt(dev, &fmt);
    TEST_ASSERT_EQUAL_UINT(1920, fmt.width);
    TEST_ASSERT_EQUAL_UINT(1080, fmt.height);

    /* open 时应通知 FPGA 格式 */
    TEST_ASSERT_EQUAL_INT(1, g_mock_fpga.format_changed_count);

    isc_close(dev);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  isc_close
 * ═══════════════════════════════════════════════════════════════════════════ */

void test_close_null_should_fail(void)
{
    int rc = isc_close(NULL);
    TEST_ASSERT_ISC_ERR(rc, ISC_ERR_INVALID_ARG);
}

void test_close_unopened_is_idempotent(void)
{
    isc_dev_t *dev = NULL;
    isc_open("mock_sensor", &dev);
    isc_close(dev);
    /* 第二次 close — 状态已是 FREE, 应返回 OK */
    int rc = isc_close(dev);
    TEST_ASSERT_ISC_OK(rc);
}

void test_close_calls_deinit(void)
{
    isc_dev_t *dev = NULL;
    isc_open("mock_sensor", &dev);
    g_mock_sensor.deinit_called = 0;
    isc_close(dev);
    TEST_ASSERT_EQUAL_INT(1, g_mock_sensor.deinit_called);
}

void test_close_streaming_calls_stream_off(void)
{
    isc_dev_t *dev = NULL;
    isc_open("mock_sensor", &dev);
    isc_stream_on(dev);
    TEST_ASSERT_EQUAL_INT(1, g_mock_sensor.stream_state);

    isc_close(dev);
    TEST_ASSERT_EQUAL_INT(0, g_mock_sensor.stream_state);

    /* stream_on + stream_off(close) 各触发一次 FPGA 通知 */
    TEST_ASSERT_EQUAL_INT(2, g_mock_fpga.stream_state_count);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Runner
 * ═══════════════════════════════════════════════════════════════════════════ */

void run_test_isc_lifecycle(void)
{
    RUN_TEST(test_is_initialized);

    RUN_TEST(test_init_null_port);
    RUN_TEST(test_init_null_fpga_ops_should_fail);
    RUN_TEST(test_init_null_sensors_should_fail);
    RUN_TEST(test_init_zero_sensors_should_fail);
    RUN_TEST(test_init_exceed_max_sensors_should_fail);
    RUN_TEST(test_init_twice_is_idempotent);
    RUN_TEST(test_deinit_twice_is_idempotent);
    RUN_TEST(test_deinit_closes_all_devices);

    RUN_TEST(test_open_by_model);
    RUN_TEST(test_open_null_dev_ptr_should_fail);
    RUN_TEST(test_open_unknown_model_should_fail);
    RUN_TEST(test_open_probe_fails_should_fail);
    RUN_TEST(test_open_init_fails_should_fail);
    RUN_TEST(test_open_auto_detect);
    RUN_TEST(test_open_exceed_max_devs_should_fail);
    RUN_TEST(test_open_sets_initial_fmt);

    RUN_TEST(test_close_null_should_fail);
    RUN_TEST(test_close_unopened_is_idempotent);
    RUN_TEST(test_close_calls_deinit);
    RUN_TEST(test_close_streaming_calls_stream_off);
}
