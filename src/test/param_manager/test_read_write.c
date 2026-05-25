#include "test_common.h"
#include <string.h>

DEF_TEST_UINT  (tu32, TID_APPLET_UINT,    PARAM_FLAG_PERSIST, 100, 0, 200);
DEF_TEST_INT   (ti32, TID_APPLET_INT,     PARAM_FLAG_PERSIST, -10, -50, 50);
DEF_TEST_FLOAT (tf32, TID_APPLET_FLOAT,   PARAM_FLAG_PERSIST, 1.5f, 0.0f, 3.0f);
PARAM_BOOL     (tboo, TID_APPLET_BOOL,    PARAM_FLAG_PERSIST, true);
static const int32_t g_tenm_vals[] = {0, 1, 2};
PARAM_ENUM     (tenm, TID_APPLET_ENUM,    PARAM_FLAG_PERSIST, 1, g_tenm_vals, 3);
PARAM_BOOL     (trdo, TID_APPLET_RDONLY,  PARAM_FLAG_PERSIST|PARAM_FLAG_READONLY, false);
PARAM_UINT     (tnop, TID_APPLET_NOPERSIST, 0, 50, 0, 100);

PARAM_TABLE(applet_params,
    &tu32.base, &ti32.base, &tf32.base, &tboo.base, &tenm.base,
    &trdo.base, &tnop.base
);

PARAM_MODULE_DEFINE(test_applet, TEST_MODULE_APPLET, "TestApplet",
                    NULL, NULL, mock_apply_ok, NULL, NULL);

static void register_params(void)
{
    param_module_register(&test_applet_module, applet_params, PARAM_COUNT(applet_params));
    test_reset_entry(&tu32.base);
    test_reset_entry(&ti32.base);
    test_reset_entry(&tf32.base);
    test_reset_entry(&tboo.base);
    test_reset_entry(&tenm.base);
    test_reset_entry(&trdo.base);
    test_reset_entry(&tnop.base);
}

/* ================================================================
 *  基础读测试
 * ================================================================ */
void test_read_uint_default(void) {
    register_params();
    param_value_t v;
    TEST_ASSERT_PARAM_OK(param_read(TID_APPLET_UINT, &v));
    TEST_ASSERT_EQUAL_UINT32(100, v.u32);
}

void test_read_int_default(void) {
    register_params();
    param_value_t v;
    TEST_ASSERT_PARAM_OK(param_read(TID_APPLET_INT, &v));
    TEST_ASSERT_EQUAL_INT32(-10, v.i32);
}

void test_read_float_default(void) {
    register_params();
    param_value_t v;
    TEST_ASSERT_PARAM_OK(param_read(TID_APPLET_FLOAT, &v));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.5f, v.f32);
}

void test_read_bool_default(void) {
    register_params();
    param_value_t v;
    TEST_ASSERT_PARAM_OK(param_read(TID_APPLET_BOOL, &v));
    TEST_ASSERT_TRUE(v.b);
}

void test_read_enum_default(void) {
    register_params();
    param_value_t v;
    TEST_ASSERT_PARAM_OK(param_read(TID_APPLET_ENUM, &v));
    TEST_ASSERT_EQUAL_INT32(1, v.i32);
}

void test_read_non_existent(void) {
    register_params();
    param_value_t v;
    TEST_ASSERT_PARAM_ERR(param_read(0xDEADBEEFu, &v), PARAM_ERR_INVALID_ID);
}

void test_read_null_value_ptr(void) {
    register_params();
    TEST_ASSERT_PARAM_ERR(param_read(TID_APPLET_UINT, NULL), PARAM_ERR_INVALID_ID);
}

void test_read_not_initialized(void) {
    param_deinit();
    param_value_t v;
    TEST_ASSERT_PARAM_ERR(param_read(TID_APPLET_UINT, &v), PARAM_ERR_NOT_FOUND);
}

/* ================================================================
 *  Read 类型化包装
 * ================================================================ */
void test_read_u32_wrapper(void) {
    register_params();
    uint32_t val = 0;
    TEST_ASSERT_PARAM_OK(param_read_u32(TID_APPLET_UINT, &val));
    TEST_ASSERT_EQUAL_UINT32(100, val);
}

