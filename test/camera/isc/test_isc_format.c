/**
 * @file    test_isc_format.c
 * @brief   ISC 单元测试 — 格式协商 (enum/try/set/get_fmt)
 */

#include "test_isc_common.h"
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════════
 *  isc_enum_fmt
 * ═══════════════════════════════════════════════════════════════════════════ */

void test_enum_fmt_returns_all_formats(void)
{
    isc_dev_t *dev = NULL;
    isc_open("mock_sensor", &dev);

    isc_fmt_desc_t desc;
    /* 索引 0 */
    int rc = isc_enum_fmt(dev, 0, &desc);
    TEST_ASSERT_ISC_OK(rc);
    TEST_ASSERT_EQUAL_UINT(ISC_PIX_FMT_SRGGB8, desc.pixel_format);
    TEST_ASSERT_EQUAL_UINT(8, desc.bit_depth);

    /* 索引 1 */
    rc = isc_enum_fmt(dev, 1, &desc);
    TEST_ASSERT_ISC_OK(rc);
    TEST_ASSERT_EQUAL_UINT(ISC_PIX_FMT_SBGGR10, desc.pixel_format);
    TEST_ASSERT_EQUAL_UINT(10, desc.bit_depth);

    /* 索引 2 — 穷尽 */
    rc = isc_enum_fmt(dev, 2, &desc);
    TEST_ASSERT_EQUAL_INT(ISC_ENUM_END, rc);

    isc_close(dev);
}

