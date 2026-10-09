#include "file_storage_init.h"
#include "storage_config.h"
#include <string.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(file_storage_init, LOG_LEVEL_INF);

bool file_storage_init(test_ctx_t *ctx)
{
    /* 初始化并挂载所有存储卷（littlefs / rawflash） */
    if (storage_config_init() != 0)
    {
        LOG_ERR("storage_config_init failed");
        return false;
    }

    /* 文件句柄在每次 ymodem 传输时按需 open/close */
    ctx->recv_fd = VFS_FD_INVALID;
    ctx->send_fd = VFS_FD_INVALID;

    strncpy(ctx->send_file_name, "test_1mb.bin", VFS_NAME_MAX - 1);
    ctx->send_file_name[VFS_NAME_MAX - 1] = '\0';
    return true;
}

void file_storage_deinit(test_ctx_t *ctx)
{
    if (ctx->recv_fd >= 0)
        vfs_close(ctx->recv_fd);
    if (ctx->send_fd >= 0)
        vfs_close(ctx->send_fd);
    ctx->recv_fd = VFS_FD_INVALID;
    ctx->send_fd = VFS_FD_INVALID;

    storage_config_deinit();
}
