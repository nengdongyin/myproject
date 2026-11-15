#include "file_storage_init.h"
#include "storage_config.h"
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(file_storage_init, LOG_LEVEL_INF);

/**
 * @brief 初始化并挂载所有存储卷
 *
 * 只做"存储资源就绪"。ymodem 收发的文件句柄由 file_transfer 组件在
 * 每次传输时按需 open/close, 这里不再预置任何 fd 或默认文件名 ——
 * 传输目标由参数 PID_FT_RECV_PATH / PID_FT_SEND_PATH 决定。
 */
bool file_storage_init(app_ctx_t *ctx)
{
    (void)ctx;

    if (storage_config_init() != 0)
    {
        LOG_ERR("storage_config_init failed");
        return false;
    }
    return true;
}

void file_storage_deinit(app_ctx_t *ctx)
{
    (void)ctx;
    storage_config_deinit();
}
