#include "protocol_chain_init.h"
#include <zephyr/logging/log.h>
#include <zephyr/drivers/uart.h>
#include "imperx_cmd_map.h"
#include "param_manager.h"
#include <string.h>
#include "param_dump.h"

LOG_MODULE_REGISTER(chain_test, LOG_LEVEL_DBG);

static void on_tx_ready(protocol_parser_t *parser, void *user_ctx)
{
    test_ctx_t *ctx = (test_ctx_t *)user_ctx;
    uint32_t len = parser->tx.data_len;
    for (uint32_t i = 0; i < len; i++)
    {
        uart_poll_out(uart1_dev, parser->tx.buffer[i]);
    }
    ctx->total_tx_bytes += len;
    LOG_DBG("TX: %u bytes", len);
}
static void dump_cb(const char *line, void *user_data)
{
    (void)user_data;
    LOG_INF("%s", line);
}
static void imperx_on_frame_ready(protocol_parser_t *parser,
                                  void *parsed_data, void *user_ctx)
{
    test_ctx_t *ctx = (test_ctx_t *)user_ctx;
    imperx_private_t *pri = (imperx_private_t *)parsed_data;
    imperx_cmd_type_t cmd = (pri->parsed_len == 0) ? IMPERX_CMD_READ
                                                   : IMPERX_CMD_WRITE;

    LOG_INF("Rev Imperx %s, Address: 0x%04llX",
            (cmd == IMPERX_CMD_READ) ? "READ" : "WRITE",
            (unsigned long long)pri->parsed_id);

    const cmd_map_entry_t *e = cmd_map_lookup((uint16_t)pri->parsed_id);
    if (e) {
        if (cmd == IMPERX_CMD_WRITE) {
            int ret = param_write_raw(e->param_id, pri->parsed_data, pri->parsed_len);
            param_flush();
            param_dump(PARAM_MODULE_ID(e->param_id), dump_cb, NULL);
        } else {
            param_value_t v;
            int ret = param_read(e->param_id, &v);
            if (ret == PARAM_OK) {
                static uint8_t s_resp[4];
                memcpy(s_resp, &v.u32, 4);
                pri->parsed_data = s_resp;
                pri->parsed_len = sizeof(s_resp);
            }
        }
    LOG_INF("Resp Imperx %s, Address: 0x%04llX",
            (cmd == IMPERX_CMD_READ) ? "READ" : "WRITE",
            (unsigned long long)pri->parsed_id);
        return;
    }

    if (cmd == IMPERX_CMD_WRITE && pri->parsed_id == 0xF000) {
        LOG_INF("Triggering Ymodem receiver start...");
        ymodem_adapter_start_receiver(ctx->ymodem_recv_parser);
        protocol_chain_set_locked_parser(ctx->chain,
                                         (protocol_parser_t *)ctx->ymodem_recv_parser);
    } else if (cmd == IMPERX_CMD_WRITE && pri->parsed_id == 0xF001) {
        LOG_INF("Triggering Ymodem sender start...");
        ymodem_adapter_start_sender(ctx->ymodem_send_parser);
        protocol_chain_set_locked_parser(ctx->chain,
                                         (protocol_parser_t *)ctx->ymodem_send_parser);
    }
}

static void camyu_on_frame_ready(protocol_parser_t *parser,
                                 void *parsed_data, void *user_ctx)
{
    camyu_private_t *pri = (camyu_private_t *)parsed_data;

    LOG_INF("Camyu Opcode:%d, Address: 0x%08llX",
            pri->current_frame_info.opcode,
            (unsigned long long)pri->parsed_id);

    if (pri->current_frame_info.opcode == PCTOCAMERA_READ)
    {
        static uint8_t s_resp[4] = {0xAA, 0xBB, 0xCC, 0xDD};
        pri->parsed_data = s_resp;
        pri->parsed_data_len = sizeof(s_resp);
    }
}

