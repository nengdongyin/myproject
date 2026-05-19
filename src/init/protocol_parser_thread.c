#include "protocol_parser_thread.h"
#include "app_config.h"
#include "ymodem_common.h"
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(protocol_parser_thread, LOG_LEVEL_DBG);

void protocol_parser_thread_entry(void *arg1, void *arg2, void *arg3)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    test_ctx_t *ctx = &test_ctx;

    uint8_t data_buf[YMODEM_STX_FRAME_LEN_BYTE];
    protocol_parser_t *locked_parser;
    uint32_t last_status_ms = k_uptime_get_32();

    while (1)
    {
        locked_parser = protocol_chain_get_locked_parser(ctx->chain);
        bool is_recv_locked = (locked_parser ==
                               (protocol_parser_t *)ctx->ymodem_recv_parser);

        int n;
        if (is_recv_locked)
        {
            n = k_pipe_read(&uart_pipe, data_buf,
                            sizeof(data_buf), K_MSEC(100));
        }
        else
        {
            n = k_pipe_read(&uart_pipe, data_buf,
                            1, K_MSEC(100));
            if (n > 0) {
                LOG_DBG("rx: 0x%02X", data_buf[0]);
            }
        }

        if (n > 0)
        {
            ctx->total_rx_bytes += (uint32_t)n;
            (void)protocol_chain_feed(ctx->chain, data_buf, (uint32_t)n);
        }
        else
        {
            protocol_chain_check_timeout_poll(ctx->chain);
        }

        // uint32_t now = k_uptime_get_32();
        // if (now - last_status_ms >= 5000)
        // {
        //     last_status_ms = now;
        //     LOG_INF("Status: rx=%u tx=%u locked=%s",
        //             ctx->total_rx_bytes, ctx->total_tx_bytes,
        //             locked_parser ? "yes" : "no");
        // }
    }
}
