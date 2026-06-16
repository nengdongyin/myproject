/**
 * @file    sensor_vsc.h
 * @brief   图像传感器 VSC 适配器 — 桥接 ISC 框架。
 *
 * 这不是纯 HW 驱动（传感器通过 ISC 框架访问，不直接写寄存器）。
 * 适配器持有 ISC 设备句柄，将 VSC ops 委托给 isc_*() API。
 */

#ifndef SENSOR_VSC_H
#define SENSOR_VSC_H

#include "vsc_core_types.h"

extern const vsc_driver_t sensor_imx477_vsc_driver;
void sensor_vsc_reset(void);

#endif
