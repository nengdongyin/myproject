/**
 * @file    vsc_ctrl_ids.h
 * @brief   VSC 控制 ID 定义 — 对标 ISC cid 命名空间
 *
 * 约定:
 *   - 每个 IP 类别占用一个 0x1000 对齐的段，互不重叠
 *   - 段内编号从 0x000 开始递增
 *   - 新增 IP 时在对应的 VSC_CAP_* 段中追加；若无对应段则新开一段
 *
 * 段分配:
 *   0x0000  — 通用控制
 *   0x1000  — Binning
 *   0x2000  — Crop
 *   0x3000  — Sensor
 *   0x4000  — Decoder
 *   0x5000  — Flip/Mirror（预留）
 *   0x6000  — LUT/Color（预留）
 *   0x7000  — Output IF / CameraLink（预留）
 */

#ifndef VSC_CTRL_IDS_H
#define VSC_CTRL_IDS_H

/* ── 0x0000: 通用 ── */
#define VSC_CTRL_ENABLE             0x0000   /* 通用使能 (0=关, 1=开)           */

/* ── 0x1000: Binning ── */
#define VSC_CTRL_BIN_FACTOR_X       0x1000   /* X 方向缩减因子                  */
#define VSC_CTRL_BIN_FACTOR_Y       0x1001   /* Y 方向缩减因子                  */
#define VSC_CTRL_BIN_ENABLE         0x1002   /* binning 使能                    */

/* ── 0x2000: Crop ── */
#define VSC_CTRL_CROP_WIDTH         0x2000   /* 输出宽度                        */
#define VSC_CTRL_CROP_HEIGHT        0x2001   /* 输出高度                        */
#define VSC_CTRL_CROP_LEFT          0x2002   /* 水平起始列                      */
#define VSC_CTRL_CROP_TOP           0x2003   /* 垂直起始行                      */

/* ── 0x3000: Sensor ── */
#define VSC_CTRL_SENSOR_EXPOSURE    0x3000   /* 曝光时间                        */
#define VSC_CTRL_SENSOR_GAIN        0x3001   /* 增益                            */

/* ── 0x4000: Decoder ── */
#define VSC_CTRL_DECODER_TARGET_FMT 0x4000   /* 输出像素格式                    */

#endif /* VSC_CTRL_IDS_H */
