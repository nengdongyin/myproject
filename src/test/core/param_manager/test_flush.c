#include "test_common.h"
#include <string.h>

DEF_TEST_UINT  (fu32, TID_APPLET_UINT,  PARAM_FLAG_PERSIST, 100, 0, 200);
PARAM_BOOL     (fboo, TID_APPLET_BOOL,  PARAM_FLAG_PERSIST, true);
DEF_TEST_UINT  (fu2,  TID_APPLET2_UINT, PARAM_FLAG_PERSIST, 200, 0, 300);

PARAM_IP_UINT  (fipu,  TID_IP_UINT,  PARAM_FLAG_PERSIST, 500,   500,   500);
PARAM_IP_FLOAT (fipf,  TID_IP_FLOAT, PARAM_FLAG_PERSIST, 2.0f,  2.0f,  2.0f);
PARAM_IP_BOOL  (fipb,  TID_IP_BOOL,  PARAM_FLAG_PERSIST, false);

PARAM_TABLE(applet_params,  &fu32.base, &fboo.base);
PARAM_TABLE(applet2_params, &fu2.base);
PARAM_TABLE(ip_params, &fipu.base, &fipf.base, &fipb.base);

PARAM_MODULE_DEFINE(test_applet,  TEST_MODULE_APPLET,  "FlushApplet",  NULL, NULL, NULL, mock_apply_ok, NULL, mock_flush_ok);
PARAM_MODULE_DEFINE(test_applet2, TEST_MODULE_APPLET2, "FlushApplet2", NULL, NULL, NULL, mock_apply_ok, NULL, mock_flush_ok);
IP_DRIVER_DEFINE(test_ip, TEST_MODULE_IP, "FlushIP", NULL, NULL, NULL, mock_ip_write_ok, NULL, NULL);

/* P0-2 回归: READONLY / EXEC 条目不得进入写硬件路径 */
PARAM_IP_UINT(fip_ro,   TID_IP_RDONLY, PARAM_FLAG_READONLY, 0, 0, 0);
PARAM_IP_EXEC(fip_exec, TID_IP_EXEC);

PARAM_TABLE(ip_roexec_params, &fipu.base, &fip_ro.base, &fip_exec.base);

IP_DRIVER_DEFINE(roexec_ip, TEST_MODULE_IP, "RoExecIP",
                 NULL, NULL, NULL, mock_ip_write_record, NULL, NULL);

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

void test_flush_clears_ip_dirty(void) {
    register_all();

    param_value_t v = { .u32 = 800 };
    param_write(TID_IP_UINT, v);

    param_flush();

    param_stats_t stats;
    param_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT16(0, stats.dirty_count);
    TEST_ASSERT_DIRTY(&fipu.base, 0);
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

/* ================================================================
 *  P0-2: READONLY / EXEC 永不进入写硬件路径
 * ================================================================ */
void test_flush_skips_ip_readonly_and_exec(void) {
    ip_driver_register(&roexec_ip_instance, ip_roexec_params,
                       PARAM_COUNT(ip_roexec_params));
    test_reset_entry(&fipu.base);
    test_reset_entry(&fip_ro.base);
    test_reset_entry(&fip_exec.base);

    mock_ip_reset();

    /* load_all: load 阶段不标脏 READONLY/EXEC (ip_param_load 过滤),
       init 阶段仅可写参数标脏 (ip_module_init 过滤) */
    param_load_all();
    TEST_ASSERT_PARAM_OK(param_flush());

    /* 仅可写参数被写硬件一次, 且回调收到的 id 不是 READONLY/EXEC */
    TEST_ASSERT_EQUAL_INT(1, g_ip_write_call_count);
    TEST_ASSERT_EQUAL_UINT32(TID_IP_UINT, g_ip_write_last_id);
    TEST_ASSERT_DIRTY(&fip_ro.base, 0);
    TEST_ASSERT_DIRTY(&fip_exec.base, 0);

    /* 防御路径: 即使被外部误标脏, flush 只清除 dirty, 绝不写硬件 */
    param_module_node_t *m = param_module_find(TEST_MODULE_IP);
    param_entry_mark_dirty(&fip_ro.base);
    param_entry_mark_dirty(&fip_exec.base);
    if (m) m->dirty = 1;
    mock_ip_reset();

    TEST_ASSERT_PARAM_OK(param_flush());
    TEST_ASSERT_EQUAL_INT(0, g_ip_write_call_count);
    TEST_ASSERT_DIRTY(&fip_ro.base, 0);
    TEST_ASSERT_DIRTY(&fip_exec.base, 0);

    param_stats_t stats;
    param_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT16(0, stats.dirty_count);
}

void run_test_flush(void)
{
    RUN_TEST(test_flush_no_dirty_returns_ok);
    RUN_TEST(test_flush_with_dirty_applet);
    RUN_TEST(test_flush_skips_clean_modules);
    RUN_TEST(test_flush_ip_writes_hardware);
    RUN_TEST(test_flush_clears_ip_dirty);
    RUN_TEST(test_flush_multiple_ip_params);
    RUN_TEST(test_flush_applet_then_ip_order);
    RUN_TEST(test_flush_skips_ip_readonly_and_exec);
}
