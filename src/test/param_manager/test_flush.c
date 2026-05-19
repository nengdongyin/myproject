#include "test_common.h"
#include <string.h>

DEF_TEST_UINT  (fu32, TID_APPLET_UINT,  PARAM_FLAG_PERSIST, 100, 0, 200);
PARAM_BOOL     (fboo, TID_APPLET_BOOL,  PARAM_FLAG_PERSIST, true);
DEF_TEST_UINT  (fu2,  TID_APPLET2_UINT, PARAM_FLAG_PERSIST, 200, 0, 300);

PARAM_IP_UINT  (fipu,  TID_IP_UINT,  PARAM_FLAG_PERSIST, 500);
PARAM_IP_FLOAT (fipf,  TID_IP_FLOAT, PARAM_FLAG_PERSIST, 2.0f);
PARAM_IP_BOOL  (fipb,  TID_IP_BOOL,  PARAM_FLAG_PERSIST, false);

PARAM_TABLE(applet_params,  &fu32.base, &fboo.base);
PARAM_TABLE(applet2_params, &fu2.base);
PARAM_TABLE(ip_params, &fipu.base, &fipf.base, &fipb.base);

PARAM_MODULE_DEFINE(test_applet,  TEST_MODULE_APPLET,  "FlushApplet",  mock_flush_ok, mock_apply_ok);
PARAM_MODULE_DEFINE(test_applet2, TEST_MODULE_APPLET2, "FlushApplet2", mock_flush_ok, mock_apply_ok);
IP_DRIVER_DEFINE(test_ip, TEST_MODULE_IP, "FlushIP", NULL, NULL, mock_ip_write_ok, NULL);

static void register_all(void)
{
    param_module_register(&test_applet_module, applet_params, PARAM_COUNT(applet_params));
    param_module_register(&test_applet2_module, applet2_params, PARAM_COUNT(applet2_params));
    ip_driver_register(&test_ip_instance, ip_params, PARAM_COUNT(ip_params));
    test_reset_entry(&fu32.base); test_reset_entry(&fboo.base);
    test_reset_entry(&fu2.base);
    test_reset_entry(&fipu.base); test_reset_entry(&fipf.base);
    test_reset_entry(&fipb.base);
}

/* ================================================================
 *  flush 基础行为
 * ================================================================ */
void test_flush_no_dirty_returns_ok(void) {
    register_all();
    TEST_ASSERT_PARAM_OK(param_flush());
}

void test_flush_with_dirty_applet(void) {
    register_all();

    param_value_t v = { .u32 = 150 };
    param_write(TID_APPLET_UINT, v);

    mock_flush_reset();
    TEST_ASSERT_PARAM_OK(param_flush());

    TEST_ASSERT_EQUAL_INT(1, g_flush_call_count);

    param_stats_t stats;
    param_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT16(0, stats.dirty_count);
}

void test_flush_skips_clean_modules(void) {
    register_all();

    param_value_t v = { .u32 = 50 };
    param_write(TID_APPLET2_UINT, v);

    mock_flush_reset();
    param_flush();

    TEST_ASSERT_EQUAL_INT(1, g_flush_call_count);
}

void test_flush_ip_writes_hardware(void) {
    register_all();

    param_value_t v = { .u32 = 700 };
    mock_ip_reset();
    param_write(TID_IP_UINT, v);

    param_flush();

    TEST_ASSERT_EQUAL_INT(1, g_ip_write_call_count);
    TEST_ASSERT_EQUAL_UINT32(700, g_ip_write_last_value.u32);
}

void test_flush_clears_ip_dirty_map(void) {
    register_all();

    param_value_t v = { .u32 = 800 };
    param_write(TID_IP_UINT, v);

    param_flush();

    param_stats_t stats;
    param_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT16(0, stats.dirty_count);
}

void test_flush_multiple_ip_params(void) {
    register_all();

    param_value_t v1 = { .u32 = 100 };
    param_value_t v2 = { .f32 = 3.0f };
    param_value_t v3 = { .b = true };

    param_write(TID_IP_UINT, v1);
    param_write(TID_IP_FLOAT, v2);
    param_write(TID_IP_BOOL, v3);

    mock_ip_reset();
    param_flush();

    TEST_ASSERT_EQUAL_INT(3, g_ip_write_call_count);
}

void test_flush_applet_then_ip_order(void) {
    register_all();

    param_value_t v = { .u32 = 1 };
    param_write(TID_APPLET_UINT, v);
    param_write(TID_IP_UINT, v);

    mock_flush_reset();
    mock_ip_reset();

    param_flush();

    TEST_ASSERT_EQUAL_INT(1, g_flush_call_count);
    TEST_ASSERT_EQUAL_INT(1, g_ip_write_call_count);
}

void run_test_flush(void)
{
    RUN_TEST(test_flush_no_dirty_returns_ok);
    RUN_TEST(test_flush_with_dirty_applet);
    RUN_TEST(test_flush_skips_clean_modules);
    RUN_TEST(test_flush_ip_writes_hardware);
    RUN_TEST(test_flush_clears_ip_dirty_map);
    RUN_TEST(test_flush_multiple_ip_params);
    RUN_TEST(test_flush_applet_then_ip_order);
}
