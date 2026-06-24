#include "init_thread.h"
#include "protocol_parser_thread.h"
#include "app_config.h"
#include "protocol_chain_init.h"
#include "file_storage_init.h"
#include "param_manager_init.h"
#include "vsc_lite.h"
#include "crop_vsc.h"
#include "binning_vsc.h"
#include "decoder_vsc.h"
#include "histogram_vsc.h"
#include "sensor_vsc.h"
#include "sensor_imx296_vsc.h"
#include <zephyr/logging/log.h>
#include <string.h>
#include "vsc_lite_board.h"

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

    /* ── VSC Lite pipeline ── */
    // vsc_lite_pipeline_t isp_pipe;
    // int rc = vsc_lite_pipeline_init(&isp_pipe, vsc_lite_board_stages, vsc_lite_board_num_stages);
    // if (rc != VSC_OK) {
    //     LOG_ERR("VSC Lite pipeline init failed: %d", rc);
    // } else {
    //     vsc_mbus_fmt_t intent = { { 1920, 1080, 0, 0, 0, 0, 0, 0, VSC_FMT_RAW10, 30, 1, 10, 4 }};
    //     vsc_resolver_result_t result;
    //     rc = vsc_lite_try_fmt(&isp_pipe, &intent, &result);
    //     if (rc == VSC_OK && vsc_fmt_is_valid(&result.primary_fmt)) {
    //         LOG_INF("VSC Lite negotiated: %ux%u fmt=0x%x fps=%u",
    //                 result.primary_fmt.spatial.width, result.primary_fmt.spatial.height,
    //                 (unsigned)result.primary_fmt.spatial.pixel_format,
    //                 (unsigned)result.primary_fmt.spatial.frame_rate_num);
    //         vsc_lite_commit_fmt(&isp_pipe, &result.primary_fmt);
    //     } else {
    //         LOG_WRN("VSC Lite negotiation failed: %d", rc);
    //     }
    // }

    //app_sensor_demo();

    k_thread_create(&protocol_parser_thread_data, protocol_parser_stack,
                    K_THREAD_STACK_SIZEOF(protocol_parser_stack),
                    protocol_parser_thread_entry, NULL, NULL, NULL,
                    THREAD_PRIORITY, 0, K_NO_WAIT);
    k_thread_name_set(&protocol_parser_thread_data, "protocol_parser");
    return;

fail_fp:
    protocol_chain_cleanup(ctx);
}
