/**
 * @file file_io_fs.c
 * @brief 文件系统 I/O 后端实现（基于 Zephyr FS API）
 *
 * 管理最多 @ref FS_MAX_OPEN 个文件描述符（fs_fd_t）的分配与回收，
 * 并实现 @ref file_io_ops_t 的全部 7 个回调函数。
 *
 * 线程安全：fd_alloc() 和 fd_free() 通过 LOCK/UNLOCK 宏保护。
 *
 * @section fd_pool 文件描述符池
 * 使用固定大小的静态数组 g_fs_io.fds[] 作为 fd 池，
 * 通过 allocated 标记位实现 O(n) 查找。池大小 FS_MAX_OPEN = 2。
 */

#include "file_io_fs.h"
#include <zephyr/fs/fs.h>
#include <string.h>
#include "port.h"

/**
 * @brief 最多同时打开的文件数
 * @note 若需要更多的并发文件操作，增大此值并评估栈/堆影响
 */
#define FS_MAX_OPEN 2

/**
 * @brief 已打开路径的缓冲区大小（用于 abort 时 unlink）
 */
#define FS_OPEN_PATH_MAX 256

/**
 * @brief 文件描述符结构体
 *
 * 封装 Zephyr fs_file_t 及元数据。分配状态由 allocated 标记管理，
 * 关联的路径在 abort 时用于 fs_unlink 删除临时文件。
 */
typedef struct
{
    int allocated;                    /**< 分配标记：1=已分配，0=空闲 */
    int idx;                          /**< 在 fds[] 中的索引（冗余但便于释放） */
    char open_path[FS_OPEN_PATH_MAX]; /**< 打开时的完整路径（abort 用） */
    struct fs_file_t fd;              /**< Zephyr 文件句柄 */
} fs_fd_t;

/**
 * @brief 全局文件描述符池
 *
 * 静态分配，由 file_io_fs_init() 初始化（清零），所有 I/O 操作共享此池。
 */
static struct
{
    fs_fd_t fds[FS_MAX_OPEN];
} g_fs_io;

/**
 * @brief 初始化文件系统后端
 *
 * 将全局 fd 池清零。必须在任何后端调用之前执行。
 * 重复调用安全。
 */
void file_io_fs_init(void)
{
    memset(&g_fs_io, 0, sizeof(g_fs_io));
}

/**
 * @brief 分配一个空闲文件描述符
 *
 * 线性扫描 g_fs_io.fds[] 查找第一个 allocated == 0 的槽位。
 * 找到后标记为已分配、记录索引、初始化 Zephyr 文件句柄。
 *
 * @return >=0 分配的槽位索引，-1 表示池已满
 *
 * @note 被 LOCK/UNLOCK 保护，多任务安全
 */
static int fd_alloc(void)
{
    system_lock();

    /* 线性扫描空闲槽位 */
    for (int i = 0; i < FS_MAX_OPEN; i++)
    {
        if (!g_fs_io.fds[i].allocated)
        {
            /* 找到空闲槽位：标记、记录索引、初始化 Zephyr 文件对象 */
            g_fs_io.fds[i].allocated = 1;
            g_fs_io.fds[i].idx = i;
            fs_file_t_init(&g_fs_io.fds[i].fd);
            system_unlock();
            return i;
        }
    }

    /* 池已满：无可用 fd */
    system_unlock();
    return -1;
}

/**
 * @brief 释放一个文件描述符
 *
 * 将指定槽位清零（包括 allocated = 0），使其可被重新分配。
 *
 * @param idx 要释放的槽位索引（0 ~ FS_MAX_OPEN-1）
 *
 * @note 被 LOCK/UNLOCK 保护，多任务安全
 */
static void fd_free(int idx)
{
    system_lock();

    /* 边界校验后清零 */
    if (idx >= 0 && (uint32_t)idx < FS_MAX_OPEN)
    {
        memset(&g_fs_io.fds[idx], 0, sizeof(g_fs_io.fds[idx]));
    }

    system_unlock();
}

