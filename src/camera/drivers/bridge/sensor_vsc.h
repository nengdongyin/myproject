/**
 * @file    sensor_vsc.h
 * @brief   图像传感器 VSC 适配器 — 驱动 + 实例类型定义。
 *
 * 实例由应用层编译期静态分配。
 */

#ifndef SENSOR_VSC_H
#define SENSOR_VSC_H

#include "vsc_core_types.h"

typedef struct isc_dev_t isc_dev_t;

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sensor_vsc_inst {
    isc_dev_t *isc_dev;
    char       model[32];
    uint32_t   max_h_total;
    uint32_t   max_v_total;
    uint8_t    bin_factor_x;
    uint8_t    bin_factor_y;
    bool       bin_enabled;
} sensor_vsc_inst_t;

extern const vsc_driver_t sensor_imx477_vsc_driver;

#ifdef __cplusplus
}
#endif

#endif
