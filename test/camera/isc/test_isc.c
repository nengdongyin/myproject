/**
 * @file    test_isc_main.c
 * @brief   ISC 单元测试 — 套件入口
 */

#include "test_isc_common.h"
#include "test_suite.h"

/* ═══════════════════════════════════════════════════════════════════════════
 *  外部测试文件提供的 runner 函数
 * ═══════════════════════════════════════════════════════════════════════════ */

extern void run_test_isc_lifecycle(void);
extern void run_test_isc_format(void);
extern void run_test_isc_control(void);

/* ═══════════════════════════════════════════════════════════════════════════
 *  ISC 套件 runner
 * ═══════════════════════════════════════════════════════════════════════════ */

int isc_run_all_tests(void)
{
    UNITY_BEGIN();

    run_test_isc_lifecycle();
    run_test_isc_format();
    run_test_isc_control();

    return UNITY_END();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  套件注册 (test_suite.h 的宏)
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_SUITE_DEFINE(isc, isc_setup, isc_teardown, isc_run_all_tests);
