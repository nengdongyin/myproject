/* ─────────────────────────────────────────────────────────────────────
 *  GENERATED FILE — DO NOT EDIT / 生成文件，请勿手改
 *
 *  源:       params/exp.params.yaml
 *  生成器:   tools/param_gen.py
 *  重新生成: python tools/param_gen.py params gen/params
 *  一致性校验: python tools/param_gen.py params gen/params --check
 * ───────────────────────────────────────────────────────────────────── */
#ifndef EXP_PARAMS_H
#define EXP_PARAMS_H
#include "param_manager.h"
#include "module_ids.h"
/* 模块 ID —— module_ids.h 已登记则用其枚举, 未登记时给模板占位 */
#ifndef MODULE_EXAMPLE_XM
#define MODULE_EXAMPLE_XM 0x7Eu
#endif
/* 参数 ID 宏 —— 模块 exp 的每条参数一个 */
#define PID_EXP_GAIN      MAKE_PARAM_ID(MODULE_EXAMPLE_XM, 0x00)
#define PID_EXP_OFFSET    MAKE_PARAM_ID(MODULE_EXAMPLE_XM, 0x01)
#define PID_EXP_SCALE     MAKE_PARAM_ID(MODULE_EXAMPLE_XM, 0x02)
#define PID_EXP_ENABLED   MAKE_PARAM_ID(MODULE_EXAMPLE_XM, 0x03)
#define PID_EXP_MODE      MAKE_PARAM_ID(MODULE_EXAMPLE_XM, 0x04)
#define PID_EXP_DEV_NAME  MAKE_PARAM_ID(MODULE_EXAMPLE_XM, 0x05)
#define PID_EXP_CALIB     MAKE_PARAM_ID(MODULE_EXAMPLE_XM, 0x06)
#define PID_EXP_RECALIB   MAKE_PARAM_ID(MODULE_EXAMPLE_XM, 0x07)

/* 参数表 (定义在 exp_params.c, 供 PARAM_MODULE_INIT 使用) */
#define EXP_PARAM_COUNT 8
extern param_entry_t *exp_params[EXP_PARAM_COUNT];

#endif /* EXP_PARAMS_H */
