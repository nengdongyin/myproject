/**
 * @file shell_config.h
 * @brief letter-shell 后端配置覆盖层 (SHELL_CFG_USER)
 *
 * 本头文件**只在** `SHELL_BACKEND_LETTER` 时被使用: `shell_api.h` 里
 * `#define SHELL_CFG_USER "shell_config.h"` 之后 `#include "shell.h"`,
 * letter-shell 的 `shell_cfg.h` 看到 `SHELL_CFG_USER` 就会
 * `#include SHELL_CFG_USER`, 把这里的内容作为默认配置之上的覆盖。
 *
 * 因此本文件与 letter-shell 的 `src/shell_cfg.h` 是同一份"旋钮集合"的两层:
 * 默认值在库内, 覆盖值在这里。**只定义需要偏离默认的项**, 其余沿用库默认,
 * 与 letter-shell 官方 README 推荐的 overlay 方式完全一致。
 *
 * @par 依赖纪律 (与 log_elog_port.c 同构)
 * 本文件只允许 include `port.h` (平台抽象), 不 include 任何 RTOS 头。
 * 换 RTOS 只改 `port_impl.c`, 本文件与 core/shell 业务代码零修改。
 *
 * @par 交互模型 (关键决策)
 * 采用"中断投喂"而非"任务自旋读":
 *   - `SHELL_TASK_WHILE=0`, 且 port 层不设 `shell->read`;
 *   - 字节由 `shell_rx_feed()` 逐字节送入 `shellHandler()` (同步解析);
 *   - `shell_task()` 退化为周期空入口, 裸机主循环 / RTOS 任务均可调用,
 *     因此 core/shell 不关心"有没有 RTOS"、"任务怎么建" —— 高可移植。
 */

#ifndef CORE_SHELL_CONFIG_H
#define CORE_SHELL_CONFIG_H

#include "port.h"

/* ================================================================
 *  平台依赖三项 (必须覆盖, 库默认是空/0)
 * ================================================================ */

/** @brief 系统毫秒 tick —— 双击 tab 补全 help / 自动锁定依赖 */
#define SHELL_GET_TICK()        system_get_time_ms()

/** @brief 内存分配 (仅伴生对象/函数签名/数组参数需要; 先接到平台分配器) */
#define SHELL_MALLOC(size)      system_malloc(size)
#define SHELL_FREE(obj)         system_free(obj)

/** @brief 使能 shell 锁 —— 与日志共享调试串口时防止输出交错
 *  @note 锁实现见 shell_letter_port.c 的 s_lock/s_unlock (复用句柄式互斥)。
 *  @note 必须是非嵌套使用: shellHandler 已持锁时, 命令内不得再次加锁。
 */
#define SHELL_USING_LOCK        1

/* ================================================================
 *  交互模型
 * ================================================================ */

/** @brief 中断投喂模型, 关闭 shellTask 自带 while 自旋 */
#define SHELL_TASK_WHILE        0

/** @brief 命令导出方式 (SHELL_EXPORT_CMD + linker section)
 *  @note 各编译器/构建系统的链接器 keep 规则见 shell_api.h 头注释;
 *        GCC (FreeRTOS / 裸机) 用 shell_sections_gcc.ld, Zephyr 用
 *        shell_sections_zephyr.ld (由 CMake 的 zephyr_linker_sources 注入)。
 */
#define SHELL_USING_CMD_EXPORT  1

/* ================================================================
 *  行为微调 (显式写出, 便于审阅)
 * ================================================================ */

/** @brief 回车: LF 与 CR 同时接受 (串口终端常见, 与库默认一致) */
#define SHELL_ENTER_LF          1
#define SHELL_ENTER_CR          1

/** @brief 管理单个 shell 实例即可 (调试串口专用) */
#define SHELL_MAX_NUMBER        1

/** @brief 默认用户 (无密码) */
#define SHELL_DEFAULT_USER      "root"
#define SHELL_DEFAULT_USER_PASSWORD ""

#endif /* CORE_SHELL_CONFIG_H */
