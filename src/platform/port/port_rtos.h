/**
 * @file port_rtos.h
 * @brief 平台移植层 —— RTOS 线程/栈信息查询 (供 ps / stacks 命令使用)
 *
 * @par 为什么放在 port 层
 * 全工程的纪律是"core/ 不 include 任何 RTOS 头"。线程枚举与栈用量本质上是
 * RTOS 私有能力 (Zephyr 的 k_thread_foreach / FreeRTOS 的 uxTaskGetSystemState),
 * 所以接口声明在这里, 实现落在 port_impl.c 的各 OS 分支里; 命令层
 * (core/debug/rtos_shell.c) 只认本头文件。
 *
 * @par 各后端的前置条件 (不满足时 port_thread_stat 返回 -ENOTSUP)
 *   - Zephyr: 需同时开 `CONFIG_THREAD_MONITOR` + `CONFIG_THREAD_STACK_INFO`
 *     + `CONFIG_INIT_STACKS`。INIT_STACKS 是"历史最大用量"的关键: 栈以 0xAA
 *     填充, 从未被写过的字节仍是 0xAA, 才能反推出峰值。
 *   - FreeRTOS: 需 `configUSE_TRACE_FACILITY = 1`
 *     (usStackHighWaterMark 字段随之可用)。注意 FreeRTOS 的
 *     `uxTaskGetSystemState()` 在**数组装不下全部任务时返回 0**, 因此后端
 *     最多枚举 PORT_FREERTOS_STAT_MAX 个任务。
 *   - RT-Thread / LIBC(PC 测试) / PORT_OS_CUSTOM: 返回 -ENOTSUP。
 *
 * @note `PORT_OS_CUSTOM` 的工程需要自己实现 `port_thread_stat()`,
 *       否则链接 `rtos_shell.c` 时会报未定义 (与 port_impl.c 的既有约定一致:
 *       CUSTOM 分支不编译任何对象代码)。
 *
 * @note 单位一律**字节**。FreeRTOS 的高水位单位是字, 已在实现里换算。
 */

#ifndef PLATFORM_PORT_RTOS_H
#define PLATFORM_PORT_RTOS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 单个线程/任务的快照 */
typedef struct
{
    char     name[16];       /**< 线程名 (超长截断, 始终 '\0' 结尾) */
    char     state[12];      /**< 状态名 ("running"/"ready"/"blocked"...) */
    int32_t  prio;           /**< 当前优先级 (数值越小越高; Zephyr 协作式线程为负) */
    uint32_t stack_size;     /**< 栈总字节数; 0 = 后端不提供 (FreeRTOS) */
    uint32_t stack_free_min; /**< 历史最小剩余字节 → 峰值占用 = size - 本值 */
} port_thread_info_t;

/**
 * @brief 按序取第 idx 个线程快照
 *
 * @param idx 从 0 开始递增, 直到返回 0
 * @param out 输出 (不能为 NULL)
 * @return 1 命中; 0 idx 越界 (遍历结束); -ENOTSUP 当前后端不支持;
 *         -EINVAL out 为 NULL
 */
int port_thread_stat(size_t idx, port_thread_info_t *out);

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_PORT_RTOS_H */
