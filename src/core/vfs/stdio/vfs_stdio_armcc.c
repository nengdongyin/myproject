/**
 * @file vfs_stdio_armcc.c
 * @brief ARM Compiler (armcc/Keil MDK) 标准 I/O 适配层
 *
 * 在 VFS_ENABLE_STDIO=1 且使用 ARM Compiler 时编译。实现 ARM C 库的
 * 底层重定向钩子 _sys_open/_sys_read/... 把标准 I/O 导流到 VFS。
 *
 * @section fd fd 映射
 *  与 newlib 层一致：0/1/2 保留，应用 HANDLE 从 3 起。
 *
 * @section caveat 注意
 *  仅为 ARM Compiler 提供。若工程用 GCC/newlib，请改用 vfs_stdio_newlib.c，
 *  两者不要同时编入（符号冲突）。
 */

#include "vfs.h"
#include "vfs_config.h"

#if VFS_ENABLE_STDIO && defined(__ARMCC_VERSION)

#include <stdio.h>
#include <errno.h>
#include "port.h"

/* ARMCC 的 openmode 位（见 ARM C 库 stdio.h）：此处按惯例取值 */
#ifndef OPEN_R
#define OPEN_R    0x01
#define OPEN_W    0x02
#define OPEN_A    0x04
#define OPEN_PLUS 0x10
#endif

#ifndef VFS_STDIO_MAX_FD
#define VFS_STDIO_MAX_FD 8
#endif

static vfs_fd_t g_fdtable[VFS_STDIO_MAX_FD];
static int g_stdio_inited;
/** @brief fd 映射表锁（保护 g_fdtable / g_stdio_inited；句柄式，与 VFS core/后端一致） */
static sys_mutex_t *g_stdio_lock;

/**
 * @brief 惰性初始化 fd 映射表并建锁
 * @note 首次调用须单线程（启动期）；sys_mutex_create 非线程安全，锁建好后
 *       后续调用才可并发。
 */
static void stdio_ensure_init(void)
{
    if (!g_stdio_inited)
    {
        for (int i = 0; i < VFS_STDIO_MAX_FD; i++)
            g_fdtable[i] = VFS_FD_INVALID;
        g_stdio_inited = 1;
    }
    if (!g_stdio_lock)
        g_stdio_lock = sys_mutex_create();
}

/** @brief ARMCC openmode → VFS_O_* */
static int map_openmode(int om)
{
    int rd = (om & OPEN_R) || (om & OPEN_PLUS);
    int wr = (om & OPEN_W) || (om & OPEN_A);

    int vf;
    if (rd && wr)
        vf = VFS_O_RDWR;
    else if (wr)
        vf = VFS_O_WRONLY;
    else
        vf = VFS_O_RDONLY;

    if (wr)
        vf |= VFS_O_CREAT;
    if (om & OPEN_W)
        vf |= VFS_O_TRUNC;
    if (om & OPEN_A)
        vf |= VFS_O_APPEND;
    return vf;
}

/**
 * @brief 为 vfs_fd 分配一个应用层 HANDLE（从 3 开始，0/1/2 保留）
 * @return >= 3 应用 HANDLE；映射表满返回 -1
 */
static int appfd_alloc(vfs_fd_t vfd)
{
    sys_mutex_lock(g_stdio_lock);
    for (int i = 3; i < VFS_STDIO_MAX_FD; i++)
    {
        if (g_fdtable[i] < 0)
        {
            g_fdtable[i] = vfd;
            sys_mutex_unlock(g_stdio_lock);
            return i;
        }
    }
    sys_mutex_unlock(g_stdio_lock);
    return -1;
}

/* ================================================================
 *  控制台钩子（stdin/stdout/stderr 的底层 I/O，平台用弱符号覆盖）
 * ================================================================ */

/** @brief 控制台输出钩子（fd 1=stdout、fd 2=stderr）；平台覆盖写到底层 UART */
__weak ssize_t vfs_console_write(const void *buf, size_t len)
{
    (void)buf;
    (void)len;
    return -ENOTSUP;
}

/** @brief 控制台输入钩子（fd 0=stdin）；平台覆盖从底层 UART 读 */
__weak ssize_t vfs_console_read(void *buf, size_t len)
{
    (void)buf;
    (void)len;
    return -ENOTSUP;
}

/**
 * @brief ARMCC 打开钩子：把 openmode 映射为 VFS_O_* 后调用 vfs_open
 * @return 应用 HANDLE；失败返回 -1
 */
FILEHANDLE _sys_open(const char *name, int openmode)
{
    stdio_ensure_init();

    vfs_fd_t vfd = vfs_open(name, map_openmode(openmode));
    if (vfd < 0)
        return -1;

    int fd = appfd_alloc(vfd);
    if (fd < 0)
    {
        vfs_close(vfd);
        return -1;
    }
    return fd;
}

/**
 * @brief ARMCC 关闭钩子：按映射表关闭对应 vfs fd
 * @return 0 成功；非法 HANDLE 返回 -1
 */
