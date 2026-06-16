/**
 * @file    crop_vsc.h
 * @brief   Crop IP 的 VSC 适配器。
 *
 * 持有 crop_dev_t 实例，实现 vsc_ip_ops_t 四个回调。
 * 注册后 VSC Pipeline 通过标准接口调用。
 */

#ifndef CROP_VSC_H
#define CROP_VSC_H

#include "vsc_core_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 注册到 VSC 框架的 crop 驱动 */
extern const vsc_driver_t crop_vsc_driver;

/** @brief 重置静态池（测试隔离） */
void crop_vsc_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* CROP_VSC_H */
