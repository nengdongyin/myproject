/**
 * @file bsp_uart.h
 * @brief 板级串口支持 —— 设备句柄 / 波特率 / 中断+管道 / 两路裸写出口
 *
 * @par 分层位置与边界
 * 本文件属于 **platform/bsp**, 因此**只允许**依赖 Zephyr 的 device / driver /
 * printk 与 devicetree, **不得** include 任何 `core/` 或 `app/` 头 —— bsp 是
 * 叶子层, 一旦认识业务策略就变成了 platform → core 的反向依赖, 换板子要连
 * 框架一起拖。
 *
 * 于是这里只提供"两个物理口的裸写能力":
 * @code
 *   bsp_comm_write()  → 通讯口 (uart1)。协议帧与"当前该由谁用这个口"的判定
 *                       全部在调用方 (app 层)
 *   bsp_dbg_write()   → 调试口。恒定输出, 与谁在用通讯口无关
 * @endcode
 * "调试口恒输出、通讯口只在 shell 模式输出"这类**选路策略**属于应用层
 * (见 app/init/diag_init.c 的 diag_tx), 不在本层。
 *
 * @par 换板子要改的地方
 * 只有本文件的 `BSP_UART_NODE` (默认 `DT_NODELABEL(uart1)`) 与调试口实现
 * (`bsp_dbg_write`)。上层调用点 (main/init_thread/diag_init/protocol_chain_init)
 * 一行都不用动。
 */

#ifndef BSP_UART_H
#define BSP_UART_H

#include <stdint.h>

/* 前向声明: 本头只暴露指针, 不需要把 <zephyr/kernel.h> 拉给所有调用方 */
struct k_pipe;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动板级串口: 等设备就绪 → 设波特率 → 挂中断回调 → 使能接收
 *
 * @param baud 目标波特率 (板级常量, 由 app 层传入, bsp 不内置产品参数)
 * @return 0 成功; -1 设备未就绪或配置失败 (已 printk 原因)
 *
 * @note 中断回调只把收到的字节塞进管道 (经 `bsp_uart_pipe()` 取得), 不做解析,
 *       也不创建线程 —— 消费者由 app 层决定。
 */
int bsp_uart_start(uint32_t baud);

/**
 * @brief 接收管道 (中断 → 线程的单向字节通道)
 *
 * @return 管道对象; 未经 bsp_uart_start() 也可安全读 (空管道)
 *
 * @note 目前 app 层直接用 `k_pipe_read()` 消费; 将来若要彻底屏蔽 Zephyr, 换成
 *       `bsp_uart_read(buf, len, timeout)` 即可, 本 getter 随之删除。
 */
struct k_pipe *bsp_uart_pipe(void);

/** @brief 通讯口裸写: 不做任何模式判断, 谁调用谁负责该不该写 */
void bsp_comm_write(const uint8_t *data, uint32_t len);

/** @brief 调试口裸写: 恒定输出, 与谁在用通讯口无关 */
void bsp_dbg_write(const uint8_t *data, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* BSP_UART_H */
