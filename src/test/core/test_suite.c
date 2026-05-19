#include <stdio.h>
#include "unity_internals.h"
#include "test_suite.h"

#define SUITE_PRINT(...) printf(__VA_ARGS__)

const test_suite_t *g_active_suite;

#define TEST_SUITE_MAX 32

typedef struct {
    const char *name;
    int   total;
    int   failures;
} suite_result_t;

static const test_suite_t *g_registry[TEST_SUITE_MAX];
static int g_registry_count;

void test_suite_add(const test_suite_t *suite)
{
    if (suite && g_registry_count < TEST_SUITE_MAX)
        g_registry[g_registry_count++] = suite;
}

int test_suite_run_all(void)
{
    int total_failures = 0;
    int count = g_registry_count;
    suite_result_t results[TEST_SUITE_MAX];

    for (int i = 0; i < count; i++) {
        const test_suite_t *s = g_registry[i];
        g_active_suite = (test_suite_t *)s;
        SUITE_PRINT("\n=== Suite [%d/%d]: %s ===\n", i + 1, count, s->name);
        int failed = s->run();
        if (failed > 0) total_failures += failed;

        results[i].name     = s->name;
        results[i].total    = Unity.NumberOfTests;
        results[i].failures = failed;
    }

    SUITE_PRINT("\n");
    SUITE_PRINT("========================================\n");
    SUITE_PRINT("  Test Summary\n");
    SUITE_PRINT("========================================\n");
    int grand_total = 0;
    for (int i = 0; i < count; i++) {
        int passed = results[i].total - results[i].failures;
        const char *status = results[i].failures ? "FAIL" : " OK ";
        SUITE_PRINT("  %-20s %4d passed  %2d failed  [%s]\n",
                    results[i].name, passed, results[i].failures, status);
        grand_total += results[i].total;
    }
    SUITE_PRINT("----------------------------------------\n");
    SUITE_PRINT("  %-20s %4d passed  %2d failed\n",
                "TOTAL", grand_total - total_failures, total_failures);
    SUITE_PRINT("========================================\n");

    return total_failures;
}

void setUp(void)
{
    if (g_active_suite && g_active_suite->set_up)
        g_active_suite->set_up();
}

void tearDown(void)
{
    if (g_active_suite && g_active_suite->tear_down)
        g_active_suite->tear_down();
}
