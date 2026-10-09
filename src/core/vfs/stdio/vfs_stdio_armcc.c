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
    if (fh < 3 || fh >= VFS_STDIO_MAX_FD || g_fdtable[fh] < 0)
        return -1;
    vfs_close(g_fdtable[fh]);
    g_fdtable[fh] = VFS_FD_INVALID;
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
    if (fh < 3 || fh >= VFS_STDIO_MAX_FD || g_fdtable[fh] < 0)
        return -1;
    ssize_t n = vfs_write(g_fdtable[fh], buf, len);
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
    if (fh < 3 || fh >= VFS_STDIO_MAX_FD || g_fdtable[fh] < 0)
        return -1;
    ssize_t n = vfs_read(g_fdtable[fh], buf, len);
    return (n == (ssize_t)len) ? 0 : -1;
}

/**
 * @brief ARMCC 定位钩子：等价 vfs_lseek(fh, pos, SEEK_SET)
 * @return 0 成功；失败 -1
 */
int _sys_seek(FILEHANDLE fh, long pos)
{
    stdio_ensure_init();
    if (fh < 3 || fh >= VFS_STDIO_MAX_FD || g_fdtable[fh] < 0)
        return -1;
    off_t rc = vfs_lseek(g_fdtable[fh], (off_t)pos, VFS_SEEK_SET);
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
    if (fh < 3 || fh >= VFS_STDIO_MAX_FD || g_fdtable[fh] < 0)
        return -1;

    /* 求长度：记原偏移 → seek 末尾 → 恢复原偏移（保持连续读不受影响） */
    off_t cur = vfs_tell(g_fdtable[fh]);
    off_t end = vfs_lseek(g_fdtable[fh], 0, VFS_SEEK_END);
    if (end < 0)
        return -1;
    if (cur >= 0)
        vfs_lseek(g_fdtable[fh], cur, VFS_SEEK_SET);
    return (long)end;
}

#endif /* VFS_ENABLE_STDIO */