static void ymodem_recv_on_frame_ready(protocol_parser_t *parser,
                                       void *parsed_data, void *user_ctx)
{
    ymodem_receiver_event_t *event = (ymodem_receiver_event_t *)parsed_data;
    test_ctx_t *ctx = (test_ctx_t *)user_ctx;

    if (!event)
    {
        return;
    }

    switch (event->type)
    {

    case YMODEM_RECV_EVENT_FILE_INFO:
        LOG_INF("Ymodem recv FILE_INFO: name=\"%s\" size=%u",
                event->file_name, event->file_size);
        fp_open_for_write(ctx->recv_fp, event->file_name);
        break;

    case YMODEM_RECV_EVENT_DATA_PACKET:
        fp_write(ctx->recv_fp, event->data, event->data_len);
        break;

    case YMODEM_RECV_EVENT_TRANSFER_COMPLETE:
        LOG_INF("Ymodem recv: TRANSFER_COMPLETE (%u bytes total)",
                event->total_received);
        fp_close(ctx->recv_fp);
        break;

    case YMODEM_RECV_EVENT_TRANSFER_FINISHED:
        LOG_INF("Ymodem recv: session finished");
        break;

    case YMODEM_RECV_EVENT_ERROR:
        LOG_ERR("Ymodem recv: transfer aborted");
        break;

    default:
        break;
    }
}

static void ymodem_send_on_frame_ready(protocol_parser_t *parser,
                                       void *parsed_data, void *user_ctx)
{
    ymodem_sender_event_t *event = (ymodem_sender_event_t *)parsed_data;
    test_ctx_t *ctx = (test_ctx_t *)user_ctx;

    switch (event->type)
    {

    case YMODEM_SENDER_EVENT_FILE_INFO:
        if (event->file_index != 0)
        {
            ymodem_sender_t *send =
                ymodem_adapter_get_sender((ymodem_protocol_parser_t *)parser);
            send->file_info.file_name[0] = '\0';
            send->file_info.file_total_size = 0;
            LOG_INF("Ymodem send: no more files, ending session");
        }
        else
        {
            uint32_t size = 0;
            if (fp_get_size(ctx->send_fp, ctx->send_file_name, &size) != FP_OK)
            {
                LOG_ERR("Ymodem send: cannot get size of %s", ctx->send_file_name);
                break;
            }
            fp_open_for_read(ctx->send_fp, ctx->send_file_name);
            ymodem_sender_t *send =
                ymodem_adapter_get_sender((ymodem_protocol_parser_t *)parser);
            strncpy(send->file_info.file_name, ctx->send_file_name,
                    sizeof(send->file_info.file_name) - 1);
            send->file_info.file_total_size = size;
            ymodem_sender_enable_1k(send);
            LOG_INF("Ymodem send: FILE_INFO idx=%u name=%s size=%u",
                    event->file_index, ctx->send_file_name, size);
        }
        break;

    case YMODEM_SENDER_EVENT_DATA_PACKET:
    {
        uint32_t actual = 0;
        fp_read(ctx->send_fp, event->data_seq * 1024,
                event->data, 1024, &actual);
        event->data_len = actual;
        break;
    }

    case YMODEM_SENDER_EVENT_TRANSFER_COMPLETE:
        LOG_INF("Ymodem send: TRANSFER_COMPLETE");
        fp_close(ctx->send_fp);
        break;

    case YMODEM_SENDER_EVENT_SESSION_FINISHED:
        LOG_INF("Ymodem send: SESSION_FINISHED");
        break;

    case YMODEM_SENDER_EVENT_ERROR:
        LOG_ERR("Ymodem send: transfer aborted");
        break;

    default:
        break;
    }
}

