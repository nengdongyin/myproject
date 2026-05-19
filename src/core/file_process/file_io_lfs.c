/**
 * @file file_io_lfs.c
 * @brief LittleFS 直接 I/O 后端实现（基于 LittleFS 原生 API）
 *
 * 绕过 Zephyr 文件系统抽象层，直接调用 lfs_* 函数。
 * 实现 @ref file_io_ops_t 的全部 7 个回调函数。
 *
 * 设计要点：
 * - lfs_t * 由调用方提供，本模块不负责挂载/格式化
 * - lfs_file_t 在 open 时动态分配，close/abort 时释放
 * - 读写偏移由 lfs_t 内部管理，无需额外 seek
 *
 * @section lfs_ctx lfs_ctx_t 生命周期
 * @code
 *   file_io_lfs_create(&lfs) → io->ctx = lfs_ctx_t { .lfs = &lfs, .file = NULL }
 *   lfs_io_open()            → io->ctx->file = malloc(lfs_file_t); lfs_file_open()
 *   lfs_io_close()           → lfs_file_close(); free(file); io->ctx->file = NULL
 * @endcode
 */

#include "file_io_lfs.h"
#include <string.h>
#include <lfs.h>
#include "system_adapter.h"

#define LFS_OPEN_PATH_MAX 256

typedef struct
{
    lfs_t *lfs;                        /**< 文件系统实例指针（由调用方持有） */
    lfs_file_t *file;                  /**< 文件句柄（NULL = 未打开） */
    int open_flags;                    /**< 打开时的 flags（abort 时判断是否需要 unlink） */
    char open_path[LFS_OPEN_PATH_MAX]; /**< 打开时的完整路径（abort 用） */
} lfs_ctx_t;

static int lfs_mode_to_flags(file_io_mode_t mode)
{
    if (mode == FILE_IO_MODE_READ)
    {
        return LFS_O_RDONLY;
    }
    return LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC;
}

/**
 * @brief LFS 后端 open 回调
 *
 * 分配 lfs_file_t 句柄，根据 mode 调用 lfs_file_open。
 *
 * @param io   文件 I/O 实例
 * @param path 文件系统绝对路径
 * @param mode 读/写模式
 * @return 0 成功，-1 失败
 */
static int lfs_io_open(file_io_t *io, const char *path, file_io_mode_t mode)
{
    lfs_ctx_t *ctx = (lfs_ctx_t *)io->ctx;
    if (!ctx || !path)
        return -1;

    lfs_file_t *file = (lfs_file_t *)system_malloc(sizeof(*file));
    if (!file)
        return -1;

    int flags = lfs_mode_to_flags(mode);
    int rc = lfs_file_open(ctx->lfs, file, path, flags);
    if (rc < 0)
    {
        system_free(file);
        return rc;
    }

    strncpy(ctx->open_path, path, LFS_OPEN_PATH_MAX - 1);
    ctx->open_path[LFS_OPEN_PATH_MAX - 1] = '\0';
    ctx->file = file;
    ctx->open_flags = flags;
    return 0;
}

/**
 * @brief LFS 后端 read 回调
 *
 * 先将文件句柄 seek 到 offset，再读取 len 字节。
 *
 * @param io       文件 I/O 实例
 * @param offset   读取起始偏移（字节）
 * @param buf      输出缓冲区
 * @param len      期望读取字节数
 * @param out_read 实际读取字节数（输出参数）
 * @return 0 成功，< 0 失败
 */
static int lfs_io_read(file_io_t *io, uint32_t offset,
                       uint8_t *buf, uint32_t len, uint32_t *out_read)
{
    lfs_ctx_t *ctx = (lfs_ctx_t *)io->ctx;
    if (!ctx || !ctx->file || !buf || !out_read)
        return -1;

    lfs_soff_t seek_rc = lfs_file_seek(ctx->lfs, ctx->file, (lfs_soff_t)offset, LFS_SEEK_SET);
    if (seek_rc < 0)
    {
        *out_read = 0;
        return (int)seek_rc;
    }

    lfs_ssize_t n = lfs_file_read(ctx->lfs, ctx->file, buf, (lfs_size_t)len);
    if (n < 0)
    {
        *out_read = 0;
        return (int)n;
    }

    *out_read = (uint32_t)n;
    return 0;
}

/**
 * @brief LFS 后端 write 回调
 *
 * 调用 lfs_file_write 写入数据，校验实际写入字节数。
 *
 * @param io   文件 I/O 实例
 * @param data 待写入数据
 * @param len  数据长度
 * @return 0 成功（全部写入），-1 部分写入或错误
 */
