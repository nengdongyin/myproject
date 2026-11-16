/**
 * @file mem_regions.h
 * @brief 板级内存白名单 —— 供 md / mw 使用的"可访问物理地址段"
 *
 * @par 资源与代码分离
 * 本文件只描述**本板**哪些地址段可以被调试命令访问, 不含任何访问逻辑
 * (判定在 core/debug/mem_access.c)。换板子只改同目录的 mem_regions.c,
 * 命令实现零改动 —— 与 storage_config.c 的分区/挂载表是同一套分工。
 *
 * @par 默认策略 (保守)
 *   - flash: 只读 (XIP 写要走 flash 编程 API, 不让 mw 直接踩)
 *   - sram : 可读写
 *   - 外设段: **默认不开放** —— 读寄存器常有副作用 (读清标志/弹 FIFO),
 *     写错可能锁总线。需要时在 mem_regions.c 里显式加一行, 并自行承担后果。
 */

#ifndef PLATFORM_BSP_MEM_REGIONS_H
#define PLATFORM_BSP_MEM_REGIONS_H

#include <stddef.h>
#include "mem_access.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 取本板白名单表
 *
 * @param count 输出段数 (可为 NULL)
 * @return 静态段表指针 (勿修改)
 *
 * @note 调用方 (app 启动流程) 负责把它交给 mem_access_configure();
 *       不注入时 md / mw 会因为"没有白名单"而全部拒绝。
 */
const mem_region_t *mem_regions_get(size_t *count);

/**
 * @brief 把本板白名单注入 core/debug 的访问闸门
 *
 * 等价于 `size_t n; mem_access_configure(mem_regions_get(&n), n);`。
 * app 启动阶段调一次即可。
 *
 * @return mem_access_configure 的返回值 (0 成功)
 */
int mem_regions_configure(void);

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_BSP_MEM_REGIONS_H */
