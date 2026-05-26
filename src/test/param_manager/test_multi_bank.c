#include "test_common.h"
#include <string.h>

DEF_TEST_UINT(mb_u32, TID_APPLET_UINT,  PARAM_FLAG_PERSIST, 100, 0, 200);
DEF_TEST_UINT(mb_u32b, TID_APPLET_INT,  PARAM_FLAG_PERSIST, 50,  0, 100);

PARAM_TABLE(mb_params, &mb_u32.base, &mb_u32b.base);
PARAM_MODULE_DEFINE(mb_mod, TEST_MODULE_APPLET, "MB", NULL, NULL, NULL, mock_apply_ok, NULL, NULL);

static void register_all(void)
{
    param_module_register(&mb_mod_module, mb_params, PARAM_COUNT(mb_params));
    test_reset_entry(&mb_u32.base);
    test_reset_entry(&mb_u32b.base);
}

void test_bank0_save_bank1_load_isolated(void)
{
    register_all();

    param_set_storage(&g_mock_storage2);
    (void)param_save_all();

    param_set_storage(&g_mock_storage);

    param_value_t v;
    v.u32 = 200;
    TEST_ASSERT_PARAM_OK(param_write(TID_APPLET_UINT, v));
    TEST_ASSERT_PARAM_OK(param_save_all());

    param_set_storage(&g_mock_storage2);
    TEST_ASSERT_PARAM_OK(param_load_all());

    param_read(TID_APPLET_UINT, &v);
    TEST_ASSERT_EQUAL_UINT32(100, v.u32);

    param_set_storage(&g_mock_storage);
    TEST_ASSERT_PARAM_OK(param_load_all());
    param_read(TID_APPLET_UINT, &v);
    TEST_ASSERT_EQUAL_UINT32(200, v.u32);
}

void test_bank_switch_save_then_load(void)
{
    register_all();

    param_value_t v;
    v.u32 = 10;
    TEST_ASSERT_PARAM_OK(param_write(TID_APPLET_UINT, v));
    v.i32 = 20;
    TEST_ASSERT_PARAM_OK(param_write(TID_APPLET_INT, v));
    TEST_ASSERT_PARAM_OK(param_save_all());

    param_set_storage(&g_mock_storage2);

    v.u32 = 30;
    TEST_ASSERT_PARAM_OK(param_write(TID_APPLET_UINT, v));
    v.i32 = 40;
    TEST_ASSERT_PARAM_OK(param_write(TID_APPLET_INT, v));
    TEST_ASSERT_PARAM_OK(param_save_all());

    param_read(TID_APPLET_UINT, &v);
    TEST_ASSERT_EQUAL_UINT32(30, v.u32);
    param_read(TID_APPLET_INT, &v);
    TEST_ASSERT_EQUAL_INT32(40, v.i32);

    param_set_storage(&g_mock_storage);
    param_read(TID_APPLET_UINT, &v);
    TEST_ASSERT_EQUAL_UINT32(30, v.u32);
    param_read(TID_APPLET_INT, &v);
    TEST_ASSERT_EQUAL_INT32(40, v.i32);

    TEST_ASSERT_PARAM_OK(param_load_all());
    param_read(TID_APPLET_UINT, &v);
    TEST_ASSERT_EQUAL_UINT32(10, v.u32);
    param_read(TID_APPLET_INT, &v);
    TEST_ASSERT_EQUAL_INT32(20, v.i32);
}

void test_bank_set_null_disables_persist(void)
{
    register_all();

    param_value_t v = { .u32 = 55 };
    TEST_ASSERT_PARAM_OK(param_write(TID_APPLET_UINT, v));
    TEST_ASSERT_PARAM_OK(param_save_all());

    param_set_storage(NULL);

    int ret = param_save_all();
    TEST_ASSERT_EQUAL_INT(PARAM_ERR_NOT_FOUND, ret);

    param_set_storage(&g_mock_storage);

    param_value_t v2 = { .u32 = 0 };
    param_write(TID_APPLET_UINT, v2);
    TEST_ASSERT_PARAM_OK(param_load_one(TID_APPLET_UINT));
    param_read(TID_APPLET_UINT, &v2);
    TEST_ASSERT_EQUAL_UINT32(55, v2.u32);
}

void test_bank_init_with_null_driver(void)
{
    param_deinit();
    mock_storage_reset();
    mock_storage2_reset();
    mock_callbacks_reset();

    int ret = param_init(NULL, NULL);
    TEST_ASSERT_EQUAL_INT(PARAM_OK, ret);

    register_all();

    param_value_t v = { .u32 = 99 };
    TEST_ASSERT_PARAM_OK(param_write(TID_APPLET_UINT, v));

    int save_ret = param_save_all();
    TEST_ASSERT_EQUAL_INT(PARAM_ERR_NOT_FOUND, save_ret);

    param_set_storage(&g_mock_storage);

    TEST_ASSERT_PARAM_OK(param_save_all());
    param_set_storage(NULL);
    param_validate_all();
}

void run_test_multi_bank(void)
{
    RUN_TEST(test_bank0_save_bank1_load_isolated);
    RUN_TEST(test_bank_switch_save_then_load);
    RUN_TEST(test_bank_set_null_disables_persist);
    RUN_TEST(test_bank_init_with_null_driver);
}
