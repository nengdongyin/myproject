#include "test_common.h"
#include "test_suite.h"

/* ================================================================
 *  setUp / tearDown — 每个测试用例前后调用
 * ================================================================ */

void pm_setup(void)
{
    mock_storage_reset();
    mock_storage2_reset();
    mock_callbacks_reset();
    param_init(&g_mock_storage, NULL);
}

void pm_teardown(void)
{
    param_deinit();
}

/* ================================================================
 *  Runner 清单 — 所有测试文件的 test runner 函数
 * ================================================================ */
extern void run_test_read_write(void);
extern void run_test_boundary(void);
extern void run_test_init_order(void);
extern void run_test_error_handling(void);
extern void run_test_load_save(void);
extern void run_test_flush(void);
extern void run_test_reset(void);
extern void run_test_other(void);
extern void run_test_multi_bank(void);

int param_run_all_tests(void)
{
    UNITY_BEGIN();

    run_test_read_write();
    run_test_boundary();
    run_test_init_order();
    run_test_error_handling();
    run_test_load_save();
    run_test_flush();
    run_test_reset();
    run_test_other();
    run_test_multi_bank();

    return UNITY_END();
}

TEST_SUITE_DEFINE(param_manager, pm_setup, pm_teardown, param_run_all_tests);
