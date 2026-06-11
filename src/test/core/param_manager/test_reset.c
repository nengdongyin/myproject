#include "test_common.h"
#include <string.h>

DEF_TEST_UINT  (ru32, TID_APPLET_UINT,   PARAM_FLAG_PERSIST, 100, 0, 200);
PARAM_BOOL     (rboo, TID_APPLET_BOOL,   PARAM_FLAG_PERSIST, true);
PARAM_BOOL     (rrdo, TID_APPLET_RDONLY, PARAM_FLAG_PERSIST|PARAM_FLAG_READONLY, false);

PARAM_IP_UINT  (ripu, TID_IP_UINT,  PARAM_FLAG_PERSIST, 500,   500,   500);
PARAM_IP_FLOAT (ripf, TID_IP_FLOAT, PARAM_FLAG_PERSIST, 2.0f,  2.0f,  2.0f);

static uint8_t g_blob_buf2[TEST_BLOB_SIZE];
PARAM_BLOB     (rblb, TID_APPLET_BLOB, PARAM_FLAG_PERSIST, g_blob_buf2, TEST_BLOB_SIZE);

PARAM_TABLE(applet_params, &ru32.base, &rboo.base, &rrdo.base, &rblb.base);
PARAM_TABLE(ip_params, &ripu.base, &ripf.base);

PARAM_MODULE_DEFINE(test_applet, TEST_MODULE_APPLET, "ResetApplet", NULL, NULL, NULL, mock_apply_ok, NULL, NULL);
IP_DRIVER_DEFINE(test_ip, TEST_MODULE_IP, "ResetIP", NULL, NULL, NULL, mock_ip_write_ok, NULL, NULL);

static void register_all(void)
{
    param_module_register(&test_applet_module, applet_params, PARAM_COUNT(applet_params));
    ip_driver_register(&test_ip_instance, ip_params, PARAM_COUNT(ip_params));
    test_reset_entry(&ru32.base); test_reset_entry(&rboo.base);
    test_reset_entry(&rrdo.base); test_reset_entry(&rblb.base);
    test_reset_entry(&ripu.base); test_reset_entry(&ripf.base);
}

/* ================================================================
 *  reset_one 测试
 * ================================================================ */
void test_reset_one_uint(void) {
    register_all();

    param_value_t v = { .u32 = 999 };
    param_write(TID_APPLET_UINT, v);

    TEST_ASSERT_PARAM_OK(param_reset_one(TID_APPLET_UINT));

    param_read(TID_APPLET_UINT, &v);
    TEST_ASSERT_EQUAL_UINT32(100, v.u32);
    TEST_ASSERT_DIRTY(&ru32.base, 1);
}

void test_reset_one_bool(void) {
    register_all();

    param_value_t v = { .b = false };
    param_write(TID_APPLET_BOOL, v);

    TEST_ASSERT_PARAM_OK(param_reset_one(TID_APPLET_BOOL));

    param_read(TID_APPLET_BOOL, &v);
    TEST_ASSERT_TRUE(v.b);
}

void test_reset_one_ip(void) {
    register_all();

    param_value_t v = { .u32 = 999 };
    param_write(TID_IP_UINT, v);

    TEST_ASSERT_PARAM_OK(param_reset_one(TID_IP_UINT));

    param_read(TID_IP_UINT, &v);
    TEST_ASSERT_EQUAL_UINT32(500, v.u32);
    TEST_ASSERT_DIRTY(&ripu.base, 1);
}

void test_reset_one_float(void) {
    register_all();

    param_value_t v = { .f32 = 5.0f };
    param_write(TID_APPLET_UINT, v);

    TEST_ASSERT_PARAM_OK(param_reset_one(TID_APPLET_UINT));

    param_read(TID_APPLET_UINT, &v);
    TEST_ASSERT_EQUAL_UINT32(100, v.u32);
}

void test_reset_one_not_found(void) {
    register_all();
    TEST_ASSERT_PARAM_ERR(param_reset_one(0xDEADBEEFu), PARAM_ERR_INVALID_ID);
}

/* ================================================================
 *  reset_all 测试
 * ================================================================ */
void test_reset_all_clears_all_dirty(void) {
    register_all();

    param_value_t v = { .u32 = 1 };
    param_write(TID_APPLET_UINT, v);
    param_write(TID_IP_UINT, v);

    TEST_ASSERT_PARAM_OK(param_reset_all());

    param_stats_t stats;
    param_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT16(0, stats.dirty_count);

    param_read(TID_APPLET_UINT, &v);
    TEST_ASSERT_EQUAL_UINT32(100, v.u32);
    param_read(TID_IP_UINT, &v);
    TEST_ASSERT_EQUAL_UINT32(500, v.u32);
}

void test_reset_all_ip_dirty_map_cleared(void) {
    register_all();

    param_value_t v = { .f32 = 5.0f };
    param_write(TID_IP_FLOAT, v);

    TEST_ASSERT_PARAM_OK(param_reset_all());

    param_read(TID_IP_FLOAT, &v);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 2.0f, v.f32);
}

void test_reset_one_clears_ip_dirty_map(void) {
    register_all();

    param_value_t v = { .u32 = 999 };
    param_write(TID_IP_UINT, v);

    TEST_ASSERT_PARAM_OK(param_reset_one(TID_IP_UINT));

    param_stats_t stats;
    param_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT16(1, stats.dirty_count);
}

/* ================================================================
 *  BLOB reset
 * ================================================================ */
void test_reset_blob(void) {
    register_all();

    uint8_t new_data[TEST_BLOB_SIZE];
    memset(new_data, 0xCC, TEST_BLOB_SIZE);
    param_value_t v = { .ptr = new_data };
    param_write(TID_APPLET_BLOB, v);

    TEST_ASSERT_PARAM_OK(param_reset_one(TID_APPLET_BLOB));
}

void run_test_reset(void)
{
    RUN_TEST(test_reset_one_uint);
    RUN_TEST(test_reset_one_bool);
    RUN_TEST(test_reset_one_ip);
    RUN_TEST(test_reset_one_float);
    RUN_TEST(test_reset_one_not_found);
    RUN_TEST(test_reset_all_clears_all_dirty);
    RUN_TEST(test_reset_all_ip_dirty_map_cleared);
    RUN_TEST(test_reset_one_clears_ip_dirty_map);
    RUN_TEST(test_reset_blob);
}
