/**
 * @file    isc_control.h
 * @brief   ISC 控制框架 — CID 定义、控制类型、标志位、控制类
 *
 * CID 结构 (32-bit):
 * ┌──────────────┬──────────────┐
 * │  高 16-bit   │  低 16-bit   │
 * │  控制类      │  控制 ID     │
 * └──────────────┴──────────────┘
 */

#ifndef ISC_CONTROL_H
#define ISC_CONTROL_H

#include <stdint.h>

/* ──── CID 类基址 ──── */
#define ISC_CID_USER_BASE        0x00A00000u  /**< 用户控制: 翻转、测试模式           */
#define ISC_CID_CAMERA_BASE      0x00A10000u  /**< 相机控制: 曝光、增益、帧率、WB      */
#define ISC_CID_SENSOR_BASE      0x00A20000u  /**< 传感器控制: 黑电平/温度/lane/时钟…  */
#define ISC_CID_PRIVATE_BASE     0x08000000u  /**< 厂商私有控制起点                    */

/* ──── 控制类枚举 (用于 isc_ctrl_desc_t.ctrl_class) ──── */
#define ISC_CTRL_CLASS_USER      0x00A00000u
#define ISC_CTRL_CLASS_CAMERA    0x00A10000u
#define ISC_CTRL_CLASS_SENSOR    0x00A20000u

/* ──── 控制类型 ──── */
typedef enum {
    ISC_CTRL_TYPE_INTEGER   = 0,    /**< 有符号整数 → isc_ctrl_value_t.i64         */
    ISC_CTRL_TYPE_BOOLEAN   = 1,    /**< 布尔值     → isc_ctrl_value_t.b           */
    ISC_CTRL_TYPE_ENUM      = 2,    /**< 枚举索引   → isc_ctrl_value_t.i64         */
    ISC_CTRL_TYPE_FLOAT     = 3,    /**< 浮点值     → isc_ctrl_value_t.f           */
} isc_ctrl_type_t;

/* ──── 控制标志位 ──── */
#define ISC_CTRL_FLAG_READ_ONLY     (1u << 0)  /**< 只读                              */
#define ISC_CTRL_FLAG_WRITE_ONLY    (1u << 1)  /**< 只写                              */
#define ISC_CTRL_FLAG_VOLATILE      (1u << 2)  /**< 值可自行变化, get 须真读寄存器      */
#define ISC_CTRL_FLAG_INACTIVE      (1u << 3)  /**< 当前模式下不可用                   */
#define ISC_CTRL_FLAG_STREAMABLE    (1u << 4)  /**< 流中可修改                         */
#define ISC_CTRL_FLAG_NEXT_CTRL     (1u << 5)  /**< 枚举标记: query_ctrl 返回下一项     */

/* ──── 标准控制 ID ──── */

/* 用户控制 (ISC_CID_USER_BASE + N) */
#define ISC_CID_HFLIP              (ISC_CID_USER_BASE + 1)   /**< 水平镜像 (BOOLEAN)      */
#define ISC_CID_VFLIP              (ISC_CID_USER_BASE + 2)   /**< 垂直镜像 (BOOLEAN)      */

/* 相机控制 (ISC_CID_CAMERA_BASE + N) */
#define ISC_CID_EXPOSURE           (ISC_CID_CAMERA_BASE + 1) /**< 曝光时间 ns (INTEGER)   */
#define ISC_CID_ANALOG_GAIN        (ISC_CID_CAMERA_BASE + 2) /**< 模拟增益 × (FLOAT)      */
#define ISC_CID_DIGITAL_GAIN       (ISC_CID_CAMERA_BASE + 3) /**< 数字增益 × (FLOAT)      */
#define ISC_CID_EXPOSURE_AUTO      (ISC_CID_CAMERA_BASE + 4) /**< 自动曝光模式 (ENUM)     */
#define ISC_CID_GAIN_AUTO          (ISC_CID_CAMERA_BASE + 5) /**< 自动增益模式 (ENUM)     */
#define ISC_CID_FRAME_RATE         (ISC_CID_CAMERA_BASE + 6) /**< 帧率 fps (FLOAT)        */

/* 传感器控制 (ISC_CID_SENSOR_BASE + N) */
#define ISC_CID_TEST_PATTERN       (ISC_CID_SENSOR_BASE + 1) /**< 测试图输出 (ENUM)       */
#define ISC_CID_BLACK_LEVEL        (ISC_CID_SENSOR_BASE + 2) /**< 黑电平 DN (INTEGER)     */
#define ISC_CID_TEMPERATURE        (ISC_CID_SENSOR_BASE + 3) /**< 结温 ℃ (FLOAT, 只读)   */
#define ISC_CID_LANE_COUNT         (ISC_CID_SENSOR_BASE + 4) /**< MIPI lane 数 (ENUM)     */
#define ISC_CID_PIXEL_CLOCK        (ISC_CID_SENSOR_BASE + 5) /**< 像素时钟 Hz (INTEGER,只读) */
#define ISC_CID_BIT_DEPTH          (ISC_CID_SENSOR_BASE + 6) /**< 输出位深 (ENUM)         */
#define ISC_CID_LINE_LENGTH        (ISC_CID_SENSOR_BASE + 7) /**< 行长度 pclk (INTEGER,只读) */
#define ISC_CID_FRAME_LENGTH       (ISC_CID_SENSOR_BASE + 8) /**< 帧长度 lines (INTEGER,只读) */

/* ──── ENUM 型控制项取值 ──── */

/* ISC_CID_EXPOSURE_AUTO */
#define ISC_EXPOSURE_AUTO_MANUAL     0   /**< 手动曝光                            */
#define ISC_EXPOSURE_AUTO_ONCE       1   /**< 单次自动曝光                        */
#define ISC_EXPOSURE_AUTO_CONTINUOUS 2   /**< 连续自动曝光                        */

/* ISC_CID_GAIN_AUTO */
#define ISC_GAIN_AUTO_MANUAL         0   /**< 手动增益                            */
#define ISC_GAIN_AUTO_ONCE           1   /**< 单次自动增益                        */
#define ISC_GAIN_AUTO_CONTINUOUS     2   /**< 连续自动增益                        */

#endif /* ISC_CONTROL_H */
