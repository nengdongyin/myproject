#include "sensor_module.h"
#include <string.h>
#include <stdio.h>
#include "param_event_lwevt.h"

typedef struct {
    uint32_t base_addr;
    bool     initialized;
    uint32_t reg_shadow[8];
} sensor_device_t;
static sensor_device_t g_sensor_dev = { .base_addr = 0x40020000 };

static void sensor_device_hw_init(sensor_device_t *dev)
{
    if (dev->initialized) return;
    memset(dev->reg_shadow, 0, sizeof(dev->reg_shadow));
    dev->initialized = true;
}

static uint32_t sensor_reg_read(sensor_device_t *s, uint32_t offset)
{
    return s->reg_shadow[offset / 4];
}

static void sensor_reg_write(sensor_device_t *s, uint32_t offset, uint32_t val)
{
    s->reg_shadow[offset / 4] = val;
}

static void sensor_on_param_changed(lwevt_t *evt)
{
    if (evt->type != PARAM_EVT_CHANGED)
        return;

    uint32_t      id = evt->msg.param_changed.param_id;
    param_value_t v  = param_event_get_value(evt);

    switch (id) {
    case PID_IP_SENSOR_EXPOSURE:
        printf("[sensor] exposure changed: %u us\n", v.u32);
        break;
    case PID_IP_SENSOR_GAIN:
        printf("[sensor] gain changed: %.2f\n", (double)v.f32);
        break;
    case PID_IP_SENSOR_FPS:
        printf("[sensor] fps changed: %u\n", v.u32);
        param_value_t exposure_max;
        exposure_max.u32 = (uint32_t)(990000u / (v.u32 + 1));
        param_set_range(PID_IP_SENSOR_EXPOSURE, NULL, &exposure_max);
        break;
    default:
        break;
    }
}

static int sensor_read(void *drv, uint32_t param_id, param_value_t *value)
{
    sensor_device_t *s = (sensor_device_t *)drv;

    switch (param_id) {
    case PID_IP_SENSOR_EXPOSURE:
        value->u32 = sensor_reg_read(s, 0x00);
        break;
    case PID_IP_SENSOR_GAIN:
        value->f32 = *(float *)&s->reg_shadow[1];
        break;
    case PID_IP_SENSOR_FPS:
        value->u32 = sensor_reg_read(s, 0x08);
        break;
    case PID_IP_SENSOR_RESOLUTION:
        value->i32 = (int32_t)sensor_reg_read(s, 0x0C);
        break;
    case PID_IP_SENSOR_SHUTDOWN:
        value->b = (sensor_reg_read(s, 0x10) & 1u) != 0;
        break;
    case PID_IP_SENSOR_ROI_X:
        value->u32 = sensor_reg_read(s, 0x14);
        break;
    case PID_IP_SENSOR_FRAME_CNT:
        value->u32 = sensor_reg_read(s, 0x18);
        break;
    case PID_IP_SENSOR_CUR_LUMA:
        value->u32 = sensor_reg_read(s, 0x1C);
        break;
    default:
        return PARAM_ERR_INVALID_ID;
    }
    return PARAM_OK;
}

static int sensor_write(void *drv, uint32_t param_id, param_value_t value)
{
    sensor_device_t *s = (sensor_device_t *)drv;

    switch (param_id) {
    case PID_IP_SENSOR_EXPOSURE:
        sensor_reg_write(s, 0x00, value.u32);
        param_save_one(PID_IP_SENSOR_EXPOSURE);
        break;
    case PID_IP_SENSOR_GAIN:
        s->reg_shadow[1] = *(uint32_t *)&value.f32;
        break;
    case PID_IP_SENSOR_FPS:
        sensor_reg_write(s, 0x08, value.u32);
        break;
    case PID_IP_SENSOR_RESOLUTION:
        sensor_reg_write(s, 0x0C, value.u32);
        break;
    case PID_IP_SENSOR_SHUTDOWN: {
        uint32_t ctrl = sensor_reg_read(s, 0x10);
        if (value.b) ctrl |= 1u; else ctrl &= ~1u;
        sensor_reg_write(s, 0x10, ctrl);
        break;
    }
    case PID_IP_SENSOR_ROI_X:
        sensor_reg_write(s, 0x14, value.u32);
        break;
    case PID_IP_SENSOR_FRAME_CNT:
        sensor_reg_write(s, 0x18, value.u32);
        break;
    case PID_IP_SENSOR_CUR_LUMA:
        sensor_reg_write(s, 0x1C, value.u32);
        break;
    default:
        return PARAM_ERR_INVALID_ID;
    }
    return PARAM_OK;
}

