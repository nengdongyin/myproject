#include "test_common.h"
#include "module_ids.h"
#include <string.h>

DEF_TEST_UINT (eu32, TID_APPLET_UINT, PARAM_FLAG_PERSIST, 100, 0, 200);
PARAM_BOOL    (erdo, TID_APPLET_RDONLY, PARAM_FLAG_PERSIST|PARAM_FLAG_READONLY, false);

PARAM_TABLE(applet_params, &eu32.base, &erdo.base);
PARAM_MODULE_DEFINE(test_applet, TEST_MODULE_APPLET, "ErrApplet", NULL, NULL, NULL, mock_apply_ok, NULL, mock_flush_ok);

PARAM_IP_UINT  (eip, TID_IP_UINT, PARAM_FLAG_PERSIST, 500,   500,   500);
PARAM_TABLE(ip_params, &eip.base);
IP_DRIVER_DEFINE(test_ip, TEST_MODULE_IP, "ErrIP", NULL, NULL, NULL, mock_ip_write_ok, NULL, NULL);

static void register_all(void)
{
    param_module_register(&test_applet_module, applet_params, PARAM_COUNT(applet_params));
    ip_driver_register(&test_ip_instance, ip_params, PARAM_COUNT(ip_params));
    test_reset_entry(&eu32.base); test_reset_entry(&erdo.base);
    test_reset_entry(&eip.base);
}

/* ================================================================
 *  API 前置校验 (未 init 时调用)
 * ================================================================ */
void test_api_no_init_write_raw(void) {
    param_deinit();
    uint8_t d[4] = {0};
    TEST_ASSERT_PARAM_ERR(param_write_raw(TID_APPLET_UINT, d, 4), PARAM_ERR_NOT_FOUND);
}

void test_api_no_init_write_immediate(void) {
    param_deinit();
    param_value_t v = { .u32 = 1 };
    TEST_ASSERT_PARAM_ERR(param_write_immediate(TID_APPLET_UINT, v), PARAM_ERR_NOT_FOUND);
}

void test_api_no_init_flush(void) {
    param_deinit();
    TEST_ASSERT_PARAM_ERR(param_flush(), PARAM_ERR_NOT_FOUND);
}

void test_api_no_init_save_all(void) {
    param_deinit();
    TEST_ASSERT_PARAM_ERR(param_save_all(), PARAM_ERR_NOT_FOUND);
}

void test_api_no_init_save_one(void) {
    param_deinit();
    TEST_ASSERT_PARAM_ERR(param_save_one(TID_APPLET_UINT), PARAM_ERR_NOT_FOUND);
}

void test_api_no_init_load_all(void) {
    param_deinit();
    TEST_ASSERT_PARAM_ERR(param_load_all(), PARAM_ERR_NOT_FOUND);
}

void test_api_no_init_load_one(void) {
    param_deinit();
    TEST_ASSERT_PARAM_ERR(param_load_one(TID_APPLET_UINT), PARAM_ERR_NOT_FOUND);
}

void test_api_no_init_reset_all(void) {
    param_deinit();
    TEST_ASSERT_PARAM_ERR(param_reset_all(), PARAM_ERR_NOT_FOUND);
}

void test_api_no_init_reset_one(void) {
    param_deinit();
    TEST_ASSERT_PARAM_ERR(param_reset_one(TID_APPLET_UINT), PARAM_ERR_NOT_FOUND);
}

void test_api_no_init_exec(void) {
    param_deinit();
    TEST_ASSERT_PARAM_ERR(param_exec(MAKE_PARAM_ID(TEST_MODULE_APPLET, 0), NULL), PARAM_ERR_NOT_FOUND);
}

/* ================================================================
 *  模块 flush 错误
 * ================================================================ */
void test_flush_applet_cb_fails(void) {
    test_applet_module.flush = mock_flush_fail;
    register_all();

    param_value_t v = { .u32 = 50 };
    param_write(TID_APPLET_UINT, v);

    mock_flush_reset();
    int ret = param_flush();
    TEST_ASSERT_PARAM_ERR(ret, PARAM_ERR_FLASH_FAIL);

    param_stats_t stats;
    param_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT16(1, stats.flush_error_count);
}

void test_flush_ip_write_cb_fails(void) {
    test_ip_instance.write = mock_ip_write_fail;
    register_all();

    param_value_t v = { .u32 = 100 };
    param_write(TID_IP_UINT, v);

    mock_ip_reset();
    param_value_t write = { .u32 = 999 };
    param_write(TID_IP_UINT, write);

    int ret = param_flush();
    TEST_ASSERT_PARAM_ERR(ret, PARAM_ERR_NOT_FOUND);
}

