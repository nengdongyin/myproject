#include "auto_exp_control.h"
#include <string.h>

ae_instance_t g_ae_instance;

static int ae_apply(uint32_t param_id, param_value_t value)
{
    switch (param_id) {
    case PID_AE_TARGET_LUMA:
        ae_instance_set_target_luma(&g_ae_instance, (uint8_t)value.u32);
        break;
    case PID_AE_SPEED:
        ae_instance_set_speed(&g_ae_instance, value.f32);
        break;
    case PID_AE_ENABLE:
        ae_instance_set_enable(&g_ae_instance, value.b);
        break;
    case PID_AE_MODEL_NAME:
    case PID_AE_CALIB_DATA:
        break;
    default:
        return PARAM_ERR_INVALID_ID;
    }
    return PARAM_OK;
}

static int ae_flush(void *ctx)
{
    (void)ctx;
    return PARAM_OK;
}

static int ae_init(void *ctx)
{
    ae_instance_t *ae = (ae_instance_t *)ctx;

    ae_instance_init(ae);

    param_value_t v;
    param_read(PID_AE_TARGET_LUMA, &v);
    ae_instance_set_target_luma(ae, (uint8_t)v.u32);

    param_read(PID_AE_SPEED, &v);
    ae_instance_set_speed(ae, v.f32);

    param_read(PID_AE_ENABLE, &v);
    ae_instance_set_enable(ae, v.b);

    return PARAM_OK;
}


static char     g_ae_model_name_buf[AE_MODEL_NAME_MAX_LEN + 1] = "OV4689-SENSOR";
static uint8_t  g_ae_calib_buf[AE_CALIB_DATA_SIZE] = {
    1, 1, 1, 1, 1,
    1, 2, 2, 2, 1,
    1, 2, 4, 2, 1,
    1, 2, 2, 2, 1,
    1, 1, 1, 1, 1,
};
PARAM_UINT (ae_target_luma, PID_AE_TARGET_LUMA, PARAM_FLAG_PERSIST, 128, 0, 255);
PARAM_FLOAT(ae_speed,       PID_AE_SPEED,        PARAM_FLAG_PERSIST, 0.5f, 0.01f, 1.0f);
PARAM_BOOL (ae_enable,      PID_AE_ENABLE,        PARAM_FLAG_PERSIST, true);
PARAM_STRING(ae_model_name, PID_AE_MODEL_NAME, PARAM_FLAG_PERSIST,
             g_ae_model_name_buf, AE_MODEL_NAME_MAX_LEN);
PARAM_BLOB  (ae_calib_data, PID_AE_CALIB_DATA, PARAM_FLAG_PERSIST,
             g_ae_calib_buf, AE_CALIB_DATA_SIZE);

PARAM_TABLE(ae_params,
    &ae_target_luma.base,
    &ae_speed.base,
    &ae_enable.base,
    &ae_model_name.base,
    &ae_calib_data.base,
);

PARAM_MODULE_DEFINE(auto_exp, MODULE_AUTO_EXP, "AutoExposure",
                    &g_ae_instance, ae_init, NULL, ae_apply, NULL, ae_flush);

void auto_exp_module_init(void)
{
    param_module_register(&auto_exp_module,
                          ae_params,
                          PARAM_COUNT(ae_params));
}
