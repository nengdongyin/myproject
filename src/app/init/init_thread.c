#include "init_thread.h"
#include "protocol_parser_thread.h"
#include "app_config.h"
#include "bsp_uart.h"
#include "diag_init.h"
#include "protocol_chain_init.h"
#include "file_storage_init.h"
#include "param_manager_init.h"
#include "vsc_lite.h"
#include "port.h"
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

app_ctx_t app_ctx;
extern void app_sensor_demo(void);
void init_thread_entry(void *arg1, void *arg2, void *arg3)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    app_ctx_t *ctx = &app_ctx;

    memset(ctx, 0, sizeof(app_ctx_t));

    /* ================================================================
     *  装配顺序表 (改顺序前请逐条确认)
     *
     *  1. 平台互斥锁 —— param/file/vfs 的 system_lock(id) 依赖它;
     *  2. 串口就绪   —— log/shell 的字节出口指向这个口, 中断也开始往管道灌字节;
     *  3. 诊断设施   —— log 必须早于后面任何 LOG_*; shell 早于一切命令入口;
     *  4. 协议链     —— 建链 + 挂 ymodem/imperx/camyu (内部会打日志);
     *  5. 存储挂载   —— littlefs 可用之后参数才能读旧值;
     *  6. 参数初始化 —— 读参数缓存/备份分区 (shell 的 ft 命令依赖参数);
     *  7. 协议解析线程 —— 最后起, 它消费第 2 步开始灌入管道的字节。
     *
     *  失败路径逆序: 协议链拆卸 → 诊断设施拆卸 (先断 shell 入口)。
     * ================================================================ */

    /* 1) 平台互斥锁 */
    system_mutex_init();

    /* 2) 串口 (设备/波特率/中断)。失败就没必要往下走 —— 没有出口可报错 */
    if (bsp_uart_start(TEST_BAUD_RATE) != 0)
    {
        return;
    }

    /* 3) 诊断设施: 日志 → shell → md/mw 白名单 */
    if (!diag_init())
    {
        return;
    }

    /* 4) 协议链 */
    if (!protocol_chain_init(ctx))
    {
        LOG_ERR("Protocol chain init failed");
        diag_deinit();
        return;
    }

    /* 5) 存储挂载 */
    if (!file_storage_init(ctx))
    {
        LOG_ERR("File storage init failed");
        goto fail_fp;
    }

    /* 6) 参数加载/初始化 */
    param_manager_init();

    LOG_INF("===== Protocol Chain Test Started =====");
    LOG_INF("  Imperx:     ready (send hex frames to test)");
    LOG_INF("  Camyu:      ready (send hex frames to test)");
    LOG_INF("  Ymodem Rx:  idle (Imperx WRITE 0xF000 -> PID_FT_RECV_START)");
    LOG_INF("  Ymodem Tx:  idle (Imperx WRITE 0xF001 -> PID_FT_SEND_START)");
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
    diag_deinit();
}
