#include "file_storage_init.h"
#include "file_io_fs.h"
#include <string.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(file_storage_init, LOG_LEVEL_INF);

bool file_storage_init(test_ctx_t *ctx)
{
    file_io_fs_init();

    ctx->recv_fp = fp_create(file_io_fs_create(), "/lfs");
    ctx->send_fp = fp_create(file_io_fs_create(), "/lfs");
    strncpy(ctx->send_file_name, "test_1mb.bin", FP_NAME_MAX - 1);
    ctx->send_file_name[FP_NAME_MAX - 1] = '\0';

    if (!ctx->recv_fp || !ctx->send_fp)
    {
        LOG_ERR("Failed to create file processing instances");
        return false;
    }
    return true;
}

void file_storage_deinit(test_ctx_t *ctx)
{
    if (ctx->recv_fp)
        fp_destroy(ctx->recv_fp);
    if (ctx->send_fp)
        fp_destroy(ctx->send_fp);
}
