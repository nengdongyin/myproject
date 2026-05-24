#include "test_common.h"
#include <string.h>

DEF_TEST_UINT  (lu32,  TID_APPLET_UINT,    PARAM_FLAG_PERSIST, 100, 0, 200);
DEF_TEST_FLOAT (lf32,  TID_APPLET_FLOAT,   PARAM_FLAG_PERSIST, 1.5f, 0.0f, 3.0f);
PARAM_BOOL     (lboo,  TID_APPLET_BOOL,    PARAM_FLAG_PERSIST, true);
PARAM_UINT     (lnop,  TID_APPLET_NOPERSIST, 0, 50, 0, 100);

PARAM_IP_UINT  (lipu,  TID_IP_UINT,  PARAM_FLAG_PERSIST, 500,   500,   500);
PARAM_IP_FLOAT (lipf,  TID_IP_FLOAT, PARAM_FLAG_PERSIST, 2.0f,  2.0f,  2.0f);

static uint8_t g_blob_data[TEST_BLOB_SIZE];
PARAM_BLOB     (lblb,  TID_APPLET_BLOB, PARAM_FLAG_PERSIST, g_blob_data, TEST_BLOB_SIZE);

PARAM_TABLE(applet_params, &lu32.base, &lf32.base, &lboo.base, &lnop.base, &lblb.base);
PARAM_TABLE(ip_params, &lipu.base, &lipf.base);
PARAM_MODULE_DEFINE(test_applet, TEST_MODULE_APPLET, "LSApplet", NULL, mock_apply_ok);
IP_DRIVER_DEFINE(test_ip, TEST_MODULE_IP, "LSIP", NULL, NULL, mock_ip_write_ok, NULL);

static void register_all(void)
{
    param_module_register(&test_applet_module, applet_params, PARAM_COUNT(applet_params));
    ip_driver_register(&test_ip_instance, ip_params, PARAM_COUNT(ip_params));
    test_reset_entry(&lu32.base);  test_reset_entry(&lf32.base);
    test_reset_entry(&lboo.base);  test_reset_entry(&lnop.base);
    test_reset_entry(&lblb.base);
    test_reset_entry(&lipu.base);  test_reset_entry(&lipf.base);
}

/* ================================================================
 *  Load 测试
 * ================================================================ */
void test_load_one_valid(void) {
    register_all();

    param_value_t preset = { .u32 = 250 };
    mock_storage_preset(TID_APPLET_UINT, (const uint8_t *)&preset, sizeof(preset));

    TEST_ASSERT_PARAM_OK(param_load_one(TID_APPLET_UINT));

    param_value_t v;
    param_read(TID_APPLET_UINT, &v);
    TEST_ASSERT_EQUAL_UINT32(200, v.u32);
}

void test_load_one_no_persist(void) {
    register_all();

    param_value_t preset = { .u32 = 99 };
    mock_storage_preset(TID_APPLET_NOPERSIST, (const uint8_t *)&preset, sizeof(preset));

    TEST_ASSERT_PARAM_OK(param_load_one(TID_APPLET_NOPERSIST));

    param_value_t v;
    param_read(TID_APPLET_NOPERSIST, &v);
    TEST_ASSERT_EQUAL_UINT32(50, v.u32);
}

void test_load_one_not_found(void) {
    register_all();
    TEST_ASSERT_PARAM_ERR(param_load_one(0xDEADBEEFu), PARAM_ERR_INVALID_ID);
}

void test_load_float(void) {
    register_all();

    param_value_t preset = { .f32 = 2.75f };
    mock_storage_preset(TID_APPLET_FLOAT, (const uint8_t *)&preset, sizeof(preset));

    TEST_ASSERT_PARAM_OK(param_load_one(TID_APPLET_FLOAT));

    param_value_t v;
    param_read(TID_APPLET_FLOAT, &v);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 2.75f, v.f32);
}

void test_load_bool(void) {
    register_all();

    param_value_t preset = { .b = false };
    mock_storage_preset(TID_APPLET_BOOL, (const uint8_t *)&preset, sizeof(preset));

    TEST_ASSERT_PARAM_OK(param_load_one(TID_APPLET_BOOL));

    param_value_t v;
    param_read(TID_APPLET_BOOL, &v);
    TEST_ASSERT_FALSE(v.b);
}

void test_load_ip_param(void) {
    register_all();

    param_value_t preset = { .u32 = 888 };
    mock_storage_preset(TID_IP_UINT, (const uint8_t *)&preset, sizeof(preset));

    TEST_ASSERT_PARAM_OK(param_load_one(TID_IP_UINT));

    param_value_t v;
    param_read(TID_IP_UINT, &v);
    TEST_ASSERT_EQUAL_UINT32(888, v.u32);
}

void test_load_storage_fails(void) {
    register_all();
    mock_storage_set_load_ret(PARAM_ERR_FLASH_FAIL);

    TEST_ASSERT_PARAM_ERR(param_load_one(TID_APPLET_UINT), PARAM_ERR_FLASH_FAIL);

    param_value_t v;
    param_read(TID_APPLET_UINT, &v);
    TEST_ASSERT_EQUAL_UINT32(100, v.u32);
}

