/**
 * @file diag_init.h
 * @brief 诊断设施装配 —— 日志 / shell / md-mw 地址白名单
 *
 * @par 为什么单独成文件
 * 这四件事的共同点是"**只服务于人机诊断**", 与协议链、文件系统毫无关系。
 * 它们曾经和协议链装配混在 `protocol_chain_init.c` 里, 导致"调日志级别要打开
 * 协议文件"。现在:
 *   - 装配顺序由编排层 (init_thread.c) 显式给出;
 *   - 谁需要诊断设施, 就调 diag_init() 一次。
 *
 * @par 出口
 * log 与 shell 共用一个扇出出口 `diag_tx` (调试口恒输出 + 通讯口仅 shell 模式),
 * 由本文件内部组合 `bsp_dbg_write()` / `bsp_comm_write()` + `console_mode_get()`
 * 得到 —— “选路”是应用策略, 所以不下沉到 platform/bsp。
 *
 * @par 装配顺序 (在 diag_init 内部固定)
 * 1. 日志 (最早: 后面每一步都可能要打日志)
 * 2. shell (命令入口)
 * 3. md/mw 地址白名单 (在 shell 之后: 只影响命令是否放行, 失败仅告警)
 */

#ifndef DIAG_INIT_H
#define DIAG_INIT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 装配诊断设施 (日志 → shell → 地址白名单)
 *
 * @return true 成功 (白名单失败不算失败: 只告警, md/mw 保持 fail-closed 拒绝);
 *         false 日志或 shell 初始化失败 (那时已没有可用的诊断输出)
 *
 * @note 前置条件: `system_mutex_init()` 已调用、`bsp_uart_start()` 已成功 ——
 *       log/shell 的锁与字节出口都依赖它们。
 * @note 必须在任何 `LOG_*` 之前调用 (包括编排层自己的日志)。
 */
bool diag_init(void);

/**
 * @brief 拆卸诊断设施 (目前只有 shell)
 *
 * @note 只关"人机入口"; 日志不强关 (退出路径上还要打日志)。调用顺序由编排层
 *       保证: 先于协议链的拆卸, 以免还有入口往 shellHandler 里喂字节。
 */
void diag_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* DIAG_INIT_H */