static int sensor_init_cb(void *drv)
{
    sensor_device_t *dev = (sensor_device_t *)drv;

    sensor_device_hw_init(dev);

    param_value_t v;
    param_read(PID_IP_SENSOR_EXPOSURE, &v);
    sensor_reg_write(dev, 0x00, v.u32);

    param_read(PID_IP_SENSOR_GAIN, &v);
    dev->reg_shadow[1] = *(uint32_t *)&v.f32;

    param_read(PID_IP_SENSOR_FPS, &v);
    sensor_reg_write(dev, 0x08, v.u32);

    param_read(PID_IP_SENSOR_RESOLUTION, &v);
    sensor_reg_write(dev, 0x0C, v.u32);

    param_read(PID_IP_SENSOR_ROI_X, &v);
    sensor_reg_write(dev, 0x14, v.u32);

    return PARAM_OK;
}
static int sensor_exec(uint32_t param_id, param_value_t arg)
{
    (void)arg;
    switch (param_id) {
    case PID_IP_SENSOR_SHUTDOWN:
        sensor_device_hw_init(&g_sensor_dev);
        return PARAM_OK;
    default:
        return PARAM_ERR_NOT_FOUND;
    }
}


PARAM_IP_UINT (ip_sensor_exposure,   PID_IP_SENSOR_EXPOSURE,   PARAM_FLAG_PERSIST, 10000,  10000, 10000);
PARAM_IP_FLOAT(ip_sensor_gain,       PID_IP_SENSOR_GAIN,       PARAM_FLAG_PERSIST, 1.0f,   1.0f,  1.0f);
PARAM_IP_UINT (ip_sensor_fps,        PID_IP_SENSOR_FPS,        PARAM_FLAG_PERSIST, 30,     30,    30);
PARAM_IP_INT  (ip_sensor_resolution, PID_IP_SENSOR_RESOLUTION, PARAM_FLAG_PERSIST, 1,      1,     1);
PARAM_IP_EXEC (ip_sensor_shutdown,   PID_IP_SENSOR_SHUTDOWN);
PARAM_IP_UINT (ip_sensor_roi_x,      PID_IP_SENSOR_ROI_X,      PARAM_FLAG_PERSIST, 1920,   1920,  1920);
PARAM_IP_UINT (ip_sensor_frame_cnt,  PID_IP_SENSOR_FRAME_CNT,  PARAM_FLAG_READONLY, 0,      0,     0);
PARAM_IP_UINT (ip_sensor_cur_luma,  PID_IP_SENSOR_CUR_LUMA,   PARAM_FLAG_READONLY, 0,      0,     0);

PARAM_TABLE(sensor_params,
    &ip_sensor_exposure.base,
    &ip_sensor_gain.base,
    &ip_sensor_fps.base,
    &ip_sensor_resolution.base,
    &ip_sensor_shutdown.base,
    &ip_sensor_roi_x.base,
    &ip_sensor_frame_cnt.base,
    &ip_sensor_cur_luma.base,
);

IP_DRIVER_DEFINE(sensor, IP_SENSOR, "OV4689_Sensor_IP",
                 &g_sensor_dev, sensor_read, sensor_write, NULL);

void sensor_module_init(void)
{
    sensor_instance.init_cb  = sensor_init_cb;
    sensor_instance.node.exec_cb = sensor_exec;
    ip_driver_register(&sensor_instance,
                       sensor_params,
                       PARAM_COUNT(sensor_params));

    lwevt_register(sensor_on_param_changed);
}
