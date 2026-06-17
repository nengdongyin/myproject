/**
 * @file    vsc_lite_control.h
 * @brief   VSC Lite 参数管理模块 — 基于 param_manager 的用户参数持久化与恢复。
 *
 * 模式照搬 auto_exp_control.c / ae_instance.c:
 *   - PARAM_UINT / PARAM_BOOL 定义参数条目（PARAM_FLAG_PERSIST 标记持久化）
 *   - PARAM_TABLE + PARAM_MODULE_DEFINE + PARAM_MODULE_INIT 注册模块
 *   - init 回调中 param_read 恢复用户值 → vsc_lite_set_ctrl 路由到正确 IP
 *     → vsc_lite_try_fmt + vsc_lite_commit_fmt 提交
 *
 * 参数 ID 使用 VSC 能力路由: (cap_id, ctrl_id) → param_id。
 * 同一参数（如 BIN_FACTOR_X）在上电后由 VSC 管线根据当前拓扑路由到
 * sensor 或 FPGA binning IP——用户不感知底层 IP 变化。
 */

#ifndef VSC_LITE_CONTROL_H
#define VSC_LITE_CONTROL_H

#include "app_param_manager.h"
#include "vsc_lite.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── 模块 ID（在 module_ids.h 中分配） ── */
#ifndef MODULE_VSC_LITE
#define MODULE_VSC_LITE  0x03u
#endif

/* ========================================================================
 *  参数 local_id 枚举 — 每个对应一个 (cap_id, ctrl_id) 对
 * ======================================================================== */

enum {
    VSCL_INTENT_WIDTH  = 0,   /* 用户意图宽度                   */
    VSCL_INTENT_HEIGHT = 1,   /* 用户意图高度                   */
    VSCL_INTENT_FORMAT = 2,   /* 用户意图像素格式                */
    VSCL_INTENT_FPS_N  = 3,   /* 用户意图帧率分子                */
    VSCL_INTENT_FPS_D  = 4,   /* 用户意图帧率分母                */
    VSCL_BIN_FACTOR_X  = 5,   /* binning X 方向因子 (1/2/4/8)   */
    VSCL_BIN_FACTOR_Y  = 6,   /* binning Y 方向因子 (1/2/4/8)   */
    VSCL_BIN_ENABLE    = 7,   /* binning 使能                    */
    VSCL_CROP_LEFT     = 8,   /* crop ROI 水平起始偏移           */
    VSCL_CROP_TOP      = 9,   /* crop ROI 垂直起始偏移           */
    VSCL_CROP_WIDTH    = 10,  /* crop 输出宽度                   */
    VSCL_CROP_HEIGHT   = 11,  /* crop 输出高度                   */
    VSCL_COUNT
};

/* ========================================================================
 *  Param ID 宏
 * ======================================================================== */

#define PID_VSCL_INTENT_WIDTH   MAKE_PARAM_ID(MODULE_VSC_LITE, VSCL_INTENT_WIDTH)
#define PID_VSCL_INTENT_HEIGHT  MAKE_PARAM_ID(MODULE_VSC_LITE, VSCL_INTENT_HEIGHT)
#define PID_VSCL_INTENT_FORMAT  MAKE_PARAM_ID(MODULE_VSC_LITE, VSCL_INTENT_FORMAT)
#define PID_VSCL_INTENT_FPS_N   MAKE_PARAM_ID(MODULE_VSC_LITE, VSCL_INTENT_FPS_N)
#define PID_VSCL_INTENT_FPS_D   MAKE_PARAM_ID(MODULE_VSC_LITE, VSCL_INTENT_FPS_D)
#define PID_VSCL_BIN_FACTOR_X   MAKE_PARAM_ID(MODULE_VSC_LITE, VSCL_BIN_FACTOR_X)
#define PID_VSCL_BIN_FACTOR_Y   MAKE_PARAM_ID(MODULE_VSC_LITE, VSCL_BIN_FACTOR_Y)
#define PID_VSCL_BIN_ENABLE     MAKE_PARAM_ID(MODULE_VSC_LITE, VSCL_BIN_ENABLE)
#define PID_VSCL_CROP_LEFT      MAKE_PARAM_ID(MODULE_VSC_LITE, VSCL_CROP_LEFT)
#define PID_VSCL_CROP_TOP       MAKE_PARAM_ID(MODULE_VSC_LITE, VSCL_CROP_TOP)
#define PID_VSCL_CROP_WIDTH     MAKE_PARAM_ID(MODULE_VSC_LITE, VSCL_CROP_WIDTH)
#define PID_VSCL_CROP_HEIGHT    MAKE_PARAM_ID(MODULE_VSC_LITE, VSCL_CROP_HEIGHT)

/* ========================================================================
 *  模块上下文 — 持有管线引用，init 回调中使用
 * ======================================================================== */

typedef struct {
    vsc_lite_pipeline_t *pipe;         /* 已初始化的管线（pipeline_init 之后设置） */
    const vsc_lite_stage_def_t *stages; /* stage 配置表引用                       */
    uint8_t                 num_stages; /* stage 数量                              */
} vsc_lite_control_ctx_t;

/** @brief 全局模块上下文（供应用层在 pipeline_init 后设置 pipe） */
extern vsc_lite_control_ctx_t g_vsc_lite_control_ctx;

/* ========================================================================
 *  公开 API
 * ======================================================================== */

/**
 * @brief 注册 VSC Lite 参数模块到 param_manager。
 *
 * 调用 param_module_register，将参数条目和 init/write 回调注册到框架。
 * 之后 param_manager_init 会自动调用 init 回调从 Flash 恢复参数并提交管线。
 *
 * 调用时机：vsc_lite_pipeline_init 之后、param_manager_init 之前。
 *
 * @param pipe       已初始化的管线
 * @param stages     stage 配置表引用（后续 try_fmt 可能重新协商时使用）
 * @param num_stages stage 数量
 */
void vsc_lite_control_module_init(vsc_lite_pipeline_t *pipe,
                                 const vsc_lite_stage_def_t *stages,
                                 uint8_t num_stages);

#ifdef __cplusplus
}
#endif

#endif /* VSC_LITE_CONTROL_H */