void test_read_u32_null_val(void) {
    register_params();
    TEST_ASSERT_PARAM_OK(param_read_u32(TID_APPLET_UINT, NULL));
}

void test_read_i32_wrapper(void) {
    register_params();
    int32_t val = 0;
    TEST_ASSERT_PARAM_OK(param_read_i32(TID_APPLET_INT, &val));
    TEST_ASSERT_EQUAL_INT32(-10, val);
}

void test_read_f32_wrapper(void) {
    register_params();
    float val = 0.0f;
    TEST_ASSERT_PARAM_OK(param_read_f32(TID_APPLET_FLOAT, &val));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.5f, val);
}

void test_read_bool_wrapper(void) {
    register_params();
    bool val = false;
    TEST_ASSERT_PARAM_OK(param_read_bool(TID_APPLET_BOOL, &val));
    TEST_ASSERT_TRUE(val);
}

/* ================================================================
 *  基础写测试
 * ================================================================ */
void test_write_uint_valid(void) {
    register_params();
    param_value_t v = { .u32 = 150 };
    TEST_ASSERT_PARAM_OK(param_write(TID_APPLET_UINT, v));
    g_apply_last_ret = -99;
    param_read(TID_APPLET_UINT, &v);
    TEST_ASSERT_EQUAL_UINT32(150, v.u32);
    TEST_ASSERT_DIRTY(&tu32.base, 1);
}

void test_write_int_valid(void) {
    register_params();
    param_value_t v = { .i32 = 30 };
    TEST_ASSERT_PARAM_OK(param_write(TID_APPLET_INT, v));
    param_read(TID_APPLET_INT, &v);
    TEST_ASSERT_EQUAL_INT32(30, v.i32);
    TEST_ASSERT_DIRTY(&ti32.base, 1);
}

void test_write_float_valid(void) {
    register_params();
    param_value_t v = { .f32 = 2.5f };
    TEST_ASSERT_PARAM_OK(param_write(TID_APPLET_FLOAT, v));
    param_read(TID_APPLET_FLOAT, &v);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 2.5f, v.f32);
    TEST_ASSERT_DIRTY(&tf32.base, 1);
}

void test_write_bool_valid(void) {
    register_params();
    param_value_t v = { .b = false };
    TEST_ASSERT_PARAM_OK(param_write(TID_APPLET_BOOL, v));
    param_read(TID_APPLET_BOOL, &v);
    TEST_ASSERT_FALSE(v.b);
    TEST_ASSERT_DIRTY(&tboo.base, 1);
}

void test_write_triggers_apply_cb(void) {
    register_params();
    param_value_t v = { .u32 = 42 };
    mock_apply_reset();
    TEST_ASSERT_PARAM_OK(param_write(TID_APPLET_UINT, v));
    TEST_ASSERT_EQUAL_INT(1, g_apply_call_count);
    TEST_ASSERT_EQUAL_UINT32(TID_APPLET_UINT, g_apply_last_id);
    TEST_ASSERT_EQUAL_UINT32(42, g_apply_last_value.u32);
}

void test_write_apply_rejects(void) {
    test_applet_module.apply = mock_apply_fail;
    register_params();
    param_value_t v = { .u32 = 42 };
    TEST_ASSERT_PARAM_ERR(param_write(TID_APPLET_UINT, v), PARAM_ERR_INVALID_ID);
    param_read(TID_APPLET_UINT, &v);
    TEST_ASSERT_EQUAL_UINT32(100, v.u32);
    TEST_ASSERT_DIRTY(&tu32.base, 0);
    test_applet_module.apply = mock_apply_ok;
}

void test_write_readonly(void) {
    register_params();
    param_value_t v = { .b = true };
    TEST_ASSERT_PARAM_ERR(param_write(TID_APPLET_RDONLY, v), PARAM_ERR_READONLY);
}

void test_write_non_existent(void) {
    register_params();
    param_value_t v = { .u32 = 1 };
    TEST_ASSERT_PARAM_ERR(param_write(0xDEADBEEFu, v), PARAM_ERR_INVALID_ID);
}

