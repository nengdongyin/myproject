/**
 * @file    vsc_lite_board_default.c
 * @brief   默认板级配置 — 基于当前 FPGA 比特流 (system_graph.json) 的静态实例。
 *
 * base_addr 来源: drivers/system_graph.json
 *    crop_0      @ 0x43C00000
 *    binning_0   @ 0x43C10000
 *    decoder_0   @ 0x43C20000
 *    histogram_0 @ 0x43C30000
 *
 * 更换 FPGA 比特流时:
 *   1. 复制此文件为 vsc_lite_board_<project>.c
 *   2. 修改各实例的 base_addr 和出厂默认值
 *   3. 调整 stages[] 顺序以匹配新拓扑
 *   4. CMakeLists.txt 中替换编译目标文件
 */

#include "vsc_lite_board.h"

/* ═══════════════════════════════════════════════════════════════════════
 *  编译期静态实例
 *
 *  每个实例对应 FPGA 中的一个 IP 核。base_addr 是编译期常量，
 *  不在运行时通过 init 参数传递——消除了抽象泄露。
 * ═══════════════════════════════════════════════════════════════════════ */

sensor_vsc_inst_t g_vsc_board_sensor = {
    .model = "sensor_imx477",
};

crop_vsc_inst_t g_vsc_board_crop = {
    .hw = {
        .base_addr = 0x43C00000,
        .max_w     = 8192,
        .max_h     = 8192,
        .align_w   = 8,
        .align_h   = 8,
    },
};

binning_vsc_inst_t g_vsc_board_binning = {
    .hw = {
        .base_addr = 0x43C10000,
        .factor_x  = 2,
        .factor_y  = 2,
    },
};

decoder_vsc_inst_t g_vsc_board_decoder = {
    .hw = {
        .base_addr = 0x43C20000,
    },
};

histogram_vsc_inst_t g_vsc_board_histogram = {
    .hw = {
        .base_addr = 0x43C30000,
    },
};

/* ═══════════════════════════════════════════════════════════════════════
 *  管线配置表
 *
 *  stages[] 顺序 = 数据流方向: sensor → crop → binning → decoder → endpoint
 *  同一 driver 可多次出现（携带不同实例），如两个 binning IP:
 *      { &binning_vsc_driver, &g_binning_0 },
 *      { &binning_vsc_driver, &g_binning_1 },
 * ═══════════════════════════════════════════════════════════════════════ */

const vsc_lite_stage_def_t vsc_lite_board_stages[] = {
    { &sensor_imx477_vsc_driver, &g_vsc_board_sensor    },
    { &crop_vsc_driver,          &g_vsc_board_crop      },
    { &binning_vsc_driver,       &g_vsc_board_binning   },
    { &decoder_vsc_driver,       &g_vsc_board_decoder   },
    { &histogram_vsc_driver,     &g_vsc_board_histogram },  /* ANALYZER — TAP 旁路，不参与格式协商 */
};

const uint8_t vsc_lite_board_num_stages =
    sizeof(vsc_lite_board_stages) / sizeof(vsc_lite_board_stages[0]);