/**
 * @brief FS 后端 open 回调
 *
 * 分配一个 fd 槽位，根据 mode 转换为 Zephyr flags（FS_O_READ / FS_O_WRITE | FS_O_CREATE），
 * 调用 fs_open()，成功后记录路径到 open_path（供 abort 用），将 fd 指针存入 io->ctx。
 *
 * @param io   文件 I/O 实例
 * @param path 文件系统绝对路径
 * @param mode 读/写模式
 * @return 0 成功，其他为 Zephyr fs_open 错误码
 */
static int fs_io_open(file_io_t *io, const char *path, file_io_mode_t mode)
{
    /* 从池中分配一个 fd 槽位 */
    int idx = fd_alloc();
    if (idx < 0)
        return -1;

    /* 将 file_io_mode_t 映射为 Zephyr fs_open flags */
    int fs_flags = 0;
    if (mode == FILE_IO_MODE_READ)
    {
        fs_flags = FS_O_READ; /* 只读 */
    }
    else
    {
        fs_flags = FS_O_WRITE | FS_O_CREATE; /* 写入 + 不存在则创建 */
    }

    /* 调用 Zephyr fs_open 打开文件 */
    int rc = fs_open(&g_fs_io.fds[idx].fd, path, fs_flags);
    if (rc != 0)
    {
        /* 打开失败：回收已分配的 fd 槽位 */
        fd_free(idx);
        return rc;
    }

    /* 打开成功：记录路径并绑定 fd 到 io->ctx */
    strncpy(g_fs_io.fds[idx].open_path, path, FS_OPEN_PATH_MAX - 1);
    g_fs_io.fds[idx].open_path[FS_OPEN_PATH_MAX - 1] = '\0';
    io->ctx = &g_fs_io.fds[idx];
    return 0;
}

/**
 * @brief FS 后端 read 回调
 *
 * 从 io->ctx 获取 fs_fd_t，先 fs_seek 到 offset，再 fs_read 读 len 字节。
 * 实际读取的字节数返回在 out_read 中。
 *
 * @param io       文件 I/O 实例
 * @param offset   读取起始偏移（字节偏移）
 * @param buf      输出缓冲区
 * @param len      期望读取字节数
 * @param out_read 实际读取字节数（输出参数）
 * @return 0 成功，其他为错误码
 */
static int fs_io_read(file_io_t *io, uint32_t offset,
                      uint8_t *buf, uint32_t len, uint32_t *out_read)
{
    /* 提取 fd 指针 */
    fs_fd_t *fdp = (fs_fd_t *)io->ctx;
    if (!fdp)
        return -1;

    /* 定位到 offset（FS_SEEK_SET = 从文件开头算起） */
    int rc = fs_seek(&fdp->fd, (off_t)offset, FS_SEEK_SET);
    if (rc != 0)
    {
        *out_read = 0; /* seek 失败：读取 0 字节 */
        return rc;
    }

    /* 执行实际读取 */
    ssize_t n = fs_read(&fdp->fd, buf, len);
    if (n < 0)
    {
        *out_read = 0; /* 读取错误：读取 0 字节 */
        return (int)n;
    }

    /* 成功：n 为实际读取字节数 */
    *out_read = (uint32_t)n;
    return 0;
}

/**
 * @brief FS 后端 write 回调
 *
 * 从 io->ctx 获取 fs_fd_t，调用 fs_write 写入数据。
 * 校验实际写入字节数是否等于请求的 len。
 *
 * @param io   文件 I/O 实例
 * @param data 待写入数据
 * @param len  数据长度
 * @return 0 成功（全部写入），-1 表示部分写入或错误
 */
static int fs_io_write(file_io_t *io, const uint8_t *data, uint32_t len)
{
    fs_fd_t *fdp = (fs_fd_t *)io->ctx;
    if (!fdp)
        return -1;

    /* 调用 Zephyr fs_write */
    ssize_t n = fs_write(&fdp->fd, data, len);

    /* fs_write 返回负数表示错误 */
    if (n < 0)
        return (int)n;

    /* 检查是否全部写入（FS 不会返回短写，但双重保险） */
    return ((uint32_t)n == len) ? 0 : -1;
}

/**
 * @brief FS 后端 close 回调
 *
 * 通过 fs_close 正常关闭文件，然后释放 fd 槽位。
 *
 * @param io  文件 I/O 实例
 * @return 0 成功，其他为 fs_close 错误码
 */