void test_write_not_initialized(void) {
    param_deinit();
    param_value_t v = { .u32 = 1 };
    TEST_ASSERT_PARAM_ERR(param_write(TID_APPLET_UINT, v), PARAM_ERR_NOT_FOUND);
}

/* ================================================================
 *  Write 类型化包装
 * ================================================================ */
void test_write_u32_wrapper(void) {
    register_params();
    TEST_ASSERT_PARAM_OK(param_write_u32(TID_APPLET_UINT, 77));
    param_value_t v;
    param_read(TID_APPLET_UINT, &v);
    TEST_ASSERT_EQUAL_UINT32(77, v.u32);
}

void test_write_i32_wrapper(void) {
    register_params();
    TEST_ASSERT_PARAM_OK(param_write_i32(TID_APPLET_INT, -20));
    param_value_t v;
    param_read(TID_APPLET_INT, &v);
    TEST_ASSERT_EQUAL_INT32(-20, v.i32);
}

void test_write_f32_wrapper(void) {
    register_params();
    TEST_ASSERT_PARAM_OK(param_write_f32(TID_APPLET_FLOAT, 2.25f));
    param_value_t v;
    param_read(TID_APPLET_FLOAT, &v);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 2.25f, v.f32);
}

void test_write_bool_wrapper(void) {
    register_params();
    TEST_ASSERT_PARAM_OK(param_write_bool(TID_APPLET_BOOL, false));
    param_value_t v;
    param_read(TID_APPLET_BOOL, &v);
    TEST_ASSERT_FALSE(v.b);
}

/* ================================================================
 *  枚举写测试
 * ================================================================ */
void test_write_enum_valid(void) {
    register_params();
    param_value_t v = { .i32 = 2 };
    TEST_ASSERT_PARAM_OK(param_write(TID_APPLET_ENUM, v));
    param_read(TID_APPLET_ENUM, &v);
    TEST_ASSERT_EQUAL_INT32(2, v.i32);
}

void test_write_enum_invalid(void) {
    register_params();
    param_value_t v = { .i32 = 99 };
    TEST_ASSERT_PARAM_ERR(param_write(TID_APPLET_ENUM, v), PARAM_ERR_OUT_OF_RANGE);
}

/* ================================================================
 *  persist 标志跳过写
 * ================================================================ */
void test_write_no_persist_still_applies_dirty(void) {
    register_params();
    param_value_t v = { .u32 = 60 };
    TEST_ASSERT_PARAM_OK(param_write(TID_APPLET_NOPERSIST, v));
    param_read(TID_APPLET_NOPERSIST, &v);
    TEST_ASSERT_EQUAL_UINT32(60, v.u32);
}

void run_test_read_write(void)
{
    RUN_TEST(test_read_uint_default);
    RUN_TEST(test_read_int_default);
    RUN_TEST(test_read_float_default);
    RUN_TEST(test_read_bool_default);
    RUN_TEST(test_read_enum_default);
    RUN_TEST(test_read_non_existent);
    RUN_TEST(test_read_null_value_ptr);
    RUN_TEST(test_read_not_initialized);
    RUN_TEST(test_read_u32_wrapper);
    RUN_TEST(test_read_u32_null_val);
    RUN_TEST(test_read_i32_wrapper);
    RUN_TEST(test_read_f32_wrapper);
    RUN_TEST(test_read_bool_wrapper);
    RUN_TEST(test_write_uint_valid);
    RUN_TEST(test_write_int_valid);
    RUN_TEST(test_write_float_valid);
    RUN_TEST(test_write_bool_valid);
    RUN_TEST(test_write_triggers_apply_cb);
    RUN_TEST(test_write_apply_rejects);
    RUN_TEST(test_write_readonly);
    RUN_TEST(test_write_non_existent);
    RUN_TEST(test_write_not_initialized);
    RUN_TEST(test_write_u32_wrapper);
    RUN_TEST(test_write_i32_wrapper);
    RUN_TEST(test_write_f32_wrapper);
    RUN_TEST(test_write_bool_wrapper);
    RUN_TEST(test_write_enum_valid);
    RUN_TEST(test_write_enum_invalid);
    RUN_TEST(test_write_no_persist_still_applies_dirty);
}