/* ================================================================
 *  flush integrity check
 * ================================================================ */
void test_flush_integrity_all_covered(void) {
    PARAM_MODULE_DEFINE(tmod, MODULE_AUTO_EXP, "Tmod", NULL, NULL, NULL, NULL, NULL, NULL);
    DEF_TEST_UINT(tp, MAKE_PARAM_ID(MODULE_AUTO_EXP, 0), 0, 50, 0, 100);
    PARAM_TABLE(tparams, &tp.base);
    param_module_register(&tmod_module, tparams, PARAM_COUNT(tparams));
    TEST_ASSERT_PARAM_OK(param_check_flush_integrity());
}

void test_flush_integrity_unordered_module(void) {
    PARAM_MODULE_DEFINE(unord, TEST_MODULE_UNORDERED, "Unordered", NULL, NULL, NULL, NULL, NULL, NULL);
    DEF_TEST_UINT(uparam, TID_APPLET2_UINT, 0, 50, 0, 100);
    PARAM_TABLE(uparams, &uparam.base);

    param_module_register(&unord_module, uparams, PARAM_COUNT(uparams));

    TEST_ASSERT_PARAM_ERR(param_check_flush_integrity(), PARAM_ERR_NOT_FOUND);

    param_stats_t stats;
    param_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT16(1, stats.flush_order_miss_count);
}

/* ================================================================
 *  apply_cb 错误传播
 * ================================================================ */
void test_apply_cb_returns_error(void) {
    test_applet_module.apply = mock_apply_fail;
    register_all();

    param_value_t v = { .u32 = 42 };
    int ret = param_write(TID_APPLET_UINT, v);
    TEST_ASSERT_PARAM_ERR(ret, PARAM_ERR_INVALID_ID);

    param_read(TID_APPLET_UINT, &v);
    TEST_ASSERT_EQUAL_UINT32(100, v.u32);
    test_applet_module.apply = mock_apply_ok;
}

/* ================================================================
 *  统计完整性
 * ================================================================ */
void test_stats_module_param_counts(void) {
    register_all();

    param_stats_t stats;
    param_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT16(2, stats.module_count);
}

void test_stats_dirty_inc_dec(void) {
    register_all();

    param_value_t v = { .u32 = 1 };
    param_write(TID_APPLET_UINT, v);
    v.u32 = 2; param_write(TID_APPLET_UINT, v);
    v.u32 = 3; param_write(TID_APPLET_UINT, v);

    param_stats_t stats;
    param_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT16(1, stats.dirty_count);

    param_entry_clear_dirty(&eu32.base);
    param_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT16(0, stats.dirty_count);
}

void test_stats_dirty_dec_below_zero(void) {
    register_all();

    param_stats_t stats;
    param_get_stats(&stats);

    param_stats_dirty_dec();
    param_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT16(0, stats.dirty_count);
}

void test_stats_clear(void) {
    register_all();

    param_value_t v = { .u32 = 1 };
    param_write(TID_APPLET_UINT, v);

    param_clear_stats();

    param_stats_t stats;
    param_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT16(0, stats.dirty_count);
    TEST_ASSERT_EQUAL_UINT16(0, stats.flush_error_count);
    TEST_ASSERT_EQUAL_UINT16(0, stats.flush_order_miss_count);
}

void run_test_error_handling(void)
{
    RUN_TEST(test_api_no_init_write_raw);
    RUN_TEST(test_api_no_init_write_immediate);
    RUN_TEST(test_api_no_init_flush);
    RUN_TEST(test_api_no_init_save_all);
    RUN_TEST(test_api_no_init_save_one);
    RUN_TEST(test_api_no_init_load_all);
    RUN_TEST(test_api_no_init_load_one);
    RUN_TEST(test_api_no_init_reset_all);
    RUN_TEST(test_api_no_init_reset_one);
    RUN_TEST(test_api_no_init_exec);
    RUN_TEST(test_flush_applet_cb_fails);
    RUN_TEST(test_flush_ip_write_cb_fails);
    RUN_TEST(test_flush_integrity_all_covered);
    RUN_TEST(test_flush_integrity_unordered_module);
    RUN_TEST(test_apply_cb_returns_error);
    RUN_TEST(test_stats_module_param_counts);
    RUN_TEST(test_stats_dirty_inc_dec);
    RUN_TEST(test_stats_dirty_dec_below_zero);
    RUN_TEST(test_stats_clear);
}
