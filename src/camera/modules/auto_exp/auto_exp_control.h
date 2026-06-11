#ifndef AUTO_EXP_CONTROL_H
#define AUTO_EXP_CONTROL_H

#include "app_param_manager.h"
#include "ae_instance.h"
#include "module_ids.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    AE_TARGET_LUMA = 0,
    AE_SPEED       = 1,
    AE_ENABLE      = 2,
    AE_MODEL_NAME  = 3,
    AE_CALIB_DATA  = 4,
};

#define PID_AE_TARGET_LUMA  MAKE_PARAM_ID(MODULE_AUTO_EXP, AE_TARGET_LUMA)
#define PID_AE_SPEED        MAKE_PARAM_ID(MODULE_AUTO_EXP, AE_SPEED)
#define PID_AE_ENABLE       MAKE_PARAM_ID(MODULE_AUTO_EXP, AE_ENABLE)
#define PID_AE_MODEL_NAME   MAKE_PARAM_ID(MODULE_AUTO_EXP, AE_MODEL_NAME)
#define PID_AE_CALIB_DATA   MAKE_PARAM_ID(MODULE_AUTO_EXP, AE_CALIB_DATA)

#define AE_MODEL_NAME_MAX_LEN   31u
#define AE_CALIB_DATA_SIZE      25u

extern ae_instance_t g_ae_instance;

void auto_exp_module_init(void);

#ifdef __cplusplus
}
#endif

#endif /* AUTO_EXP_CONTROL_H */
