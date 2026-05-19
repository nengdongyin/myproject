#ifndef TEST_SUITE_H
#define TEST_SUITE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct test_suite {
    const char *name;
    void (*set_up)(void);
    void (*tear_down)(void);
    int  (*run)(void);
} test_suite_t;

extern const test_suite_t *g_active_suite;

#define TEST_SUITE_DEFINE(_name, _setup, _tdown, _runner)               \
    const test_suite_t _ts_##_name = {                                  \
        .name = #_name, .set_up = (_setup),                             \
        .tear_down = (_tdown), .run = (_runner)                         \
    }

void test_suite_add(const test_suite_t *suite);
int  test_suite_run_all(void);

#ifdef __cplusplus
}
#endif
#endif
