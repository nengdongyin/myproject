/**
 * @file    vsc_lite_control.c
 * @brief   VSC Lite 参数管理模块实现。
 *
 * 模式照搬 auto_exp_control.c：
 *   - PARAM_UINT / PARAM_BOOL → 参数条目
 *   - PARAM_TABLE → 参数表
 *   - PARAM_MODULE_DEFINE → 模块定义
 *   - vsc_lite_control_module_init() → param_module_register
 *
 * 参数恢复流程（init 回调中）：
 *   1. param_read 逐参数从 Flash cache 读取用户保存值
 *   2. intent 参数组装为 vsc_mbus_fmt_t
 *   3. 控制参数通过 vsc_lite_set_ctrl 路由到正确 IP
 *   4. vsc_lite_try_fmt + vsc_lite_commit_fmt 提交管线
 */

#include "vsc_lite_control.h"
#include "vsc_ctrl_ids.h"
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════
 *  param local_id → (cap_id, ctrl_id, default) 映射表
 *
 *  VSCL_INTENT_* 项的 cap_id/ctrl_id 为 0——它们在 init 中特殊处理。
 * ═══════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t cap_id;
    uint32_t ctrl_id;
} vsc_lite_param_map_t;

static const vsc_lite_param_map_t s_map[VSCL_COUNT] = {
    [VSCL_BIN_FACTOR_X]  = { VSC_CAP_BINNING, VSC_CTRL_BIN_FACTOR_X },
    [VSCL_BIN_FACTOR_Y]  = { VSC_CAP_BINNING, VSC_CTRL_BIN_FACTOR_Y },
    [VSCL_BIN_ENABLE]    = { VSC_CAP_BINNING, VSC_CTRL_BIN_ENABLE   },
    [VSCL_CROP_LEFT]     = { VSC_CAP_CROP,    VSC_CTRL_CROP_LEFT    },
    [VSCL_CROP_TOP]      = { VSC_CAP_CROP,    VSC_CTRL_CROP_TOP     },
    [VSCL_CROP_WIDTH]    = { VSC_CAP_CROP,    VSC_CTRL_CROP_WIDTH   },
    [VSCL_CROP_HEIGHT]   = { VSC_CAP_CROP,    VSC_CTRL_CROP_HEIGHT  },
};

/* ═══════════════════════════════════════════════════════════════════════
 *  全局上下文
 * ═══════════════════════════════════════════════════════════════════════ */

vsc_lite_control_ctx_t g_vsc_lite_control_ctx;

/* ═══════════════════════════════════════════════════════════════════════
 *  write 回调 — 用户通过 param_write / 命令行设置参数时触发
 * ═══════════════════════════════════════════════════════════════════════ */

