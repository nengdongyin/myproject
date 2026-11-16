#include "imperx_cmd_map.h"
#include "sensor_module.h"
#include "module_ids.h"
#include "file_transfer.h"
#include "console_mode.h"

static const cmd_map_entry_t g_cmd_map[] = {
    { 0x0010, PID_IP_SENSOR_EXPOSURE },
    { 0x0012, PID_IP_SENSOR_GAIN },
    { 0x0014, PID_IP_SENSOR_FPS },
    { 0x0016, PID_IP_SENSOR_RESOLUTION },
    { 0x0018, PID_IP_SENSOR_ROI_X },

    /* 文件传输命令 —— 写这两个地址即触发对应 EXEC 参数:
     *   WRITE 0xF000 → PID_FT_RECV_START (启动 ymodem 接收)
     *   WRITE 0xF001 → PID_FT_SEND_START (启动 ymodem 发送)
     *
     * param_write_raw 对 EXEC 参数自动路由到模块的 exec 回调,
     * 因此协议层不需要任何专用命令通道 —— "旋钮即参数"。 */
    { 0xF000, PID_FT_RECV_START },
    { 0xF001, PID_FT_SEND_START },

    /* 通信串口复用 —— 写这个地址即把 uart1 交给 shell:
     *   WRITE 0xF003 → shell 模式 (交互式命令行, 应急调试)
     * 与上面两条一样走 param_write_raw → EXEC 回调, 不需要专用命令通道。
     * 回程只有一条路: shell 内敲 `uart_mode proto` (切到 shell 后协议帧
     * 已不被解析, 再开一个"切回协议"的地址也接不到)。
     * 详见 core/console/console_mode.h。 */
    { 0xF003, PID_CONSOLE_ENTER_SHELL },
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
