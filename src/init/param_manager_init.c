#include "param_manager_init.h"
#include <stdio.h>
#include <string.h>
#include "param_manager.h"
#include "lwevt/lwevt.h"
#include "param_storage_flashdb.h"
#include "sensor_module.h"
#include "auto_exp_control.h"
#include "ip_param_manager.h"
#include "param_dump.h"

static void dump_cb(const char *line, void *user_data)
{
    (void)user_data;
    printf("%s\n", line);
}

static volatile uint8_t s_notify_depth = 0;

static void on_param_changed(uint32_t param_id, param_value_t new_value)
{
    if (s_notify_depth > 0)
        return;
    s_notify_depth++;

    lwevt_t evt;
    evt.type = PARAM_EVT_CHANGED;
    evt.msg.param_changed.param_id = param_id;
    memcpy(&evt.msg.param_changed.new_value, &new_value, sizeof(new_value));
    lwevt_dispatch_ex(&evt, PARAM_EVT_CHANGED);

    s_notify_depth--;
}

void param_manager_init(void)
{
    const param_storage_drv_t *storage = param_storage_flashdb_create();
    if (!storage)
        return;
    lwevt_init();
    param_init(storage, on_param_changed);


#ifdef PARAM_MODULE_AUTO_REGISTER
    param_modules_register_all();
#else
    sensor_module_init();
    auto_exp_module_init();
#endif


    int ret = param_load_all();
    if (ret != PARAM_OK)
        printf("[PM] load_all ret=%d\n", ret);

    ret = param_check_flush_integrity();
    if (ret != PARAM_OK)
        printf("[PM] WARNING: MODULE_INIT_ORDER not covered!\n");

    param_value_t fps, exposure_max;
    param_read(PID_IP_SENSOR_FPS, &fps);
    exposure_max.u32 = (uint32_t)(990000u / (fps.u32 + 1));
    param_set_range(PID_IP_SENSOR_EXPOSURE, NULL, &exposure_max);
    printf("[PM] fps=%u -> exposure_max=%u us\n", fps.u32, exposure_max.u32);

    param_validate_all();

    ret = param_flush();

    printf("[PM] init done: load_all=%d flush=%d\n", ret, ret);
    printf("[PM] --- dump after init ---\n");
    param_dump(0, dump_cb, NULL);
}
