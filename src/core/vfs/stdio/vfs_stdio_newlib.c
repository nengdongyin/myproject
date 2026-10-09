/**
 * @file vfs_stdio_newlib.c
 * @brief newlib/glibc 标准 I/O 适配层（_open_r / _read_r / ... → VFS）
 *
 * 在 VFS_ENABLE_STDIO=1 时编译。把 newlib 的系统调用钩子导流到 VFS，使
 * fopen/fread/fprintf 等标准 I/O 经由本 VFS 落盘。
 *
 * @section fd fd 映射
 *  0/1/2 保留给 stdin/stdout/stderr（本层不接管，交由平台原有实现）。
 *  应用 fd 从 3 起，g_fdtable[fd] 保存对应的 vfs_fd_t。
 *
 * @section caveat 注意
 *  这些符号是全局系统调用钩子，会与平台 libc 的其它实现冲突。仅当你确实
 *  想让标准 I/O 走 VFS 时才开启，并确保 0/1/2 由平台 UART 等接管。
 */

#include "vfs.h"
#include "vfs_config.h"

#if VFS_ENABLE_STDIO && !defined(__ARMCC_VERSION)

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/reent.h>
#include <stddef.h>
#include <string.h>

#ifndef VFS_STDIO_MAX_FD
#define VFS_STDIO_MAX_FD 8
#endif

static vfs_fd_t g_fdtable[VFS_STDIO_MAX_FD];
static int g_stdio_inited;

/** @brief 惰性初始化 fd 映射表（全部置为 VFS_FD_INVALID） */
static void stdio_ensure_init(void)
{
    if (!g_stdio_inited)
    {
        for (int i = 0; i < VFS_STDIO_MAX_FD; i++)
            g_fdtable[i] = VFS_FD_INVALID;
        g_stdio_inited = 1;
    }
}

/** @brief newlib 打开标志 → VFS_O_* */
static int map_flags(int flags)
{
    int vf = flags & (O_RDONLY | O_WRONLY | O_RDWR); /* 0/1/2 与 VFS 一致 */
    if (flags & O_CREAT)
        vf |= VFS_O_CREAT;
    if (flags & O_EXCL)
        vf |= VFS_O_EXCL;
    if (flags & O_TRUNC)
        vf |= VFS_O_TRUNC;
    if (flags & O_APPEND)
        vf |= VFS_O_APPEND;
    return vf;
}

/**
 * @brief 为 vfs_fd 分配一个应用层 fd（从 3 开始，0/1/2 保留）
 * @return >= 3 应用 fd；映射表满返回 -1
 */
static int appfd_alloc(vfs_fd_t vfd)
{
    for (int i = 3; i < VFS_STDIO_MAX_FD; i++)
    {
        if (g_fdtable[i] < 0)
        {
            g_fdtable[i] = vfd;
            return i;
        }
    }
    return -1;
}

/**
 * @brief newlib 打开钩子：把 name/flags 导流到 vfs_open
 * @return 应用 fd；失败返回 -1 并设 errno
 */
int _open_r(struct _reent *r, const char *name, int flags, int mode)
{
    (void)r;
    (void)mode;
    stdio_ensure_init();

    vfs_fd_t vfd = vfs_open(name, map_flags(flags));
    if (vfd < 0)
    {
        errno = -vfd;
        return -1;
    }
    int fd = appfd_alloc(vfd);
    if (fd < 0)
    {
        vfs_close(vfd);
        errno = ENFILE;
        return -1;
    }
    return fd;
}

/** @brief _open 的全局线程本地包装（委托 _open_r） */
int _open(const char *name, int flags, int mode)
{
    return _open_r(_REENT, name, flags, mode);
}

/**
 * @brief newlib 关闭钩子：按映射表关闭对应 vfs fd
 * @return 0 成功；非法 fd 返回 -1 并设 errno=EBADF
 */
int _close_r(struct _reent *r, int fd)
{
    (void)r;
    stdio_ensure_init();
    if (fd < 3 || fd >= VFS_STDIO_MAX_FD || g_fdtable[fd] < 0)
    {
        errno = EBADF;
        return -1;
    }
    vfs_close(g_fdtable[fd]);
    g_fdtable[fd] = VFS_FD_INVALID;
    return 0;
}

/** @brief _close 的全局线程本地包装（委托 _close_r） */
int _close(int fd)
{
    return _close_r(_REENT, fd);
}

/**
 * @brief newlib 读钩子：vfs_read 成功后返回字节数，失败设 errno 并返回 -1
 */
_ssize_t _read_r(struct _reent *r, int fd, void *buf, size_t len)
{
    (void)r;
    stdio_ensure_init();
    if (fd < 3 || fd >= VFS_STDIO_MAX_FD || g_fdtable[fd] < 0)
    {
        errno = EBADF;
        return -1;
    }
    ssize_t n = vfs_read(g_fdtable[fd], buf, len);
    if (n < 0)
    {
        errno = -n;
        return -1;
    }
    return (_ssize_t)n;
}

