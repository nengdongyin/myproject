#include "test_common.h"
#include "module_ids.h"
#include <string.h>

static int g_init_seq;
static int g_ip_init_seq;
static int g_applet_init_seq;

static int mock_init_seq_ip(void *ctx)
{
    (void)ctx;
    g_ip_init_seq = g_init_seq++;
    return PARAM_OK;
}

static int mock_init_seq_applet(void *ctx)
{
    (void)ctx;
    g_applet_init_seq = g_init_seq++;
    return PARAM_OK;
}

PARAM_IP_UINT(ip_p1, TID_IP_UINT, PARAM_FLAG_PERSIST, 100,   100,   100);
PARAM_IP_FLOAT(ip_p2, TID_IP_FLOAT, PARAM_FLAG_PERSIST, 1.0f,  1.0f,  1.0f);

PARAM_TABLE(ip_params, &ip_p1.base, &ip_p2.base);

IP_DRIVER_DEFINE(order_ip, TEST_MODULE_IP, "OrderIP", .write = mock_ip_write_ok);

DEF_TEST_UINT(app_p1, TID_APPLET_UINT, PARAM_FLAG_PERSIST, 50, 0, 100);
DEF_TEST_INT(app_p2, TID_APPLET_INT, PARAM_FLAG_PERSIST, 0, -50, 50);

PARAM_TABLE(applet_params, &app_p1.base, &app_p2.base);

PARAM_MODULE_DEFINE(order_applet, TEST_MODULE_APPLET, "OrderApplet", .write = mock_apply_ok);

/* ================================================================
 *  注册顺序测试
 * ================================================================ */
void test_order_ip_before_applet(void)
{
    g_init_seq = 0;
    g_ip_init_seq = -1;
    g_applet_init_seq = -1;

    order_ip_module.init = mock_init_seq_ip;
    order_applet_module.init = mock_init_seq_applet;

    param_module_register(&order_ip_module, ip_params, PARAM_COUNT(ip_params));
    param_module_register(&order_applet_module, applet_params, PARAM_COUNT(applet_params));

    param_load_all();

    TEST_ASSERT_TRUE(g_ip_init_seq < g_applet_init_seq);
}

void test_order_reverse_register(void)
{
    g_init_seq = 0;
    g_ip_init_seq = -1;
    g_applet_init_seq = -1;

    order_ip_module.init = mock_init_seq_ip;
    order_applet_module.init = mock_init_seq_applet;

    param_module_register(&order_applet_module, applet_params, PARAM_COUNT(applet_params));
    param_module_register(&order_ip_module, ip_params, PARAM_COUNT(ip_params));

    param_load_all();

    TEST_ASSERT_TRUE(g_applet_init_seq >= 0);
    TEST_ASSERT_TRUE(g_ip_init_seq >= 0);
    TEST_ASSERT_EQUAL_INT(2, g_init_seq);
}

/* ================================================================
 *  真实模块 ID 的顺序契约回归 (X-macro 清单的行序 = init/flush 顺序)
 *
 *  用 module_ids.h 真实清单中的 ID (IP_SENSOR=0x80, MODULE_AUTO_EXP=0x01)
 *  验证 "IP 硬件在前、业务模块在后" 的文档不变量 —— 若未来有人重排
 *  PARAM_MODULE_LIST 行序, 此测试即失败 (TEST_MODULE_* 用例用高位 ID,
 *  不在清单内, 全部尾插, 抓不住这类回归)。
 * ================================================================ */
static int g_real_seq;
static int g_real_ip_seq;
static int g_real_app_seq;

static int mock_real_ip_init(void *ctx)
{
    (void)ctx;
    g_real_ip_seq = g_real_seq++;
    return PARAM_OK;
}

static int mock_real_app_init(void *ctx)
{
    (void)ctx;
    g_real_app_seq = g_real_seq++;
    return PARAM_OK;
}

void test_real_id_order_ip_before_app(void)
{
    PARAM_IP_UINT(ri_p, MAKE_PARAM_ID(IP_SENSOR, 0), 0, 1, 1, 1);
    PARAM_UINT (ra_p, MAKE_PARAM_ID(MODULE_AUTO_EXP, 0), 0, 1, 1, 1);

    PARAM_TABLE(ri_params, &ri_p.base);
    PARAM_TABLE(ra_params, &ra_p.base);

    IP_DRIVER_DEFINE(ri, IP_SENSOR, "RI", .init = mock_real_ip_init);
    PARAM_MODULE_DEFINE(ra, MODULE_AUTO_EXP, "RA", .init = mock_real_app_init);

    g_real_seq = 0;
    g_real_ip_seq = -1;
    g_real_app_seq = -1;

    param_module_register(&ri_module, ri_params, PARAM_COUNT(ri_params));
    param_module_register(&ra_module, ra_params, PARAM_COUNT(ra_params));

    param_load_all();

    TEST_ASSERT_TRUE(g_real_ip_seq >= 0);
    TEST_ASSERT_TRUE(g_real_app_seq >= 0);
    TEST_ASSERT_TRUE(g_real_ip_seq < g_real_app_seq);
}

