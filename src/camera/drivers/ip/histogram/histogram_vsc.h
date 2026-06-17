/**
 * @file    histogram_vsc.h
 * @brief   Histogram IP 的 VSC 适配器 — 驱动 + 实例类型定义。
 */

#ifndef HISTOGRAM_VSC_H
#define HISTOGRAM_VSC_H

#include "vsc_core_types.h"
#include "histogram_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { histogram_dev_t hw; } histogram_vsc_inst_t;

extern const vsc_driver_t histogram_vsc_driver;

#ifdef __cplusplus
}
#endif

#endif