static int fs_io_close(file_io_t *io)
{
    fs_fd_t *fdp = (fs_fd_t *)io->ctx;
    if (!fdp)
        return 0;

    /* 记录槽位索引（close 后 fdp 指向的数据将被 memset 覆盖） */
    int idx = fdp->idx;

    /* 关闭 Zephyr 文件句柄 */
    int rc = fs_close(&fdp->fd);

    /* 清理通用层 ctx 并释放 fd 槽位 */
    io->ctx = NULL;
    fd_free(idx);
    return rc;
}

/**
 * @brief FS 后端 abort 回调
 *
 * 强制关闭文件，若 open_path 有效则调用 fs_unlink 删除临时文件。
 * 与 close 的区别：在关闭后额外删除磁盘上的文件。
 *
 * @param io  文件 I/O 实例
 * @return 始终返回 0
 */
static int fs_io_abort(file_io_t *io)
{
    fs_fd_t *fdp = (fs_fd_t *)io->ctx;
    if (!fdp)
    {
        io->ctx = NULL;
        return 0;
    }

    /* 先强制关闭文件 */
    fs_close(&fdp->fd);

    /* 若记录了已打开的路径，则删除该文件（清理临时文件） */
    if (fdp->open_path[0] != '\0')
    {
        fs_unlink(fdp->open_path);
    }

    /* 释放 fd 槽位 */
    int idx = fdp->idx;
    io->ctx = NULL;
    fd_free(idx);
    return 0;
}

/**
 * @brief FS 后端 get_size 回调
 *
 * 通过 fs_stat 查询文件的元数据，从中提取文件类型和大小。
 * 仅当 fs_stat 成功且条目为普通文件时才返回 true。
 *
 * @param io       文件 I/O 实例（未使用）
 * @param path     文件路径
 * @param out_size 输出文件大小
 * @return true 成功，false 失败（文件不存在/不是文件）
 */
static bool fs_io_get_size(file_io_t *io, const char *path, uint32_t *out_size)
{
    (void)io; /* io 未使用，由路径直接查询文件系统 */

    struct fs_dirent entry;

    /* 查询文件元数据 */
    int rc = fs_stat(path, &entry);
    if (rc != 0)
        return false;

    /* 仅对普通文件类型返回大小（目录、符号链接等返回 false） */
    if (entry.type != FS_DIR_ENTRY_FILE)
        return false;

    *out_size = (uint32_t)entry.size;
    return true;
}

/**
 * @brief FS 后端 remove 回调
 *
 * 直接调用 fs_unlink 删除文件。
 *
 * @param io   文件 I/O 实例（未使用）
 * @param path 文件路径
 * @return 0 成功，其他为 fs_unlink 错误码
 */
static int fs_io_remove(file_io_t *io, const char *path)
{
    (void)io; /* io 未使用，由路径直接操作文件系统 */
    return fs_unlink(path);
}

/**
 * @brief FS 后端虚函数表（静态常量化实例）
 *
 * 将所有 7 个回调函数指针绑定到对应的 fs_io_xxx 实现。
 */
static const file_io_ops_t g_fs_ops = {
    .open = fs_io_open,
    .read = fs_io_read,
    .write = fs_io_write,
    .close = fs_io_close,
    .get_size = fs_io_get_size,
    .remove = fs_io_remove,
    .abort = fs_io_abort,
};

/**
 * @brief 创建文件系统 I/O 后端实例
 *
 * 动态分配 file_io_t，将 ops 绑定到 g_fs_ops（Zephyr FS 实现）。
 * ctx 初始为 NULL，在 open 时由 fs_io_open 填充。
 *
 * @return 成功返回独立分配的 file_io_t 指针，内存不足返回 NULL
 */
file_io_t *file_io_fs_create(void)
{
    file_io_t *io = (file_io_t *)system_malloc(sizeof(*io));
    if (!io)
        return NULL;

    io->ops = &g_fs_ops;
    io->ctx = NULL;
    return io;
}

/**
 * @brief 销毁文件系统 I/O 后端实例
 *
 * 释放 file_io_t 所占内存。对 NULL 指针安全。
 *
 * @param io 由 file_io_fs_create() 创建的实例
 */
void file_io_fs_destroy(file_io_t *io)
{
    system_free(io);
}
