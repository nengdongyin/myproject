/**
 * @file    test_runner_vsc.c
 * @brief   Standalone test runner for VSC Resolver unit tests.
 *
 * Bypasses Zephyr — links only Unity, test_suite, and VSC code.
 * Compile with:
 *   gcc -std=c11 -Isrc/vsc -Isrc/test/core -Isrc/test/core/unity -Isrc/test/vsc \
 *       src/vsc/vsc_resolver.c \
 *       src/test/vsc/test_vsc_resolver.c \
 *       src/test/core/unity/unity.c \
 *       src/test/core/test_suite.c \
 *       test_runner_vsc.c -o build/test_vsc
 */

#include <stdio.h>
#include "unity.h"
#include "test_suite.h"

extern const test_suite_t _ts_vsc_resolver;

int main(void)
{
    printf("=== VSC Resolver Unit Tests ===\n\n");

    test_suite_add(&_ts_vsc_resolver);

    int failures = test_suite_run_all();
    printf("\n=== %d failures ===\n", failures);

    return failures != 0;
}
