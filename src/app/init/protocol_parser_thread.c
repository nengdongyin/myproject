#include "protocol_parser_thread.h"
#include "app_config.h"
#include "bsp_uart.h" /* bsp_uart_pipe(): 接收管道由 bsp 提供 */
#include "ymodem_common.h"
#include "file_transfer.h"
#include "console_mode.h"
#include "shell_api.h"
#include <zephyr/kernel.h> /* k_pipe_read / K_MSEC: 不靠 app_config.h 传递 */
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(protocol_parser_thread, LOG_LEVEL_DBG);

void protocol_parser_thread_entry(void *arg1, void *arg2, void *arg3)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    app_ctx_t *ctx = &app_ctx;

    uint8_t data_buf[YMODEM_STX_FRAME_LEN_BYTE];

    while (1)
    {
        int n;

        /* ymodem 接收态: 单帧最多 1029 字节, 一次读满比逐字节读快两个数量级;
           其余状态逐字节读, 便于协议链按首字节匹配。 */
        if (file_transfer_wants_bulk_rx())
        {
            n = k_pipe_read(bsp_uart_pipe(), data_buf,
                            sizeof(data_buf), K_MSEC(100));
        }
        else
        {
            n = k_pipe_read(bsp_uart_pipe(), data_buf,
                            1, K_MSEC(100));
            if (n > 0)
            {
                LOG_DBG("rx: 0x%02X", data_buf[0]);
            }
        }

        if (n > 0)
        {
            ctx->total_rx_bytes += (uint32_t)n;

            /* 通信串口是时分复用: 同一批字节只能交给一个消费者。
               shell 模式下 shellHandler 逐字节解析并回显, 再喂协议链就会
               同时踩回显污染 / 0x0A 触发命令行执行 / 0x52,0x57 撞 Imperx 帧头。 */
            if (console_mode_get() == CONSOLE_MODE_SHELL)
            {
                shell_rx_feed(data_buf, (uint32_t)n);
            }
            else
            {
                (void)protocol_chain_feed(ctx->chain, data_buf, (uint32_t)n);
            }
        }
        else
        {
            /* 协议超时重传 / 握手重试 / 会话收尾全部由各 parser 的 poll 钩子
               驱动, file_transfer 组件不参与本循环的任何调度。 */
            protocol_chain_check_timeout_poll(ctx->chain);
        }
    }
}