void test_load_all_multiple(void) {
    register_all();

    param_value_t v_u32 = { .u32 = 111 };
    param_value_t v_f32 = { .f32 = 2.25f };
    param_value_t v_bool = { .b = false };
    param_value_t v_ip = { .u32 = 999 };

    mock_storage_preset(TID_APPLET_UINT,  (const uint8_t *)&v_u32, sizeof(v_u32));
    mock_storage_preset(TID_APPLET_FLOAT, (const uint8_t *)&v_f32, sizeof(v_f32));
    mock_storage_preset(TID_APPLET_BOOL,  (const uint8_t *)&v_bool, sizeof(v_bool));
    mock_storage_preset(TID_IP_UINT,       (const uint8_t *)&v_ip, sizeof(v_ip));

    param_load_all();

    param_value_t v;
    param_read(TID_APPLET_UINT, &v);
    TEST_ASSERT_EQUAL_UINT32(111, v.u32);
    param_read(TID_APPLET_FLOAT, &v);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 2.25f, v.f32);
    param_read(TID_APPLET_BOOL, &v);
    TEST_ASSERT_FALSE(v.b);
    param_read(TID_IP_UINT, &v);
    TEST_ASSERT_EQUAL_UINT32(999, v.u32);
}

void test_load_all_partial_flash(void) {
    register_all();

    param_value_t preset = { .u32 = 77 };
    mock_storage_preset(TID_APPLET_UINT, (const uint8_t *)&preset, sizeof(preset));

    param_load_all();

    param_value_t v;
    param_read(TID_APPLET_UINT, &v);
    TEST_ASSERT_EQUAL_UINT32(77, v.u32);

    param_read(TID_APPLET_BOOL, &v);
    TEST_ASSERT_TRUE(v.b);
}

/* ================================================================
 *  Save 测试
 * ================================================================ */
void test_save_one_writes_storage(void) {
    register_all();

    param_value_t v = { .u32 = 42 };
    param_write(TID_APPLET_UINT, v);

    mock_storage_set_save_ret(0);
    TEST_ASSERT_PARAM_OK(param_save_one(TID_APPLET_UINT));
}

void test_save_all_skips_non_persist(void) {
    register_all();

    param_value_t v = { .u32 = 99 };
    param_write(TID_APPLET_NOPERSIST, v);

    TEST_ASSERT_PARAM_OK(param_save_all());
}

void test_save_float(void) {
    register_all();

    param_value_t v = { .f32 = 2.0f };
    param_write(TID_APPLET_FLOAT, v);

    TEST_ASSERT_PARAM_OK(param_save_one(TID_APPLET_FLOAT));
}

void test_save_bool(void) {
    register_all();

    param_value_t v = { .b = false };
    param_write(TID_APPLET_BOOL, v);

    TEST_ASSERT_PARAM_OK(param_save_one(TID_APPLET_BOOL));
}

void test_save_blob(void) {
    register_all();

    uint8_t data[TEST_BLOB_SIZE];
    memset(data, 0xAB, TEST_BLOB_SIZE);
    param_value_t v = { .ptr = data };
    param_write(TID_APPLET_BLOB, v);

    TEST_ASSERT_PARAM_OK(param_save_one(TID_APPLET_BLOB));
}

/* ================================================================
 *  Load/Save 往返
 * ================================================================ */
void test_save_load_roundtrip_uint(void) {
    register_all();

    param_value_t write_val = { .u32 = 123 };
    param_write(TID_APPLET_UINT, write_val);
    param_save_one(TID_APPLET_UINT);

    param_deinit();
    mock_callbacks_reset();
    param_init(&g_mock_storage, NULL);
    register_all();

    param_load_all();

    param_value_t read_val;
    param_read(TID_APPLET_UINT, &read_val);
    TEST_ASSERT_EQUAL_UINT32(123, read_val.u32);
}

void test_save_load_roundtrip_float(void) {
    register_all();

    param_value_t write_val = { .f32 = 2.5f };
    param_write(TID_APPLET_FLOAT, write_val);
    param_save_one(TID_APPLET_FLOAT);

    param_deinit();
    mock_callbacks_reset();
    param_init(&g_mock_storage, NULL);
    register_all();

    param_load_all();

    param_value_t read_val;
    param_read(TID_APPLET_FLOAT, &read_val);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 2.5f, read_val.f32);
}

void run_test_load_save(void)
{
    RUN_TEST(test_load_one_valid);
    RUN_TEST(test_load_one_no_persist);
    RUN_TEST(test_load_one_not_found);
    RUN_TEST(test_load_float);
    RUN_TEST(test_load_bool);
    RUN_TEST(test_load_ip_param);
    RUN_TEST(test_load_storage_fails);
    RUN_TEST(test_load_all_multiple);
    RUN_TEST(test_load_all_partial_flash);
    RUN_TEST(test_save_one_writes_storage);
    RUN_TEST(test_save_all_skips_non_persist);
    RUN_TEST(test_save_float);
    RUN_TEST(test_save_bool);
    RUN_TEST(test_save_blob);
    RUN_TEST(test_save_load_roundtrip_uint);
    RUN_TEST(test_save_load_roundtrip_float);
}