static int vsc_lite_write(void *ctx_ptr, uint32_t param_id, param_value_t value)
{
    vsc_lite_control_ctx_t *ctx = (vsc_lite_control_ctx_t *)ctx_ptr;
    uint16_t loc = PARAM_LOCAL_ID(param_id);

    if (loc >= VSCL_COUNT || !ctx->pipe)
        return PARAM_ERR_INVALID_ID;

    /* 所有参数均为 intent 的一部分 — 仅更新 cache，由 try_fmt 统一协商 */
    (void)ctx;
    return PARAM_OK;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  read 回调 — 从管线读取当前控制值
 * ═══════════════════════════════════════════════════════════════════════ */

static int vsc_lite_read(void *ctx_ptr, uint32_t param_id, param_value_t *value)
{
    vsc_lite_control_ctx_t *ctx = (vsc_lite_control_ctx_t *)ctx_ptr;
    uint16_t loc = PARAM_LOCAL_ID(param_id);

    if (loc >= VSCL_COUNT || !ctx->pipe || !value)
        return PARAM_ERR_INVALID_ID;

    (void)loc;
    return PARAM_ERR_INVALID_ID;  /* 所有参数均从 param_manager cache 读取 */
}

/* ═══════════════════════════════════════════════════════════════════════
 *  init 回调 — param_manager 从 Flash 恢复持久化参数后调用
 *
 *  此时 ctx->pipe 已就绪（pipeline_init 在 param_manager_init 之前完成）。
 *  逐参数 param_read → set_ctrl 路由 → try_fmt + commit_fmt 提交。
 * ═══════════════════════════════════════════════════════════════════════ */

static int vsc_lite_init(void *ctx_ptr)
{
    vsc_lite_control_ctx_t *ctx = (vsc_lite_control_ctx_t *)ctx_ptr;
    param_value_t v;
    int rc;

    if (!ctx->pipe || !ctx->stages || ctx->num_stages < 2)
        return PARAM_ERR_INVALID_ID;

    /* ── 1. 从 Flash 恢复完整 intent（含 bin/crop 字段）─── */
    vsc_mbus_fmt_t intent;
    memset(&intent, 0, sizeof(intent));
    intent.spatial.pixel_format   = VSC_FMT_RAW10;
    intent.spatial.frame_rate_num = 30;
    intent.spatial.frame_rate_den = 1;
    intent.spatial.bit_depth      = 10;
    intent.spatial.lanes          = 4;
    intent.spatial.bin_x          = 1;
    intent.spatial.bin_y          = 1;
    intent.spatial.dec_x          = 1;
    intent.spatial.dec_y          = 1;

    if (param_read(PID_VSCL_INTENT_WIDTH,  &v) == PARAM_OK) intent.spatial.width          = v.u32;
    if (param_read(PID_VSCL_INTENT_HEIGHT, &v) == PARAM_OK) intent.spatial.height         = v.u32;
    if (param_read(PID_VSCL_INTENT_FORMAT, &v) == PARAM_OK) intent.spatial.pixel_format   = v.u32;
    if (param_read(PID_VSCL_INTENT_FPS_N,  &v) == PARAM_OK) intent.spatial.frame_rate_num = v.u32;
    if (param_read(PID_VSCL_INTENT_FPS_D,  &v) == PARAM_OK) intent.spatial.frame_rate_den = v.u32;

    /* bin/crop 字段直接作为 intent 的一部分，不再通过 set_ctrl 单独路由 */
    if (param_read(PID_VSCL_BIN_FACTOR_X,  &v) == PARAM_OK) intent.spatial.bin_x     = (uint8_t)v.u32;
    if (param_read(PID_VSCL_BIN_FACTOR_Y,  &v) == PARAM_OK) intent.spatial.bin_y     = (uint8_t)v.u32;
    if (param_read(PID_VSCL_CROP_LEFT,     &v) == PARAM_OK) intent.spatial.offsetx = v.u32;
    if (param_read(PID_VSCL_CROP_TOP,      &v) == PARAM_OK) intent.spatial.offsety = v.u32;
    if (param_read(PID_VSCL_DEC_FACTOR_X,  &v) == PARAM_OK) intent.spatial.dec_x     = (uint8_t)v.u32;
    if (param_read(PID_VSCL_DEC_FACTOR_Y,  &v) == PARAM_OK) intent.spatial.dec_y     = (uint8_t)v.u32;

    /* ── 3. 格式协商 + 提交 ── */
    vsc_resolver_result_t result;
    rc = vsc_lite_try_fmt(ctx->pipe, &intent, &result);
    if (rc != VSC_OK) return PARAM_ERR_INVALID_ID;

    rc = vsc_lite_commit_fmt(ctx->pipe, &result.primary_fmt);
    return (rc == VSC_OK) ? PARAM_OK : PARAM_ERR_INVALID_ID;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  flush 回调
 * ═══════════════════════════════════════════════════════════════════════ */

static int vsc_lite_flush(void *ctx_ptr)
{
    (void)ctx_ptr;
    return PARAM_OK;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  参数条目定义
 * ═══════════════════════════════════════════════════════════════════════ */

PARAM_UINT(vscl_intent_width,  PID_VSCL_INTENT_WIDTH,  PARAM_FLAG_PERSIST, 1920,         64, 8192);
PARAM_UINT(vscl_intent_height, PID_VSCL_INTENT_HEIGHT, PARAM_FLAG_PERSIST, 1080,          4, 8192);
PARAM_UINT(vscl_intent_format, PID_VSCL_INTENT_FORMAT, PARAM_FLAG_PERSIST, VSC_FMT_RAW10, 0, 0);
PARAM_UINT(vscl_intent_fps_n,  PID_VSCL_INTENT_FPS_N,  PARAM_FLAG_PERSIST, 30,            1, 1000);
PARAM_UINT(vscl_intent_fps_d,  PID_VSCL_INTENT_FPS_D,  PARAM_FLAG_PERSIST, 1,             1, 1000);
PARAM_UINT(vscl_bin_factor_x,  PID_VSCL_BIN_FACTOR_X,  PARAM_FLAG_PERSIST, 2,             1, 8);
PARAM_UINT(vscl_bin_factor_y,  PID_VSCL_BIN_FACTOR_Y,  PARAM_FLAG_PERSIST, 2,             1, 8);
PARAM_BOOL(vscl_bin_enable,    PID_VSCL_BIN_ENABLE,    PARAM_FLAG_PERSIST, true);
PARAM_UINT(vscl_crop_left,     PID_VSCL_CROP_LEFT,     PARAM_FLAG_PERSIST, 0,             0, 8192);
PARAM_UINT(vscl_crop_top,      PID_VSCL_CROP_TOP,      PARAM_FLAG_PERSIST, 0,             0, 8192);
PARAM_UINT(vscl_crop_width,    PID_VSCL_CROP_WIDTH,    PARAM_FLAG_PERSIST, 1920,         64, 8192);
PARAM_UINT(vscl_crop_height,   PID_VSCL_CROP_HEIGHT,   PARAM_FLAG_PERSIST, 1080,          4, 8192);
PARAM_UINT(vscl_dec_factor_x,  PID_VSCL_DEC_FACTOR_X,  PARAM_FLAG_PERSIST, 1,             1, 8);
PARAM_UINT(vscl_dec_factor_y,  PID_VSCL_DEC_FACTOR_Y,  PARAM_FLAG_PERSIST, 1,             1, 8);

PARAM_TABLE(vsc_lite_control_params,
    &vscl_intent_width.base,
    &vscl_intent_height.base,
    &vscl_intent_format.base,
    &vscl_intent_fps_n.base,
    &vscl_intent_fps_d.base,
    &vscl_bin_factor_x.base,
    &vscl_bin_factor_y.base,
    &vscl_bin_enable.base,
    &vscl_crop_left.base,
    &vscl_crop_top.base,
    &vscl_crop_width.base,
    &vscl_crop_height.base,
    &vscl_dec_factor_x.base,
    &vscl_dec_factor_y.base,
);

PARAM_MODULE_DEFINE(vsc_lite, MODULE_VSC_LITE, "VSC-Lite-Pipeline",
                    &g_vsc_lite_control_ctx,
                    vsc_lite_init, vsc_lite_read, vsc_lite_write,
                    NULL, vsc_lite_flush);

/* ═══════════════════════════════════════════════════════════════════════
 *  公开 API
 * ═══════════════════════════════════════════════════════════════════════ */

void vsc_lite_control_module_init(vsc_lite_pipeline_t *pipe,
                                 const vsc_lite_stage_def_t *stages,
                                 uint8_t num_stages)
{
    memset(&g_vsc_lite_control_ctx, 0, sizeof(g_vsc_lite_control_ctx));
    g_vsc_lite_control_ctx.pipe       = pipe;
    g_vsc_lite_control_ctx.stages     = stages;
    g_vsc_lite_control_ctx.num_stages = num_stages;

    param_module_register(&vsc_lite_module,
                          vsc_lite_control_params,
                          PARAM_COUNT(vsc_lite_control_params));
}
