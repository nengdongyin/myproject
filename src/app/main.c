#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include "app_config.h"
#include "init_thread.h"
#include "ymodem_common.h"

#define UART1_NODE DT_NODELABEL(uart1)
const struct device *const uart1_dev = DEVICE_DT_GET(UART1_NODE);

K_THREAD_STACK_DEFINE(init_stack, INIT_STACK_SIZE);
static struct k_thread init_thread_data;

K_PIPE_DEFINE(uart_pipe, PIPE_BUFFER_SIZE, 1);

uint32_t system_get_time_ms(void)
{
    return k_uptime_get_32();
}

static void uart1_irq_callback(const struct device *dev, void *user_data)
{
    uart_irq_update(dev);
    if (uart_irq_rx_ready(dev))
    {
        uint8_t rx_buf[YMODEM_STX_FRAME_LEN_BYTE];
        int bytes_read = uart_fifo_read(dev, rx_buf, sizeof(rx_buf));
        if (bytes_read > 0)
        {
            k_pipe_write(&uart_pipe, rx_buf, (size_t)bytes_read, K_NO_WAIT);
        }
    }

    if (uart_irq_tx_ready(dev))
    {
    }
}

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

int main(void)
{
    printk("20260521\n");
    printk("=== Protocol Chain Test (Zephyr) ===\n");
    printk("  Protocols: Imperx, Camyu, Ymodem(Rx), Ymodem(Tx)\n");
    printk("\n");

    if (!device_is_ready(uart1_dev))
    {
        printk("Error: UART1 device not ready\n");
        return -1;
    }

    set_baud_rate(uart1_dev, TEST_BAUD_RATE);

    uart_irq_callback_user_data_set(uart1_dev, uart1_irq_callback, NULL);
    uart_irq_rx_enable(uart1_dev);

    k_thread_create(&init_thread_data, init_stack,
                    K_THREAD_STACK_SIZEOF(init_stack),
                    init_thread_entry, NULL, NULL, NULL,
                    THREAD_PRIORITY, 0, K_NO_WAIT);
    k_thread_name_set(&init_thread_data, "init");

    while (1)
    {
        k_msleep(5000);
    }
    return 0;
}
