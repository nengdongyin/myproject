#pragma once

#include <zephyr/kernel.h>
#include "protocol_parser.h"
#include "protocol_chain.h"
#include "protocol_parser_imperx.h"
#include "protocol_parser_camyu.h"

#define TEST_BAUD_RATE 460800
#define STACK_SIZE 12288
#define INIT_STACK_SIZE 4096
#define THREAD_PRIORITY 7
/* 接收管道长度属于 bsp 内部细节 (BSP_UART_PIPE_SIZE, 见 platform/bsp/bsp_uart.c) */

#define IMPERX_RX_BUF_SIZE 32
#define IMPERX_TX_BUF_SIZE 32
#define CAMYU_RX_BUF_SIZE 320
#define CAMYU_TX_BUF_SIZE 320

/**
 * @brief 应用上下文
 *
 * @note 只承载 imperx / camyu 与全局收发统计。ymodem 的状态、文件句柄、
 *       收发缓冲区全部归 src/core/file_transfer 组件所有 —— 传输相关配置
 *       走参数系统 (PID_FT_*), 不再进本结构体。
 */
typedef struct
{
    protocol_chain *chain;

    imperx_protocol_parser_t *imperx_parser;
    camyu_protocol_parser_t *camyu_parser;

    uint8_t imperx_rx_buf[IMPERX_RX_BUF_SIZE];
    uint8_t imperx_tx_buf[IMPERX_TX_BUF_SIZE];
    uint8_t camyu_rx_buf[CAMYU_RX_BUF_SIZE];
    uint8_t camyu_tx_buf[CAMYU_TX_BUF_SIZE];

    uint32_t total_rx_bytes;
} app_ctx_t;

extern app_ctx_t app_ctx;
/* 串口设备/接收管道 不再暴露给 app: 它们属于 platform/bsp (见 bsp_uart.h 的
   bsp_uart_pipe())。app 层只经 bsp_uart.h 的接口取用。 */
