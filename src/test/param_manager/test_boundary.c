#include "test_common.h"
#include <string.h>

DEF_TEST_UINT  (bu32, TID_APPLET_UINT, PARAM_FLAG_PERSIST, 100, 10, 190);
DEF_TEST_INT   (bi32, TID_APPLET_INT,  PARAM_FLAG_PERSIST, 0, -100, 100);
DEF_TEST_FLOAT (bf32, TID_APPLET_FLOAT,PARAM_FLAG_PERSIST, 1.5f, 0.0f, 3.0f);
static const int32_t g_benm_vals[] = {10, 20, 30};
PARAM_ENUM     (benm, TID_APPLET_ENUM, PARAM_FLAG_PERSIST, 10, g_benm_vals, 3);

PARAM_IP_UINT  (ipu,  TID_IP_UINT,  PARAM_FLAG_PERSIST, 500,   500,   500);
PARAM_IP_FLOAT (ipf,  TID_IP_FLOAT, PARAM_FLAG_PERSIST, 2.0f,  2.0f,  2.0f);
PARAM_IP_BOOL  (ipb,  TID_IP_BOOL,  PARAM_FLAG_PERSIST, false);

PARAM_TABLE(applet_params, &bu32.base, &bi32.base, &bf32.base, &benm.base);
PARAM_TABLE(ip_params, &ipu.base, &ipf.base, &ipb.base);

PARAM_MODULE_DEFINE(test_applet, TEST_MODULE_APPLET, "BoundApplet", NULL, NULL, NULL, mock_apply_ok, NULL, NULL);
IP_DRIVER_DEFINE(test_ip, TEST_MODULE_IP, "BoundIP", NULL, NULL, NULL, mock_ip_write_ok, NULL, NULL);

static void register_all(void)
{
    param_module_register(&test_applet_module, applet_params, PARAM_COUNT(applet_params));
    ip_driver_register(&test_ip_instance, ip_params, PARAM_COUNT(ip_params));
    test_reset_entry(&bu32.base); test_reset_entry(&bi32.base);
    test_reset_entry(&bf32.base); test_reset_entry(&benm.base);
    test_reset_entry(&ipu.base); test_reset_entry(&ipf.base);
    test_reset_entry(&ipb.base);
}

/* ================================================================
 *  范围裁剪
 * ================================================================ */
void test_range_uint_at_min(void) {
    register_all();
    param_value_t v = { .u32 = 10 };
    TEST_ASSERT_PARAM_OK(param_write(TID_APPLET_UINT, v));
    param_read(TID_APPLET_UINT, &v);
    TEST_ASSERT_EQUAL_UINT32(10, v.u32);
}

void test_range_uint_at_max(void) {
    register_all();
    param_value_t v = { .u32 = 190 };
    TEST_ASSERT_PARAM_OK(param_write(TID_APPLET_UINT, v));
    param_read(TID_APPLET_UINT, &v);
    TEST_ASSERT_EQUAL_UINT32(190, v.u32);
}

void test_range_uint_below_min(void) {
    register_all();
    param_value_t v = { .u32 = 5 };
    TEST_ASSERT_PARAM_OK(param_write(TID_APPLET_UINT, v));
    param_read(TID_APPLET_UINT, &v);
    TEST_ASSERT_EQUAL_UINT32(10, v.u32);
}

void test_range_uint_above_max(void) {
    register_all();
    param_value_t v = { .u32 = 250 };
    TEST_ASSERT_PARAM_OK(param_write(TID_APPLET_UINT, v));
    param_read(TID_APPLET_UINT, &v);
    TEST_ASSERT_EQUAL_UINT32(190, v.u32);
}

void test_range_int_below_min(void) {
    register_all();
    param_value_t v = { .i32 = -999 };
    TEST_ASSERT_PARAM_OK(param_write(TID_APPLET_INT, v));
    param_read(TID_APPLET_INT, &v);
    TEST_ASSERT_EQUAL_INT32(-100, v.i32);
}

void test_range_int_above_max(void) {
    register_all();
    param_value_t v = { .i32 = 999 };
    TEST_ASSERT_PARAM_OK(param_write(TID_APPLET_INT, v));
    param_read(TID_APPLET_INT, &v);
    TEST_ASSERT_EQUAL_INT32(100, v.i32);
}

void test_range_float_below_min(void) {
    register_all();
    param_value_t v = { .f32 = -1.0f };
    TEST_ASSERT_PARAM_OK(param_write(TID_APPLET_FLOAT, v));
    param_read(TID_APPLET_FLOAT, &v);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, v.f32);
}

void test_range_float_above_max(void) {
    register_all();
    param_value_t v = { .f32 = 9.0f };
    TEST_ASSERT_PARAM_OK(param_write(TID_APPLET_FLOAT, v));
    param_read(TID_APPLET_FLOAT, &v);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 3.0f, v.f32);
}

/* ================================================================
 *  枚举边界
 * ================================================================ */
void test_enum_valid_first(void) {
    register_all();
    param_value_t v = { .i32 = 10 };
    TEST_ASSERT_PARAM_OK(param_write(TID_APPLET_ENUM, v));
}

void test_enum_valid_last(void) {
    register_all();
    param_value_t v = { .i32 = 30 };
    TEST_ASSERT_PARAM_OK(param_write(TID_APPLET_ENUM, v));
}

