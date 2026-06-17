#include <stdio.h>
#define UNITY_OUTPUT_CHAR(a) putchar(a)
#include "unity.h"
#include "test_suite.h"

extern const test_suite_t _ts_param_manager;
extern const test_suite_t _ts_ymodem_common;
extern const test_suite_t _ts_ymodem_sender;
extern const test_suite_t _ts_ymodem_receiver;
extern const test_suite_t _ts_protocol_chain;
extern const test_suite_t _ts_imperx;
extern const test_suite_t _ts_camyu;
extern const test_suite_t _ts_isc;
extern const test_suite_t _ts_vsc_lite;
extern const test_suite_t _ts_vsc_resolver;

int main(void)
{
    printf("=== Test Suite Runner ===\n");

    test_suite_add(&_ts_param_manager);
    test_suite_add(&_ts_ymodem_common);
    test_suite_add(&_ts_ymodem_sender);
    test_suite_add(&_ts_ymodem_receiver);
    test_suite_add(&_ts_protocol_chain);
    test_suite_add(&_ts_imperx);
    test_suite_add(&_ts_camyu);
    test_suite_add(&_ts_isc);
    test_suite_add(&_ts_vsc_lite);
    //test_suite_add(&_ts_vsc_resolver);

    int failures = test_suite_run_all();
    (void)failures;
    while (1) {}
    return 0;
}
