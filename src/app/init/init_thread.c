#include "init_thread.h"
#include "protocol_parser_thread.h"
#include "app_config.h"
#include "protocol_chain_init.h"
#include "file_storage_init.h"
#include "param_manager_init.h"
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(init_thread, LOG_LEVEL_INF);

K_THREAD_STACK_DEFINE(protocol_parser_stack, STACK_SIZE);
static struct k_thread protocol_parser_thread_data;

test_ctx_t test_ctx;
extern void app_sensor_demo(void);
void init_thread_entry(void *arg1, void *arg2, void *arg3)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    test_ctx_t *ctx = &test_ctx;

    memset(ctx, 0, sizeof(test_ctx_t));

    if (!protocol_chain_init(ctx))
    {
        LOG_ERR("Protocol chain init failed");
        return;
    }

    if (!file_storage_init(ctx))
    {
        LOG_ERR("File storage init failed");
        goto fail_fp;
    }

    param_manager_init();

    LOG_INF("===== Protocol Chain Test Started =====");
    LOG_INF("  Imperx:     ready (send hex frames to test)");
    LOG_INF("  Camyu:      ready (send hex frames to test)");
    LOG_INF("  Ymodem Rx:  idle (start via Imperx WRITE 0xF000)");
    LOG_INF("  Ymodem Tx:  idle (start via Imperx WRITE 0xF001)");
    LOG_INF("  File io:   LittleFS (file_io_fs)");
    LOG_INF("=========================================");


    app_sensor_demo();

    k_thread_create(&protocol_parser_thread_data, protocol_parser_stack,
                    K_THREAD_STACK_SIZEOF(protocol_parser_stack),
                    protocol_parser_thread_entry, NULL, NULL, NULL,
                    THREAD_PRIORITY, 0, K_NO_WAIT);
    k_thread_name_set(&protocol_parser_thread_data, "protocol_parser");
    return;

fail_fp:
    protocol_chain_cleanup(ctx);
}