int _sys_close(FILEHANDLE fh)
{
    stdio_ensure_init();

    sys_mutex_lock(g_stdio_lock);
    if (fh < 3 || fh >= VFS_STDIO_MAX_FD || g_fdtable[fh] < 0)
    {
        sys_mutex_unlock(g_stdio_lock);
        return -1;
    }
    vfs_fd_t vfd = g_fdtable[fh];   /* 锁内快照 + 清表，锁外关闭避免持锁阻塞 */
    g_fdtable[fh] = VFS_FD_INVALID;
    sys_mutex_unlock(g_stdio_lock);

    vfs_close(vfd);
    return 0;
}

/**
 * @brief ARMCC 写钩子
 * @return 0 表示全部写入成功；否则 -1（ARMCC 约定）
 */
int _sys_write(FILEHANDLE fh, const unsigned char *buf, unsigned len, int mode)
{
    (void)mode;
    stdio_ensure_init();

    /* 1 = stdout、2 = stderr → 控制台输出；0 不可写（不涉及 g_fdtable，无需加锁） */
    if (fh >= 0 && fh < 3)
    {
        if (fh == 0)
            return -1;
        ssize_t n = vfs_console_write(buf, len);
        return (n == (ssize_t)len) ? 0 : -1;
    }

    sys_mutex_lock(g_stdio_lock);
    if (fh < 0 || fh >= VFS_STDIO_MAX_FD || g_fdtable[fh] < 0)
    {
        sys_mutex_unlock(g_stdio_lock);
        return -1;
    }
    vfs_fd_t vfd = g_fdtable[fh];   /* 锁内快照，锁外 I/O */
    sys_mutex_unlock(g_stdio_lock);

    ssize_t n = vfs_write(vfd, buf, len);
    return (n == (ssize_t)len) ? 0 : -1; /* ARMCC: 0=全部成功 */
}

/**
 * @brief ARMCC 读钩子
 * @return 0 表示读满 len；否则 -1（ARMCC 约定）
 */
int _sys_read(FILEHANDLE fh, unsigned char *buf, unsigned len, int mode)
{
    (void)mode;
    stdio_ensure_init();

    /* 0 = stdin → 控制台输入；1/2 不可读（不涉及 g_fdtable，无需加锁） */
    if (fh >= 0 && fh < 3)
    {
        if (fh != 0)
            return -1;
        ssize_t n = vfs_console_read(buf, len);
        return (n == (ssize_t)len) ? 0 : -1;
    }

    sys_mutex_lock(g_stdio_lock);
    if (fh < 0 || fh >= VFS_STDIO_MAX_FD || g_fdtable[fh] < 0)
    {
        sys_mutex_unlock(g_stdio_lock);
        return -1;
    }
    vfs_fd_t vfd = g_fdtable[fh];   /* 锁内快照，锁外 I/O */
    sys_mutex_unlock(g_stdio_lock);

    ssize_t n = vfs_read(vfd, buf, len);
    return (n == (ssize_t)len) ? 0 : -1;
}

/**
 * @brief ARMCC 定位钩子：等价 vfs_lseek(fh, pos, SEEK_SET)
 * @return 0 成功；失败 -1
 */
int _sys_seek(FILEHANDLE fh, long pos)
{
    stdio_ensure_init();

    sys_mutex_lock(g_stdio_lock);
    if (fh < 3 || fh >= VFS_STDIO_MAX_FD || g_fdtable[fh] < 0)
    {
        sys_mutex_unlock(g_stdio_lock);
        return -1;
    }
    vfs_fd_t vfd = g_fdtable[fh];   /* 锁内快照，锁外 I/O */
    sys_mutex_unlock(g_stdio_lock);

    off_t rc = vfs_lseek(vfd, (off_t)pos, VFS_SEEK_SET);
    return (rc < 0) ? -1 : 0;
}

/** @brief 仅 0/1/2（标准流）视为 tty，其余返回 0 */
int _sys_istty(FILEHANDLE fh)
{
    return (fh >= 0 && fh < 3) ? 1 : 0;
}

/**
 * @brief ARMCC 求文件长度钩子：seek 到末尾再复位
 * @return 文件长度；失败返回 -1
 */
long _sys_flen(FILEHANDLE fh)
{
    stdio_ensure_init();

    sys_mutex_lock(g_stdio_lock);
    if (fh < 3 || fh >= VFS_STDIO_MAX_FD || g_fdtable[fh] < 0)
    {
        sys_mutex_unlock(g_stdio_lock);
        return -1;
    }
    vfs_fd_t vfd = g_fdtable[fh];   /* 锁内快照，锁外 I/O */
    sys_mutex_unlock(g_stdio_lock);

    /* 求长度：记原偏移 → seek 末尾 → 恢复原偏移（保持连续读不受影响） */
    off_t cur = vfs_tell(vfd);
    off_t end = vfs_lseek(vfd, 0, VFS_SEEK_END);
    if (end < 0)
        return -1;
    if (cur >= 0)
        vfs_lseek(vfd, cur, VFS_SEEK_SET);
    return (long)end;
}

#endif /* VFS_ENABLE_STDIO */