bool protocol_chain_init(test_ctx_t *ctx)
{
    ctx->chain = protocol_chain_create(4);
    if (!ctx->chain)
    {
        LOG_ERR("Failed to create protocol chain");
        return false;
    }

    ctx->imperx_parser = imperx_protocol_create(
        ctx->imperx_rx_buf, sizeof(ctx->imperx_rx_buf),
        ctx->imperx_tx_buf, sizeof(ctx->imperx_tx_buf));
    if (!ctx->imperx_parser)
    {
        LOG_ERR("Failed to create Imperx parser");
        goto fail_imperx;
    }
    protocol_parser_set_callbacks((protocol_parser_t *)ctx->imperx_parser,
                                  imperx_on_frame_ready, ctx,
                                  on_tx_ready, ctx);

    ctx->camyu_parser = camyu_protocol_create(
        ctx->camyu_rx_buf, sizeof(ctx->camyu_rx_buf),
        ctx->camyu_tx_buf, sizeof(ctx->camyu_tx_buf));
    if (!ctx->camyu_parser)
    {
        LOG_ERR("Failed to create Camyu parser");
        goto fail_camyu;
    }
    protocol_parser_set_callbacks((protocol_parser_t *)ctx->camyu_parser,
                                  camyu_on_frame_ready, ctx,
                                  on_tx_ready, ctx);

    ctx->ymodem_recv_parser = ymodem_protocol_create_receiver(
        ctx->ymodem_recv_rx_buf, sizeof(ctx->ymodem_recv_rx_buf),
        ctx->ymodem_recv_tx_buf, sizeof(ctx->ymodem_recv_tx_buf));
    if (!ctx->ymodem_recv_parser)
    {
        LOG_ERR("Failed to create Ymodem receiver parser");
        goto fail_ymodem_recv;
    }
    protocol_parser_set_callbacks((protocol_parser_t *)ctx->ymodem_recv_parser,
                                  ymodem_recv_on_frame_ready, ctx,
                                  on_tx_ready, ctx);

    ctx->ymodem_send_parser = ymodem_protocol_create_sender(
        ctx->ymodem_send_rx_buf, sizeof(ctx->ymodem_send_rx_buf),
        ctx->ymodem_send_tx_buf, sizeof(ctx->ymodem_send_tx_buf));
    if (!ctx->ymodem_send_parser)
    {
        LOG_ERR("Failed to create Ymodem sender parser");
        goto fail_ymodem_send;
    }
    protocol_parser_set_callbacks((protocol_parser_t *)ctx->ymodem_send_parser,
                                  ymodem_send_on_frame_ready, ctx,
                                  on_tx_ready, ctx);

    protocol_chain_add_parser(ctx->chain,
                              (protocol_parser_t *)ctx->ymodem_recv_parser);
    protocol_chain_add_parser(ctx->chain,
                              (protocol_parser_t *)ctx->ymodem_send_parser);
    protocol_chain_add_parser(ctx->chain,
                              (protocol_parser_t *)ctx->imperx_parser);
    protocol_chain_add_parser(ctx->chain,
                              (protocol_parser_t *)ctx->camyu_parser);

    LOG_INF("Protocol chain initialized: Ymodem(Rx), Ymodem(Tx), Imperx, Camyu");
    return true;

fail_ymodem_send:
    protocol_parser_destroy((protocol_parser_t *)ctx->ymodem_recv_parser);
    ctx->ymodem_recv_parser = NULL;
fail_ymodem_recv:
    protocol_parser_destroy((protocol_parser_t *)ctx->camyu_parser);
    ctx->camyu_parser = NULL;
fail_camyu:
    protocol_parser_destroy((protocol_parser_t *)ctx->imperx_parser);
    ctx->imperx_parser = NULL;
fail_imperx:
    protocol_chain_destroy(ctx->chain);
    ctx->chain = NULL;
    return false;
}

void protocol_chain_cleanup(test_ctx_t *ctx)
{
    if (ctx->chain)
    {
        protocol_chain_destroy(ctx->chain);
        ctx->chain = NULL;
    }
    if (ctx->imperx_parser)
    {
        protocol_parser_destroy((protocol_parser_t *)ctx->imperx_parser);
        ctx->imperx_parser = NULL;
    }
    if (ctx->camyu_parser)
    {
        protocol_parser_destroy((protocol_parser_t *)ctx->camyu_parser);
        ctx->camyu_parser = NULL;
    }
    if (ctx->ymodem_recv_parser)
    {
        protocol_parser_destroy((protocol_parser_t *)ctx->ymodem_recv_parser);
        ctx->ymodem_recv_parser = NULL;
    }
    if (ctx->ymodem_send_parser)
    {
        protocol_parser_destroy((protocol_parser_t *)ctx->ymodem_send_parser);
        ctx->ymodem_send_parser = NULL;
    }
}
