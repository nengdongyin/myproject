/* ─────────────────────────────────────────────────────────────────────
 *  GENERATED FILE — DO NOT EDIT / 生成文件，请勿手改
 *
 *  源:       params/exp.params.yaml
 *  生成器:   tools/param_gen.py
 *  重新生成: python tools/param_gen.py params gen/params
 *  一致性校验: python tools/param_gen.py params gen/params --check
 * ───────────────────────────────────────────────────────────────────── */
#include "app_param_manager.h"
#include "module_ids.h"
#include "exp_params.h"

/* module.id 与 module_ids.h 的枚举值必须一致 —— 漂移即编译失败 */
_Static_assert(MODULE_EXAMPLE_XM == 0x7E,
               "params/exp.params.yaml: module.id 与 MODULE_EXAMPLE_XM 已漂移, 请同步");

/* ── ENUM 合法值表 ── */
static const int32_t s_exp_mode_values[] = {0, 1, 2};

/* ── STRING / BLOB 缓冲区 (单缓冲: 静态可写, 生命周期 = 运行期) ── */
static char g_exp_name[16] = "example";
static uint8_t g_exp_calib[8] = {0};

/* ── 参数条目 (公开宏调用, 与手写代码同形) ── */
PARAM_APP_UINT(gain, PID_EXP_GAIN, PARAM_FLAG_READONLY, 10, 1, 100);

PARAM_APP_INT(offset, PID_EXP_OFFSET, PARAM_FLAG_PERSIST, 0, -50, 50);

PARAM_APP_FLOAT(scale, PID_EXP_SCALE, PARAM_FLAG_PERSIST, 1.0f, 0.1f, 10.0f);

PARAM_APP_BOOL(enabled, PID_EXP_ENABLED, PARAM_FLAG_PERSIST, true);

PARAM_APP_ENUM(mode, PID_EXP_MODE, PARAM_FLAG_PERSIST, 0, s_exp_mode_values, 3);

PARAM_APP_STRING(dev_name, PID_EXP_DEV_NAME, PARAM_FLAG_PERSIST, g_exp_name, sizeof(g_exp_name) - 1);

PARAM_APP_BLOB(calib, PID_EXP_CALIB, PARAM_FLAG_PERSIST, g_exp_calib, sizeof(g_exp_calib));

PARAM_APP_EXEC(recalib, PID_EXP_RECALIB);

/* ── 参数表 ── */
param_entry_t *exp_params[EXP_PARAM_COUNT] = {
    &gain.base,
    &offset.base,
    &scale.base,
    &enabled.base,
    &mode.base,
    &dev_name.base,
    &calib.base,
    &recalib.base,
};