/** @brief _read 的全局线程本地包装（委托 _read_r） */
_ssize_t _read(int fd, void *buf, size_t len)
{
    return _read_r(_REENT, fd, buf, len);
}

/**
 * @brief newlib 写钩子：vfs_write 成功后返回字节数，失败设 errno 并返回 -1
 */
_ssize_t _write_r(struct _reent *r, int fd, const void *buf, size_t len)
{
    (void)r;
    stdio_ensure_init();
    if (fd < 3 || fd >= VFS_STDIO_MAX_FD || g_fdtable[fd] < 0)
    {
        errno = EBADF;
        return -1;
    }
    ssize_t n = vfs_write(g_fdtable[fd], buf, len);
    if (n < 0)
    {
        errno = -n;
        return -1;
    }
    return (_ssize_t)n;
}

/** @brief _write 的全局线程本地包装（委托 _write_r） */
_ssize_t _write(int fd, const void *buf, size_t len)
{
    return _write_r(_REENT, fd, buf, len);
}

/**
 * @brief newlib 定位钩子：vfs_lseek 成功后返回新偏移，失败设 errno 并返回 -1
 */
_off_t _lseek_r(struct _reent *r, int fd, _off_t off, int whence)
{
    (void)r;
    stdio_ensure_init();
    if (fd < 3 || fd >= VFS_STDIO_MAX_FD || g_fdtable[fd] < 0)
    {
        errno = EBADF;
        return -1;
    }
    off_t rc = vfs_lseek(g_fdtable[fd], (off_t)off, whence);
    if (rc < 0)
    {
        errno = -rc;
        return -1;
    }
    return (_off_t)rc;
}

/** @brief _lseek 的全局线程本地包装（委托 _lseek_r） */
_off_t _lseek(int fd, _off_t off, int whence)
{
    return _lseek_r(_REENT, fd, off, whence);
}

/**
 * @brief newlib stat 钩子：vfs_stat 结果填充为 S_IFDIR/S_IFREG 模式
 * @return 0 成功；失败返回 -1 并设 errno
 */
int _stat_r(struct _reent *r, const char *name, struct stat *st)
{
    (void)r;
    vfs_stat_t vs;
    int rc = vfs_stat(name, &vs);
    if (rc < 0)
    {
        errno = -rc;
        return -1;
    }
    memset(st, 0, sizeof(*st));
    st->st_mode = (vs.type == VFS_TYPE_DIR) ? S_IFDIR : S_IFREG;
    st->st_size = vs.size;
    return 0;
}

/** @brief _stat 的全局线程本地包装（委托 _stat_r） */
int _stat(const char *name, struct stat *st)
{
    return _stat_r(_REENT, name, st);
}

/**
 * @brief newlib fstat 钩子：当前仅回填为普通文件，大小为 0
 * @note 如需精确大小，可改用 vfs_lseek(fd, 0, SEEK_END) 自行获取
 */
int _fstat_r(struct _reent *r, int fd, struct stat *st)
{
    (void)r;
    stdio_ensure_init();
    if (fd < 3 || fd >= VFS_STDIO_MAX_FD || g_fdtable[fd] < 0)
    {
        errno = EBADF;
        return -1;
    }
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFREG;
    /* 精确大小：记原偏移 → seek 到末尾取长度 → 恢复偏移 */
    off_t cur = vfs_tell(g_fdtable[fd]);
    off_t len = vfs_lseek(g_fdtable[fd], 0, VFS_SEEK_END);
    if (cur >= 0)
        vfs_lseek(g_fdtable[fd], cur, VFS_SEEK_SET);
    st->st_size = (len >= 0) ? (size_t)len : 0;
    return 0;
}

/** @brief _fstat 的全局线程本地包装（委托 _fstat_r） */
int _fstat(int fd, struct stat *st)
{
    return _fstat_r(_REENT, fd, st);
}

/** @brief 仅 0/1/2（标准流）视为 tty，其余返回 0 */
int _isatty_r(struct _reent *r, int fd)
{
    (void)r;
    return (fd >= 0 && fd < 3) ? 1 : 0; /* 仅 0/1/2 视为 tty */
}

/** @brief _isatty 的全局线程本地包装（委托 _isatty_r） */
int _isatty(int fd)
{
    return _isatty_r(_REENT, fd);
}

/**
 * @brief newlib unlink 钩子：直接转发 vfs_unlink
 * @return 0 成功；失败返回 -1 并设 errno
 */
int _unlink_r(struct _reent *r, const char *name)
{
    (void)r;
    int rc = vfs_unlink(name);
    if (rc < 0)
    {
        errno = -rc;
        return -1;
    }
    return 0;
}

/** @brief _unlink 的全局线程本地包装（委托 _unlink_r） */
int _unlink(const char *name)
{
    return _unlink_r(_REENT, name);
}

#endif /* VFS_ENABLE_STDIO */
