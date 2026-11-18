/**
 * @file main.c
 * @brief Zephyr 入口 —— 只做"平台最小初始化 + 起线程"
 *
 * @details
 * 本文件**不再**认识任何外设细节: 串口设备/波特率/中断/管道 都搬到了
 * platform/bsp/bsp_uart.c, 装配顺序都归 init_thread.c。留在这里的只有三件 Zephyr 强相关的事:
 *   1. `system_get_time_ms()` —— port.h 的 Zephyr 实现 (PC 侧用 mock_time.c);
 *   2. init 线程的栈与线程对象;
 *   3. `main()`: 打印 banner → 起 init 线程 → 空闲。
 *
 * @note 本文件**不碰串口**: 设备/波特率/中断的初始是 init_thread 顺序表第 2 步
 *       (`bsp_uart_start`), 那里是唯一调用点 —— 两处都调会让波特率设两遍、
 *       接收中断被重复使能。
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include "app_config.h"
#include "init_thread.h"

K_THREAD_STACK_DEFINE(init_stack, INIT_STACK_SIZE);
static struct k_thread init_thread_data;

/** @brief port.h 的 Zephyr 实现 (其余 PORT_OS_* 实现在 platform/port/port_impl.c) */
uint32_t system_get_time_ms(void)
{
    return k_uptime_get_32();
}

int main(void)
{
    printk("20260521\n");
    printk("=== Protocol Chain Test (Zephyr) ===\n");
    printk("  Protocols: Imperx, Camyu, Ymodem(Rx), Ymodem(Tx)\n");
    printk("\n");

    k_thread_create(&init_thread_data, init_stack,
                    K_THREAD_STACK_SIZEOF(init_stack),
                    init_thread_entry, NULL, NULL, NULL,
                    THREAD_PRIORITY, 0, K_NO_WAIT);
    k_thread_name_set(&init_thread_data, "init");

    /* 其余工作都在 init 线程里; 主线程无事可做, 只挂着 */
    while (1)
    {
        k_msleep(5000);
    }
    return 0;
}
