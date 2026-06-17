/**
 * @file    decoder_vsc.h
 * @brief   Decoder IP 的 VSC 适配器 — 驱动 + 实例类型定义。
 *
 * 实例由应用层编译期静态分配，同一 driver 可多实例。
 */

#ifndef DECODER_VSC_H
#define DECODER_VSC_H

#include "vsc_core_types.h"
#include "decoder_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Decoder IP VSC 实例 */
typedef struct { decoder_dev_t hw; } decoder_vsc_inst_t;

extern const vsc_driver_t decoder_vsc_driver;

#ifdef __cplusplus
}
#endif

#endif
