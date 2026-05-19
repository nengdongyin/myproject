#include "imperx_cmd_map.h"
#include "sensor_module.h"
#include "module_ids.h"

static const cmd_map_entry_t g_cmd_map[] = {
    { 0x0010, PID_IP_SENSOR_EXPOSURE },
    { 0x0012, PID_IP_SENSOR_GAIN },
    { 0x0014, PID_IP_SENSOR_FPS },
    { 0x0016, PID_IP_SENSOR_RESOLUTION },
    { 0x0018, PID_IP_SENSOR_ROI_X },
};

static const size_t g_cmd_map_count =
    sizeof(g_cmd_map) / sizeof(g_cmd_map[0]);

const cmd_map_entry_t *cmd_map_lookup(uint16_t addr)
{
    for (size_t i = 0; i < g_cmd_map_count; i++) {
        if (g_cmd_map[i].addr == addr)
            return &g_cmd_map[i];
    }
    return NULL;
}