static int lfs_io_write(file_io_t *io, const uint8_t *data, uint32_t len)
{
    lfs_ctx_t *ctx = (lfs_ctx_t *)io->ctx;
    if (!ctx || !ctx->file || !data || len == 0)
        return -1;

    lfs_ssize_t n = lfs_file_write(ctx->lfs, ctx->file, data, (lfs_size_t)len);
    if (n < 0)
        return (int)n;

    return ((uint32_t)n == len) ? 0 : -1;
}

/**
 * @brief LFS 后端 close 回调
 *
 * 调用 lfs_file_close 关闭文件，释放 lfs_file_t 句柄。
 *
 * @param io  文件 I/O 实例
 * @return 0 成功，< 0 为 lfs_file_close 错误码
 */
static int lfs_io_close(file_io_t *io)
{
    lfs_ctx_t *ctx = (lfs_ctx_t *)io->ctx;
    if (!ctx || !ctx->file)
        return 0;

    int rc = lfs_file_close(ctx->lfs, ctx->file);
    system_free(ctx->file);
    ctx->file = NULL;
    ctx->open_path[0] = '\0';
    return rc;
}

/**
 * @brief LFS 后端 abort 回调
 *
 * 强制关闭文件，若之前是写打开则删除文件（清理未完成的写入）。
 *
 * @param io  文件 I/O 实例
 * @return 始终返回 0
 */
static int lfs_io_abort(file_io_t *io)
{
    lfs_ctx_t *ctx = (lfs_ctx_t *)io->ctx;
    if (!ctx || !ctx->file)
        return 0;

    char path[LFS_OPEN_PATH_MAX];
    strncpy(path, ctx->open_path, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';

    lfs_file_close(ctx->lfs, ctx->file);
    system_free(ctx->file);
    ctx->file = NULL;
    ctx->open_path[0] = '\0';

    if (path[0] != '\0')
    {
        lfs_remove(ctx->lfs, path);
    }

    return 0;
}

/**
 * @brief LFS 后端 get_size 回调
 *
 * 通过 lfs_stat 查询文件大小。
 *
 * @param io       文件 I/O 实例
 * @param path     文件路径
 * @param out_size 输出文件大小
 * @return true 成功，false 失败（文件不存在）
 */
static bool lfs_io_get_size(file_io_t *io, const char *path, uint32_t *out_size)
{
    lfs_ctx_t *ctx = (lfs_ctx_t *)io->ctx;
    if (!ctx || !path || !out_size)
        return false;

    struct lfs_info info;
    int rc = lfs_stat(ctx->lfs, path, &info);
    if (rc < 0)
        return false;

    if (info.type != LFS_TYPE_REG)
        return false;

    *out_size = (uint32_t)info.size;
    return true;
}

/**
 * @brief LFS 后端 remove 回调
 *
 * 直接调用 lfs_remove 删除文件。
 *
 * @param io   文件 I/O 实例
 * @param path 文件路径
 * @return 0 成功，< 0 为 lfs_remove 错误码
 */
static int lfs_io_remove(file_io_t *io, const char *path)
{
    lfs_ctx_t *ctx = (lfs_ctx_t *)io->ctx;
    if (!ctx || !path)
        return -1;

    return lfs_remove(ctx->lfs, path);
}

static const file_io_ops_t g_lfs_ops = {
    .open = lfs_io_open,
    .read = lfs_io_read,
    .write = lfs_io_write,
    .close = lfs_io_close,
    .get_size = lfs_io_get_size,
    .remove = lfs_io_remove,
    .abort = lfs_io_abort,
};

file_io_t *file_io_lfs_create(lfs_t *lfs)
{
    if (!lfs)
        return NULL;

    file_io_t *io = (file_io_t *)system_malloc(sizeof(*io));
    if (!io)
        return NULL;

    lfs_ctx_t *ctx = (lfs_ctx_t *)system_malloc(sizeof(*ctx));
    if (!ctx)
    {
        system_free(io);
        return NULL;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->lfs = lfs;

    io->ops = &g_lfs_ops;
    io->ctx = ctx;
    return io;
}

void file_io_lfs_destroy(file_io_t *io)
{
    if (!io)
        return;

    lfs_ctx_t *ctx = (lfs_ctx_t *)io->ctx;
    if (ctx && ctx->file)
    {
        lfs_file_close(ctx->lfs, ctx->file);
        system_free(ctx->file);
    }
    system_free(io->ctx);
    system_free(io);
}
