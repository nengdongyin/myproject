/**
 * @file bsp_uart.c
 * @brief 板级串口实现 —— 设备 / 波特率 / 中断 + 管道 / 两路裸写出口
 *
 * @details
 * 本文件是 rp2350 板上"物理串口"的唯一落点 (见 bsp_uart.h 的分层说明)。
 * 刻意**不**包含任何 `core/` 或 `app/` 头: 这里只有 Zephyr 驱动与 devicetree。
 *
 * 两个出口的区别只在"目的地":
 *   - bsp_comm_write: uart1 (通讯口)。协议帧专用, 不做模式判断 —— 若这里也按
 *     模式静默, 协议在 PROTO 模式下就发不出去了;
 *   - bsp_dbg_write : 调试口。本板无独立调试串口 (app.overlay 只 enable 了
 *     uart1), 因此借 Zephyr console (printk)。**加专用调试 UART 只改这个函数**。
 */

#include "bsp_uart.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/printk.h>

/** @brief 通讯口节点 —— 换板子只改这一行 (或由 -D 覆盖) */
#ifndef BSP_UART_NODE
#define BSP_UART_NODE DT_NODELABEL(uart1)
#endif

/** @brief 中断→管道 缓冲长度 (与产品无关的 bsp 默认值, 可用 -D 覆盖) */
#ifndef BSP_UART_PIPE_SIZE
#define BSP_UART_PIPE_SIZE 2048
#endif

/** @brief 单次中断读取上限 (RP2350 UART FIFO 只有 32B, 64 足够且省栈) */
#ifndef BSP_UART_RX_CHUNK
#define BSP_UART_RX_CHUNK 64
#endif

/** @brief 调试口输出开关 (0 = 丢弃调试输出) */
#ifndef BSP_DBG_TX_ENABLE
#define BSP_DBG_TX_ENABLE 1
#endif

/* ================================================================
 *  设备与接收通道
 * ================================================================ */

static const struct device *const s_uart_dev = DEVICE_DT_GET(BSP_UART_NODE);

/* K_PIPE_DEFINE 自带 static —— 不要再加修饰符 */
K_PIPE_DEFINE(s_uart_pipe, BSP_UART_PIPE_SIZE, 1);

struct k_pipe *bsp_uart_pipe(void)
{
    return &s_uart_pipe;
}

/**
 * @brief 串口中断: 只管把收到的字节塞进管道, 不做任何解析
 *
 * @note 管道满时 K_NO_WAIT 直接丢, 由上层协议的重传机制兜底。
 */
static void uart_irq_callback(const struct device *dev, void *user_data)
{
    (void)user_data;

    uart_irq_update(dev);
    if (uart_irq_rx_ready(dev))
    {
        uint8_t rx_buf[BSP_UART_RX_CHUNK];
        int bytes_read = uart_fifo_read(dev, rx_buf, sizeof(rx_buf));
        if (bytes_read > 0)
        {
            k_pipe_write(&s_uart_pipe, rx_buf, (size_t)bytes_read, K_NO_WAIT);
        }
    }

    if (uart_irq_tx_ready(dev))
    {
        /* 发送走 uart_poll_out (阻塞式), 这里无待发数据 */
    }
}

/** @brief 运行时改波特率 (失败只打印, 不改回原值) */
static void set_baud_rate(const struct device *uart_dev, uint32_t new_baud)
{
    struct uart_config cfg;
    int ret;

    ret = uart_config_get(uart_dev, &cfg);
    if (ret != 0)
    {
        printk("Error: uart_config_get() failed (%d)\n", ret);
        return;
    }
    cfg.baudrate = new_baud;

    ret = uart_configure(uart_dev, &cfg);
    if (ret == 0)
    {
        printk("UART baudrate set to %u\n", new_baud);
    }
    else
    {
        printk("Error: uart_configure() failed (%d)\n", ret);
    }
}

int bsp_uart_start(uint32_t baud)
{
    if (!device_is_ready(s_uart_dev))
    {
        printk("Error: UART device not ready\n");
        return -1;
    }

    set_baud_rate(s_uart_dev, baud);

    uart_irq_callback_user_data_set(s_uart_dev, uart_irq_callback, NULL);
    uart_irq_rx_enable(s_uart_dev);
    return 0;
}

/* ================================================================
 *  两路裸写出口
 * ================================================================ */

void bsp_comm_write(const uint8_t *data, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++)
    {
        uart_poll_out(s_uart_dev, data[i]);
    }
}

void bsp_dbg_write(const uint8_t *data, uint32_t len)
{
#if BSP_DBG_TX_ENABLE
    /* 按"行"聚合后再 printk: 上层一次可能交来上千字节, 逐字节 printk 太贵。
       NUL 按文本语义丢弃。 */
    char line[96 + 1];
    size_t n = 0;

    for (uint32_t i = 0; i < len; i++)
    {
        if (data[i] == '\0')
            continue;
        line[n++] = (char)data[i];
        if (n >= sizeof(line) - 1 || data[i] == '\n')
        {
            line[n] = '\0';
            printk("%s", line);
            n = 0;
        }
    }
    if (n > 0)
    {
        line[n] = '\0';
        printk("%s", line);
    }
#else
    (void)data;
    (void)len;
#endif
}
