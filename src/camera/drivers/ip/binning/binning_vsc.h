/**
 * @file    binning_vsc.h
 * @brief   Binning IP 的 VSC 适配器 — 驱动 + 实例类型定义。
 *
 * 实例由应用层编译期静态分配，同一 driver 可多实例。
 */

#ifndef BINNING_VSC_H
#define BINNING_VSC_H

#include "vsc_core_types.h"
#include "binning_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Binning IP VSC 实例 — 编译期静态分配，持有纯 HW 驱动实例 */
typedef struct { binning_dev_t hw; } binning_vsc_inst_t;

extern const vsc_driver_t binning_vsc_driver;

#ifdef __cplusplus
}
#endif

#endif