void test_enum_fmt_null_params_should_fail(void)
{
    isc_dev_t *dev = NULL;
    isc_open("mock_sensor", &dev);

    TEST_ASSERT_ISC_ERR(isc_enum_fmt(NULL, 0, &(isc_fmt_desc_t){0}), ISC_ERR_INVALID_ARG);
    TEST_ASSERT_ISC_ERR(isc_enum_fmt(dev, 0, NULL), ISC_ERR_INVALID_ARG);

    isc_close(dev);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  isc_get_fmt
 * ═══════════════════════════════════════════════════════════════════════════ */

void test_get_fmt_returns_default_after_open(void)
{
    isc_dev_t *dev = NULL;
    isc_open("mock_sensor", &dev);

    isc_fmt_t fmt;
    memset(&fmt, 0xFF, sizeof(fmt));  /* 确保被覆盖 */
    int rc = isc_get_fmt(dev, &fmt);
    TEST_ASSERT_ISC_OK(rc);
    TEST_ASSERT_EQUAL_UINT(1920, fmt.width);
    TEST_ASSERT_EQUAL_UINT(1080, fmt.height);

    isc_close(dev);
}

void test_get_fmt_null_params_should_fail(void)
{
    isc_dev_t *dev = NULL;
    isc_open("mock_sensor", &dev);

    TEST_ASSERT_ISC_ERR(isc_get_fmt(NULL, &(isc_fmt_t){0}), ISC_ERR_INVALID_ARG);
    TEST_ASSERT_ISC_ERR(isc_get_fmt(dev, NULL), ISC_ERR_INVALID_ARG);

    isc_close(dev);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  isc_try_fmt — 无副作用格式试探
 * ═══════════════════════════════════════════════════════════════════════════ */

void test_try_fmt_aligns_crop(void)
{
    isc_dev_t *dev = NULL;
    isc_open("mock_sensor", &dev);

    isc_fmt_t fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.crop_width  = 101;  /* 奇数, 应向下对齐到 100 */
    fmt.crop_height = 55;   /* 奇数, 应向下对齐到 54  */
    fmt.width       = 100;
    fmt.height      = 50;

    int rc = isc_try_fmt(dev, &fmt);
    TEST_ASSERT_ISC_OK(rc);
    TEST_ASSERT_EQUAL_UINT(100, fmt.crop_width);
    TEST_ASSERT_EQUAL_UINT(54, fmt.crop_height);

    isc_close(dev);
}

void test_try_fmt_fills_default_pixel_format(void)
{
    isc_dev_t *dev = NULL;
    isc_open("mock_sensor", &dev);

    isc_fmt_t fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.width  = 640;
    fmt.height = 480;

    int rc = isc_try_fmt(dev, &fmt);
    TEST_ASSERT_ISC_OK(rc);
    /* pixel_format=0 时应填充当前格式 */
    TEST_ASSERT_EQUAL_UINT(ISC_PIX_FMT_SRGGB8, fmt.pixel_format);

    isc_close(dev);
}

void test_try_fmt_fills_default_crop_when_zero(void)
{
    isc_dev_t *dev = NULL;
    isc_open("mock_sensor", &dev);

    isc_fmt_t fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.width  = 100;
    fmt.height = 100;

    int rc = isc_try_fmt(dev, &fmt);
    TEST_ASSERT_ISC_OK(rc);
    /* crop_width=0 应填充默认值 */
    TEST_ASSERT_NOT_EQUAL(0, fmt.crop_width);
    TEST_ASSERT_NOT_EQUAL(0, fmt.crop_height);

    isc_close(dev);
}

void test_try_fmt_null_params_should_fail(void)
{
    isc_dev_t *dev = NULL;
    isc_open("mock_sensor", &dev);

    TEST_ASSERT_ISC_ERR(isc_try_fmt(NULL, &(isc_fmt_t){0}), ISC_ERR_INVALID_ARG);
    TEST_ASSERT_ISC_ERR(isc_try_fmt(dev, NULL), ISC_ERR_INVALID_ARG);

    isc_close(dev);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  isc_set_fmt — 提交格式, 写硬件 + 通知 FPGA
 * ═══════════════════════════════════════════════════════════════════════════ */

void test_set_fmt_notifies_fpga(void)
{
    isc_dev_t *dev = NULL;
    isc_open("mock_sensor", &dev);

    g_mock_fpga.format_changed_count = 0;  /* 清零 open 时产生的通知 */

    isc_fmt_t fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.width         = 640;
    fmt.height        = 480;
    fmt.pixel_format  = ISC_PIX_FMT_SRGGB8;
    fmt.bit_depth     = 8;
    fmt.crop_width    = 640;
    fmt.crop_height   = 480;
    fmt.frame_rate_num = 30;
    fmt.frame_rate_den = 1;

    int rc = isc_set_fmt(dev, &fmt);
    TEST_ASSERT_ISC_OK(rc);

    /* FPGA 应收到通知 */
    TEST_ASSERT_EQUAL_INT(1, g_mock_fpga.format_changed_count);
    TEST_ASSERT_EQUAL_UINT(640, g_mock_fpga.last_fmt.width);
    TEST_ASSERT_EQUAL_UINT(480, g_mock_fpga.last_fmt.height);

    /* get_fmt 应返回更新后的值 */
    isc_fmt_t cur;
    isc_get_fmt(dev, &cur);
    TEST_ASSERT_EQUAL_UINT(640, cur.width);
    TEST_ASSERT_EQUAL_UINT(480, cur.height);

    isc_close(dev);
}

void test_set_fmt_updates_current_fmt(void)
{
    isc_dev_t *dev = NULL;
    isc_open("mock_sensor", &dev);

    isc_fmt_t fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.width         = 1280;
    fmt.height        = 720;
    fmt.pixel_format  = ISC_PIX_FMT_SBGGR10;
    fmt.bit_depth     = 10;
    fmt.crop_width    = 1280;
    fmt.crop_height   = 720;
    fmt.frame_rate_num = 60;
    fmt.frame_rate_den = 1;

    isc_set_fmt(dev, &fmt);

    /* get_fmt 应返回新值 */
    isc_fmt_t cur;
    isc_get_fmt(dev, &cur);
    TEST_ASSERT_EQUAL_UINT(1280, cur.width);
    TEST_ASSERT_EQUAL_UINT(720, cur.height);
    TEST_ASSERT_EQUAL_UINT(ISC_PIX_FMT_SBGGR10, cur.pixel_format);

    isc_close(dev);
}

void test_set_fmt_null_params_should_fail(void)
{
    isc_dev_t *dev = NULL;
    isc_open("mock_sensor", &dev);

    TEST_ASSERT_ISC_ERR(isc_set_fmt(NULL, &(isc_fmt_t){0}), ISC_ERR_INVALID_ARG);
    TEST_ASSERT_ISC_ERR(isc_set_fmt(dev, NULL), ISC_ERR_INVALID_ARG);

    isc_close(dev);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Runner
 * ═══════════════════════════════════════════════════════════════════════════ */

void run_test_isc_format(void)
{
    RUN_TEST(test_enum_fmt_returns_all_formats);
    RUN_TEST(test_enum_fmt_null_params_should_fail);

    RUN_TEST(test_get_fmt_returns_default_after_open);
    RUN_TEST(test_get_fmt_null_params_should_fail);

    RUN_TEST(test_try_fmt_aligns_crop);
    RUN_TEST(test_try_fmt_fills_default_pixel_format);
    RUN_TEST(test_try_fmt_fills_default_crop_when_zero);
    RUN_TEST(test_try_fmt_null_params_should_fail);

    RUN_TEST(test_set_fmt_notifies_fpga);
    RUN_TEST(test_set_fmt_updates_current_fmt);
    RUN_TEST(test_set_fmt_null_params_should_fail);
}
