/**
 * @file    vsc_lite_board.h
 * @brief   VSC Lite 板级配置文件接口 — 集中定义 FPGA 比特流相关的编译期静态实例。
 *
 * 每个硬件平台 / FPGA 比特流对应一个 .c 实现文件。
 * 更换项目时只需编译链接不同的 board 文件，无需修改管线代码。
 *
 * 使用方法:
 * @code
 *   #include "vsc_lite_board.h"
 *
 *   vsc_lite_pipeline_t pipe;
 *   vsc_lite_pipeline_init(&pipe, vsc_lite_board_stages, vsc_lite_board_num_stages);
 *   vsc_lite_params_module_init(&pipe, vsc_lite_board_stages, vsc_lite_board_num_stages);
 *   param_manager_init();
 * @endcode
 */

#ifndef VSC_LITE_BOARD_H
#define VSC_LITE_BOARD_H

#include "vsc_lite.h"
#include "binning_vsc.h"
#include "crop_vsc.h"
#include "decoder_vsc.h"
#include "histogram_vsc.h"
#include "sensor_vsc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════
 *  板级全局实例 — 由具体 board .c 文件定义
 * ═══════════════════════════════════════════════════════════════════════ */

extern sensor_vsc_inst_t    g_vsc_board_sensor;
extern crop_vsc_inst_t      g_vsc_board_crop;
extern binning_vsc_inst_t   g_vsc_board_binning;
extern decoder_vsc_inst_t   g_vsc_board_decoder;
extern histogram_vsc_inst_t g_vsc_board_histogram;

/* ═══════════════════════════════════════════════════════════════════════
 *  管线配置表 — 由具体 board .c 文件定义
 * ═══════════════════════════════════════════════════════════════════════ */

/** @brief 管线 stage 配置表（driver + 实例） */
extern const vsc_lite_stage_def_t vsc_lite_board_stages[];

/** @brief stage 数量 */
extern const uint8_t vsc_lite_board_num_stages;

#ifdef __cplusplus
}
#endif

#endif /* VSC_LITE_BOARD_H */
