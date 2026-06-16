/**
 * @file    vsc_driver_ids.h
 * @brief   VSC Driver ID 常量 — VSC Lite 和 VSC 共用
 *
 * 每个 driver 的全局唯一 ID。VSC Lite 独立项目仅需此文件，
 * 不需要 gen/vsc/vsc_prop_ids.h（该文件包含 PROP 定义，属 VSC 完整版）。
 */

#ifndef VSC_DRIVER_IDS_H
#define VSC_DRIVER_IDS_H

#define VSC_DRIVER_ID_SENSOR_IMX477  0x01
#define VSC_DRIVER_ID_SENSOR_IMX296  0x02
#define VSC_DRIVER_ID_CROP           0x03
#define VSC_DRIVER_ID_BINNING        0x04
#define VSC_DRIVER_ID_DECODER        0x05
#define VSC_DRIVER_ID_HISTOGRAM      0x10

#endif /* VSC_DRIVER_IDS_H */
