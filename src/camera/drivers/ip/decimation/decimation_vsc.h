/**
 * @file    decimation_vsc.h
 * @brief   Decimation IP 的 VSC 适配器 — 驱动 + 实例类型定义。
 *
 * 实例由应用层编译期静态分配，同一 driver 可多实例。
 */

#ifndef DECIMATION_VSC_H
#define DECIMATION_VSC_H

#include "vsc_core_types.h"
#include "decimation_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Decimation IP VSC 实例 — 编译期静态分配，持有纯 HW 驱动实例 */
typedef struct { decimation_dev_t hw; } decimation_vsc_inst_t;

extern const vsc_driver_t decimation_vsc_driver;

#ifdef __cplusplus
}
#endif

#endif