/* ================================================================
 *  param_init / param_deinit
 * ================================================================ */
void test_init_normal(void)
{
    param_deinit();
    mock_storage_reset();
    int ret = param_init(&g_mock_storage, NULL);
    TEST_ASSERT_PARAM_OK(ret);
}

void test_init_null_storage(void)
{
    param_deinit();
    mock_storage_reset();
    int ret = param_init(NULL, NULL);
    TEST_ASSERT_PARAM_OK(ret);
}

void test_init_twice(void)
{
    int ret = param_init(&g_mock_storage, NULL);
    TEST_ASSERT_PARAM_ERR(ret, PARAM_ERR_BUSY);
}

void test_deinit_calls_storage_deinit(void)
{
    param_deinit();
    TEST_ASSERT_EQUAL_INT(1, g_mock_deinit_called);
}

void test_deinit_multiple_modules(void)
{
    param_module_register(&order_ip_module, ip_params, PARAM_COUNT(ip_params));
    param_module_register(&order_applet_module, applet_params, PARAM_COUNT(applet_params));

    param_deinit();
    param_init(&g_mock_storage, NULL);
    TEST_ASSERT_PARAM_OK(PARAM_OK);
}

/* ================================================================
 *  load_all 两阶段
 * ================================================================ */
static int g_load_seq;
static int g_load_phase_ip;
static int g_load_phase_applet;

static int mock_init_phase_ip(void *ctx)
{
    (void)ctx;
    g_load_phase_ip = g_load_seq++;
    param_value_t v;
    param_read(TID_IP_UINT, &v);
    return PARAM_OK;
}

static int mock_init_phase_applet(void *ctx)
{
    (void)ctx;
    g_load_phase_applet = g_load_seq++;
    param_value_t v;
    param_read(TID_APPLET_UINT, &v);
    return PARAM_OK;
}

void test_load_then_init_phase_order(void)
{
    g_load_seq = 0;
    g_load_phase_ip = -1;
    g_load_phase_applet = -1;

    order_ip_module.init = mock_init_phase_ip;
    order_applet_module.init = mock_init_phase_applet;

    param_module_register(&order_ip_module, ip_params, PARAM_COUNT(ip_params));
    param_module_register(&order_applet_module, applet_params, PARAM_COUNT(applet_params));

    param_value_t write_val = {.u32 = 555};
    param_write(TID_IP_UINT, write_val);
    mock_storage_preset(TID_IP_UINT, (const uint8_t *)&write_val, sizeof(write_val));
    mock_storage_preset(TID_APPLET_UINT, (const uint8_t *)&write_val, sizeof(write_val));

    param_load_all();

    TEST_ASSERT_TRUE(g_load_phase_ip < g_load_phase_applet);
}

static int g_captured_val;

static int capture_init_fn(void *ctx)
{
    (void)ctx;
    param_value_t v;
    param_read(TID_APPLET_UINT, &v);
    g_captured_val = (int)v.u32;
    return PARAM_OK;
}

void test_init_reads_loaded_data(void)
{
    order_applet_module.init = capture_init_fn;
    param_module_register(&order_applet_module, applet_params, PARAM_COUNT(applet_params));

    param_value_t preset = {.u32 = 777};
    mock_storage_preset(TID_APPLET_UINT, (const uint8_t *)&preset, sizeof(preset));

    g_captured_val = 0;
    param_load_all();

    TEST_ASSERT_EQUAL_INT(777, g_captured_val);
}

void run_test_init_order(void)
{
    RUN_TEST(test_order_ip_before_applet);
    RUN_TEST(test_order_reverse_register);
    RUN_TEST(test_real_id_order_ip_before_app);
    RUN_TEST(test_init_normal);
    RUN_TEST(test_init_null_storage);
    RUN_TEST(test_init_twice);
    RUN_TEST(test_deinit_calls_storage_deinit);
    RUN_TEST(test_deinit_multiple_modules);
    RUN_TEST(test_load_then_init_phase_order);
    RUN_TEST(test_init_reads_loaded_data);
}
