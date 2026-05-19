#pragma once

#include <zephyr/kernel.h>
#include "protocol_parser.h"
#include "protocol_chain.h"
#include "protocol_parser_imperx.h"
#include "protocol_parser_camyu.h"
#include "protocol_parser_ymodem.h"
#include "file_processing.h"
#include "ymodem_common.h"

#define TEST_BAUD_RATE 460800
#define STACK_SIZE 12288
#define INIT_STACK_SIZE 4096
#define THREAD_PRIORITY 7
#define PIPE_BUFFER_SIZE 2048

#define IMPERX_RX_BUF_SIZE 32
#define IMPERX_TX_BUF_SIZE 32
#define CAMYU_RX_BUF_SIZE 320
#define CAMYU_TX_BUF_SIZE 320
#define YMODEM_RECV_RX_BUF_SIZE YMODEM_STX_FRAME_LEN_BYTE
#define YMODEM_RECV_TX_BUF_SIZE 4
#define YMODEM_SEND_RX_BUF_SIZE 4
#define YMODEM_SEND_TX_BUF_SIZE YMODEM_STX_FRAME_LEN_BYTE

typedef struct
{
    protocol_chain *chain;

    imperx_protocol_parser_t *imperx_parser;
    camyu_protocol_parser_t *camyu_parser;
    ymodem_protocol_parser_t *ymodem_recv_parser;
    ymodem_protocol_parser_t *ymodem_send_parser;

    uint8_t imperx_rx_buf[IMPERX_RX_BUF_SIZE];
    uint8_t imperx_tx_buf[IMPERX_TX_BUF_SIZE];
    uint8_t camyu_rx_buf[CAMYU_RX_BUF_SIZE];
    uint8_t camyu_tx_buf[CAMYU_TX_BUF_SIZE];
    uint8_t ymodem_recv_rx_buf[YMODEM_RECV_RX_BUF_SIZE];
    uint8_t ymodem_recv_tx_buf[YMODEM_RECV_TX_BUF_SIZE];
    uint8_t ymodem_send_rx_buf[YMODEM_SEND_RX_BUF_SIZE];
    uint8_t ymodem_send_tx_buf[YMODEM_SEND_TX_BUF_SIZE];

    file_processing_t *recv_fp;
    file_processing_t *send_fp;
    char send_file_name[FP_NAME_MAX];

    uint32_t total_rx_bytes;
    uint32_t total_tx_bytes;
} test_ctx_t;

extern test_ctx_t test_ctx;
extern const struct device *const uart1_dev;
extern struct k_pipe uart_pipe;