void test_enum_invalid_before(void) {
    register_all();
    param_value_t v = { .i32 = 5 };
    TEST_ASSERT_PARAM_ERR(param_write(TID_APPLET_ENUM, v), PARAM_ERR_OUT_OF_RANGE);
}

void test_enum_invalid_after(void) {
    register_all();
    param_value_t v = { .i32 = 99 };
    TEST_ASSERT_PARAM_ERR(param_write(TID_APPLET_ENUM, v), PARAM_ERR_OUT_OF_RANGE);
}

void test_enum_invalid_between(void) {
    register_all();
    param_value_t v = { .i32 = 15 };
    TEST_ASSERT_PARAM_ERR(param_write(TID_APPLET_ENUM, v), PARAM_ERR_OUT_OF_RANGE);
}

/* ================================================================
 *  模块注册边界
 * ================================================================ */
void test_register_same_module_twice(void) {
    register_all();
    TEST_ASSERT_PARAM_ERR(
        param_module_register(&test_applet_module, applet_params, PARAM_COUNT(applet_params)),
        PARAM_ERR_ALREADY_REG);
}

void test_register_null_module(void) {
    TEST_ASSERT_PARAM_ERR(param_module_register(NULL, applet_params, PARAM_COUNT(applet_params)),
                          PARAM_ERR_INVALID_ID);
}

void test_register_null_entries(void) {
    TEST_ASSERT_PARAM_ERR(param_module_register(&test_applet_module, NULL, 4),
                          PARAM_ERR_INVALID_ID);
}

void test_register_zero_count(void) {
    TEST_ASSERT_PARAM_ERR(param_module_register(&test_applet_module, applet_params, 0),
                          PARAM_ERR_INVALID_ID);
}

void test_register_duplicate_param_across_modules(void) {
    param_module_register(&test_applet_module, applet_params, PARAM_COUNT(applet_params));

    PARAM_MODULE_DEFINE(test2, TEST_MODULE_APPLET2, "Applet2", NULL, NULL, NULL, NULL, NULL, NULL);
    PARAM_TABLE(dup_params, &bu32.base);
    TEST_ASSERT_PARAM_ERR(
        param_module_register(&test2_module, dup_params, PARAM_COUNT(dup_params)),
        PARAM_ERR_ALREADY_REG);
}

/* ================================================================
 *  IP 位图边界
 * ================================================================ */
void test_ip_max_64_params(void) {
    static param_entry_head_t ip_arr[64];
    static param_entry_t *ip_table[64];
    for (int i = 0; i < 64; i++) {
        memset(&ip_arr[i], 0, sizeof(ip_arr[i]));
        ip_arr[i].base.param_id = MAKE_PARAM_ID(TEST_MODULE_IP2, i);
        ip_arr[i].base.vtable   = &ip_vtable;
        ip_arr[i].type          = PARAM_TYPE_UINT;
        ip_arr[i].cache.u32     = i;
        ip_arr[i].default_val.u32 = i;
        ip_table[i] = &ip_arr[i].base;
    }
    IP_DRIVER_DEFINE(big_ip, TEST_MODULE_IP2, "BigIP", NULL, NULL, NULL, NULL, NULL, NULL);
    TEST_ASSERT_PARAM_OK(ip_driver_register(&big_ip_instance, ip_table, 64));
}

void test_ip_too_many_params(void) {
    static param_entry_head_t ip_arr[65];
    static param_entry_t *ip_table[65];
    for (int i = 0; i < 65; i++) {
        memset(&ip_arr[i], 0, sizeof(ip_arr[i]));
        ip_arr[i].base.param_id = MAKE_PARAM_ID(TEST_MODULE_IP2, i);
        ip_arr[i].base.vtable   = &ip_vtable;
        ip_arr[i].cache.u32     = i;
        ip_arr[i].default_val.u32 = i;
        ip_table[i] = &ip_arr[i].base;
    }
    IP_DRIVER_DEFINE(big_ip, TEST_MODULE_IP2, "BigIP", NULL, NULL, NULL, NULL, NULL, NULL);
    TEST_ASSERT_PARAM_ERR(ip_driver_register(&big_ip_instance, ip_table, 65),
                          PARAM_ERR_OUT_OF_RANGE);
}

void run_test_boundary(void)
{
    RUN_TEST(test_range_uint_at_min);
    RUN_TEST(test_range_uint_at_max);
    RUN_TEST(test_range_uint_below_min);
    RUN_TEST(test_range_uint_above_max);
    RUN_TEST(test_range_int_below_min);
    RUN_TEST(test_range_int_above_max);
    RUN_TEST(test_range_float_below_min);
    RUN_TEST(test_range_float_above_max);
    RUN_TEST(test_enum_valid_first);
    RUN_TEST(test_enum_valid_last);
    RUN_TEST(test_enum_invalid_before);
    RUN_TEST(test_enum_invalid_after);
    RUN_TEST(test_enum_invalid_between);
    RUN_TEST(test_register_same_module_twice);
    RUN_TEST(test_register_null_module);
    RUN_TEST(test_register_null_entries);
    RUN_TEST(test_register_zero_count);
    RUN_TEST(test_register_duplicate_param_across_modules);
    RUN_TEST(test_ip_max_64_params);
    RUN_TEST(test_ip_too_many_params);
}
