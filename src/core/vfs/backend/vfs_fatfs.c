/**
 * @file vfs_fatfs.c
 * @brief FatFs 后端实现（原生 f_* API + vfs_bdev_t + 内置 diskio）
 *
 * 整文件在 VFS_ENABLE_FATFS=1 时才编译。默认关闭（FatFs 未引入）。
 * 每挂载点一个 FATFS + 一个 FatFs 逻辑盘号 pdrv；pdrv→bdev 映射供 diskio 使用。
 */

#include "vfs_fatfs.h"

#if VFS_ENABLE_FATFS

#include "ff.h"
#include "diskio.h"
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include "port.h"

/** @brief FatFs 逻辑盘数量（不得 > ffconf.h 的 FF_VOLUMES） */
#ifndef VFS_FATFS_VOLUMES
#define VFS_FATFS_VOLUMES 2
#endif

/** @brief FatFs 约定的扇区大小（与 SD 一致） */
#define VFS_FATFS_SECTOR 512u

/* ================================================================
 *  diskio：pdrv → vfs_bdev_t
 * ================================================================ */

/** @brief pdrv → bdev 映射表（diskio 回调据此转发扇区访问） */
static const vfs_bdev_t *g_fat_disk[VFS_FATFS_VOLUMES];

/** @brief FatFs diskio：返回盘符状态（已绑定 bdev 则 OK，否则 NOINIT） */
DSTATUS disk_status(BYTE pdrv)
{
    return (pdrv < VFS_FATFS_VOLUMES && g_fat_disk[pdrv]) ? 0 : STA_NOINIT;
}

/** @brief FatFs diskio：初始化（此处复用 disk_status，无额外动作） */
DSTATUS disk_initialize(BYTE pdrv)
{
    return disk_status(pdrv);
}

/** @brief FatFs diskio：读扇区（按块号转发到 vfs_bdev_read） */
DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv >= VFS_FATFS_VOLUMES || !g_fat_disk[pdrv])
        return RES_NOTRDY;
    if (vfs_bdev_read(g_fat_disk[pdrv], (uint32_t)sector, buff, (uint32_t)count) < 0)
        return RES_ERROR;
    return RES_OK;
}

/** @brief FatFs diskio：写扇区（按块号转发到 vfs_bdev_prog） */
DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv >= VFS_FATFS_VOLUMES || !g_fat_disk[pdrv])
        return RES_NOTRDY;
    if (vfs_bdev_prog(g_fat_disk[pdrv], (uint32_t)sector, buff, (uint32_t)count) < 0)
        return RES_ERROR;
    return RES_OK;
}

/** @brief FatFs diskio：控制命令（容量/扇区大小/块大小等） */
DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    if (pdrv >= VFS_FATFS_VOLUMES || !g_fat_disk[pdrv])
        return RES_NOTRDY;
    const vfs_bdev_t *d = g_fat_disk[pdrv];

    switch (cmd)
    {
    case CTRL_SYNC:
        return RES_OK;
    case GET_SECTOR_COUNT:
        *(LBA_t *)buff = d->blk_count;
        return RES_OK;
    case GET_SECTOR_SIZE:
        *(WORD *)buff = (WORD)d->blk_size;
        return RES_OK;
    case GET_BLOCK_SIZE:
        *(DWORD *)buff = 1;
        return RES_OK;
    default:
        return RES_PARERR;
    }
}

/** @brief 无 RTC，固定时间戳 */
DWORD get_fattime(void)
{
    return 0;
}

/* ================================================================
 *  每挂载点 / 每句柄静态池
 * ================================================================ */

typedef struct
{
    uint8_t used;      /**< 挂载槽占用标记 */
    BYTE pdrv;         /**< 本挂载点占用的 FatFs 逻辑盘号 */
    FATFS fs;          /**< FatFs 实例 */
    char prefix[4];    /**< 盘符前缀，如 "0:" */
} fat_mnt_t;

typedef struct
{
    uint8_t used;  /**< 文件句柄槽占用标记 */
    FIL fil;       /**< FatFs 文件句柄 */
} fat_file_t;

#if VFS_ENABLE_DIR
typedef struct
{
    uint8_t used;  /**< 目录句柄槽占用标记 */
    DIR dir;       /**< FatFs 目录句柄 */
} fat_dir_t;
#endif

/** @brief FatFs 挂载槽池（每挂载点一个 FATFS + 盘号） */
static fat_mnt_t g_fat_mnts[VFS_MAX_MOUNTS];
/** @brief FatFs 文件句柄池（静态池，零堆） */
static fat_file_t g_fat_files[VFS_MAX_OPEN_FILES];
#if VFS_ENABLE_DIR
/** @brief FatFs 目录句柄池（静态池，零堆） */
static fat_dir_t g_fat_dirs[VFS_MAX_OPEN_DIRS];
#endif

/* ================================================================
 *  路径拼装 "N:/rel"
 * ================================================================ */
/** @brief 拼装 FatFs 绝对路径：盘符前缀 + "/" + 挂载点内相对路径 */
static void fat_path(const fat_mnt_t *m, const char *rel, char *out, size_t n)
{
    if (rel[0] == '\0')
        snprintf(out, n, "%s", m->prefix);
    else
        snprintf(out, n, "%s/%s", m->prefix, rel);
}

/* ================================================================
 *  backend 回调
 * ================================================================ */

/**
 * @brief 挂载 FatFs 卷
 * @details 校验扇区大小须为 512，分配挂载槽与空闲 pdrv，建立 pdrv→bdev 映射，
 *          再 f_mount 立即挂载。失败回滚 pdrv 映射。
 * @return 0 成功；-EINVAL 扇区大小不符，-ENOSPC 槽/pdrv 耗尽，-EIO f_mount 失败
 */
static int fat_vfs_mount(vfs_mount_t *mp)
{
    const vfs_bdev_t *bdev = mp->storage_dev;
    if (!bdev || bdev->blk_size != VFS_FATFS_SECTOR)
        return -EINVAL;

    int idx = -1;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++)
        if (!g_fat_mnts[i].used)
        {
            idx = i;
            break;
        }
    if (idx < 0)
        return -ENOSPC;

    BYTE pdrv = 0xFF;
    for (BYTE i = 0; i < VFS_FATFS_VOLUMES; i++)
        if (!g_fat_disk[i])
        {
            pdrv = i;
            break;
        }
    if (pdrv == 0xFF)
        return -ENOSPC;

    g_fat_disk[pdrv] = bdev;
    fat_mnt_t *m = &g_fat_mnts[idx];
    memset(m, 0, sizeof(*m)); /* FatFs 工作区必须干净（复用槽亦然） */
    m->pdrv = pdrv;
    m->prefix[0] = (char)('0' + pdrv);
    m->prefix[1] = ':';
    m->prefix[2] = '\0';

    FRESULT fr = f_mount(&m->fs, m->prefix, 1);
    if (fr != FR_OK)
    {
        g_fat_disk[pdrv] = NULL;
        return -EIO;
    }
    m->used = 1;
    mp->fs_ctx = m;
    return 0;
}

/** @brief 卸载 FatFs 卷：f_mount 注销后清除 pdrv 映射并释放槽 */
static int fat_vfs_unmount(vfs_mount_t *mp)
{
    fat_mnt_t *m = (fat_mnt_t *)mp->fs_ctx;
    if (!m)
        return 0;
    f_mount(NULL, m->prefix, 0);
    g_fat_disk[m->pdrv] = NULL;
    m->used = 0;
    mp->fs_ctx = NULL;
    return 0;
}

/**
 * @brief 分配一个 FatFs 文件句柄槽
 * @details 用后端池锁 SYS_LOCK_FILE 保护静态池。
 * @return 空闲槽指针；池满返回 NULL
 */
static fat_file_t *fat_file_alloc(void)
{
    system_lock(SYS_LOCK_FILE);
    for (int i = 0; i < VFS_MAX_OPEN_FILES; i++)
        if (!g_fat_files[i].used)
        {
            memset(&g_fat_files[i], 0, sizeof(g_fat_files[i]));
            g_fat_files[i].used = 1;
            system_unlock(SYS_LOCK_FILE);
            return &g_fat_files[i];
        }
    system_unlock(SYS_LOCK_FILE);
    return NULL;
}

/**
 * @brief 打开/创建 FatFs 文件
 * @details 把 VFS_O_* 映射为 FA_*：O_CREAT|O_EXCL → FA_CREATE_NEW，
 *          O_CREAT|O_TRUNC → FA_CREATE_ALWAYS，仅 O_TRUNC 也近似为 CREATE_ALWAYS，
 *          否则按 OPEN_ALWAYS/OPEN_APPEND 处理。
 * @return 0 成功；-ENODEV 挂载上下文缺失，-ENFILE 句柄池满，-ENOENT/-EEXIST/-EIO
 */
static int fat_vfs_open(vfs_file_t *f, const char *rel, int flags)
{
    fat_mnt_t *m = (fat_mnt_t *)f->mp->fs_ctx;
    if (!m)
        return -ENODEV;

    char path[VFS_PATH_MAX + 4];
    fat_path(m, rel, path, sizeof(path));

    int acc = flags & 0x3;
    BYTE mode = 0;
    if (acc != VFS_O_WRONLY)
        mode |= FA_READ;
    if (acc != VFS_O_RDONLY)
        mode |= FA_WRITE;

    if (flags & VFS_O_CREAT)
    {
        if (flags & VFS_O_EXCL)
            mode |= FA_CREATE_NEW;
        else if (flags & VFS_O_TRUNC)
            mode |= FA_CREATE_ALWAYS;
        else
            mode |= FA_OPEN_ALWAYS;
    }
    else if (flags & VFS_O_TRUNC)
    {
        mode |= FA_CREATE_ALWAYS; /* FatFs 无纯 truncate-on-open，近似为截断/创建 */
    }
    if (flags & VFS_O_APPEND)
        mode |= FA_OPEN_APPEND;

    fat_file_t *s = fat_file_alloc();
    if (!s)
        return -ENFILE;

    FRESULT fr = f_open(&s->fil, path, mode);
    if (fr != FR_OK)
    {
        s->used = 0;
        return (fr == FR_NO_FILE || fr == FR_NO_PATH) ? -ENOENT
               : (fr == FR_EXIST)                        ? -EEXIST
                                                         : -EIO;
    }
    f->priv = s;
    return 0;
}

/** @brief 读取 FatFs 文件：返回实际字节数，< 0 负 errno */
static ssize_t fat_vfs_read(vfs_file_t *f, void *buf, size_t n)
{
    fat_file_t *s = (fat_file_t *)f->priv;
    if (!s)
        return -EBADF;
    UINT br = 0;
    FRESULT fr = f_read(&s->fil, buf, (UINT)n, &br);
    return (fr != FR_OK) ? -EIO : (ssize_t)br;
}

/** @brief 写入 FatFs 文件：写不满视为 -ENOSPC，< 0 负 errno */
static ssize_t fat_vfs_write(vfs_file_t *f, const void *buf, size_t n)
{
    fat_file_t *s = (fat_file_t *)f->priv;
    if (!s)
        return -EBADF;
    UINT bw = 0;
    FRESULT fr = f_write(&s->fil, buf, (UINT)n, &bw);
    if (fr != FR_OK)
        return -EIO;
    return (bw == (UINT)n) ? (ssize_t)bw : -ENOSPC;
}

/** @brief 移动 FatFs 文件偏移：按 whence 求 base 后 f_lseek */
static off_t fat_vfs_lseek(vfs_file_t *f, off_t off, int whence)
{
    fat_file_t *s = (fat_file_t *)f->priv;
    if (!s)
        return -EBADF;

    FSIZE_t base;
    if (whence == VFS_SEEK_SET)
        base = 0;
    else if (whence == VFS_SEEK_CUR)
        base = f_tell(&s->fil);
    else if (whence == VFS_SEEK_END)
        base = f_size(&s->fil);
    else
        return -EINVAL;

    off_t np = (off_t)base + off;
    if (np < 0)
        return -EINVAL;

    FRESULT fr = f_lseek(&s->fil, (FSIZE_t)np);
    return (fr != FR_OK) ? -EIO : (off_t)f_tell(&s->fil);
}

/** @brief 返回 FatFs 文件当前读写指针 */
static off_t fat_vfs_tell(vfs_file_t *f)
{
    fat_file_t *s = (fat_file_t *)f->priv;
    return s ? (off_t)f_tell(&s->fil) : -EBADF;
}

/** @brief 同步 FatFs 文件（f_sync 落盘） */
static int fat_vfs_sync(vfs_file_t *f)
{
    fat_file_t *s = (fat_file_t *)f->priv;
    return s ? ((f_sync(&s->fil) == FR_OK) ? 0 : -EIO) : -EBADF;
}

/** @brief 截断 FatFs 文件：先 lseek 到 len 再 f_truncate */
static int fat_vfs_truncate(vfs_file_t *f, off_t len)
{
    fat_file_t *s = (fat_file_t *)f->priv;
    if (!s)
        return -EBADF;
    /* FatFs f_truncate 截断到当前读写指针；先把指针移到 len */
    if (f_lseek(&s->fil, (FSIZE_t)len) != FR_OK)
        return -EIO;
    return (f_truncate(&s->fil) == FR_OK) ? 0 : -EIO;
}

/** @brief 关闭 FatFs 文件：f_close 后释放文件槽 */
static int fat_vfs_close(vfs_file_t *f)
{
    fat_file_t *s = (fat_file_t *)f->priv;
    if (!s)
        return 0;
    int rc = (f_close(&s->fil) == FR_OK) ? 0 : -EIO;
    system_lock(SYS_LOCK_FILE);
    s->used = 0;
    system_unlock(SYS_LOCK_FILE);
    f->priv = NULL;
    return rc;
}

/** @brief 查询 FatFs 文件/目录状态（f_stat） */
static int fat_vfs_stat(vfs_mount_t *mp, const char *rel, vfs_stat_t *st)
{
    fat_mnt_t *m = (fat_mnt_t *)mp->fs_ctx;
    if (!m)
        return -ENODEV;
    char path[VFS_PATH_MAX + 4];
    fat_path(m, rel, path, sizeof(path));

    FILINFO info;
    FRESULT fr = f_stat(path, &info);
    if (fr != FR_OK)
        return -ENOENT;
    st->type = (info.fattrib & AM_DIR) ? VFS_TYPE_DIR : VFS_TYPE_FILE;
    st->size = (uint32_t)info.fsize;
    return 0;
}

/** @brief 删除 FatFs 文件（f_unlink） */
static int fat_vfs_unlink(vfs_mount_t *mp, const char *rel)
{
    fat_mnt_t *m = (fat_mnt_t *)mp->fs_ctx;
    if (!m)
        return -ENODEV;
    char path[VFS_PATH_MAX + 4];
    fat_path(m, rel, path, sizeof(path));
    return (f_unlink(path) == FR_OK) ? 0 : -EIO;
}

/** @brief 重命名 FatFs 文件/目录（f_rename） */
static int fat_vfs_rename(vfs_mount_t *mp, const char *from, const char *to)
{
    fat_mnt_t *m = (fat_mnt_t *)mp->fs_ctx;
    if (!m)
        return -ENODEV;
    char pf[VFS_PATH_MAX + 4], pt[VFS_PATH_MAX + 4];
    fat_path(m, from, pf, sizeof(pf));
    fat_path(m, to, pt, sizeof(pt));
    return (f_rename(pf, pt) == FR_OK) ? 0 : -EIO;
}

/** @brief 创建 FatFs 目录（f_mkdir） */
static int fat_vfs_mkdir(vfs_mount_t *mp, const char *rel)
{
    fat_mnt_t *m = (fat_mnt_t *)mp->fs_ctx;
    if (!m)
        return -ENODEV;
    char path[VFS_PATH_MAX + 4];
    fat_path(m, rel, path, sizeof(path));
    return (f_mkdir(path) == FR_OK) ? 0 : -EIO;
}

/** @brief 查询 FatFs 卷容量：f_getfree 换算为总/空闲字节 */
static int fat_vfs_statvfs(vfs_mount_t *mp, vfs_statvfs_t *st)
{
    fat_mnt_t *m = (fat_mnt_t *)mp->fs_ctx;
    if (!m)
        return -ENODEV;
    DWORD nfree = 0;
    FATFS *fs = NULL;
    FRESULT fr = f_getfree(m->prefix, &nfree, &fs);
    if (fr != FR_OK || !fs)
        return -EIO;
    uint32_t csize = fs->csize * VFS_FATFS_SECTOR; /* 每簇字节数 */
    uint32_t total = (fs->n_fatent > 2)
                         ? (uint32_t)((fs->n_fatent - 2) * (DWORD)csize)
                         : 0;
    uint32_t freeb = (uint32_t)(nfree * (DWORD)csize);
    st->total_bytes = total;
    st->free_bytes = freeb;
    return 0;
}

#if VFS_ENABLE_DIR
/** @brief 打开 FatFs 目录：分配目录槽并 f_opendir */
static int fat_vfs_opendir(vfs_dir_t *d, const char *rel)
{
    fat_mnt_t *m = (fat_mnt_t *)d->mp->fs_ctx;
    if (!m)
        return -ENODEV;
    char path[VFS_PATH_MAX + 4];
    fat_path(m, rel, path, sizeof(path));

    system_lock(SYS_LOCK_FILE);
    int slot = -1;
    for (int i = 0; i < VFS_MAX_OPEN_DIRS; i++)
    {
        if (!g_fat_dirs[i].used)
        {
            g_fat_dirs[i].used = 1;
            slot = i;
            break;
        }
    }
    system_unlock(SYS_LOCK_FILE);
    if (slot < 0)
        return -ENFILE;

    /* 目录 I/O 不持全局池锁（避免跨卷瓶颈） */
    FRESULT fr = f_opendir(&g_fat_dirs[slot].dir, path);
    if (fr != FR_OK)
    {
        system_lock(SYS_LOCK_FILE);
        g_fat_dirs[slot].used = 0;
        system_unlock(SYS_LOCK_FILE);
        return -ENOENT;
    }
    d->priv = &g_fat_dirs[slot];
    return 0;
}

/** @brief 读取下一个 FatFs 目录项：1 有项，0 结束，< 0 负 errno */
static int fat_vfs_readdir(vfs_dir_t *d, vfs_dirent_t *ent)
{
    fat_dir_t *s = (fat_dir_t *)d->priv;
    if (!s)
        return -EBADF;
    FILINFO info;
    FRESULT fr = f_readdir(&s->dir, &info);
    if (fr != FR_OK)
        return -EIO;
    if (info.fname[0] == '\0')
        return 0; /* 结束 */

    memset(ent, 0, sizeof(*ent));
    ent->type = (info.fattrib & AM_DIR) ? VFS_TYPE_DIR : VFS_TYPE_FILE;
    ent->size = (uint32_t)info.fsize;
    strncpy(ent->name, info.fname, VFS_NAME_MAX);
    ent->name[VFS_NAME_MAX] = '\0';
    return 1;
}

/** @brief 关闭 FatFs 目录：f_closedir 后释放目录槽 */
static int fat_vfs_closedir(vfs_dir_t *d)
{
    fat_dir_t *s = (fat_dir_t *)d->priv;
    if (!s)
        return 0;
    int rc = (f_closedir(&s->dir) == FR_OK) ? 0 : -EIO;
    system_lock(SYS_LOCK_FILE);
    s->used = 0;
    system_unlock(SYS_LOCK_FILE);
    d->priv = NULL;
    return rc;
}
#endif /* VFS_ENABLE_DIR */

/* ================================================================
 *  虚函数表
 * ================================================================ */

const vfs_fs_ops_t vfs_fatfs_ops = {
    .name = "fatfs",
    .caps = VFS_CAP_READ | VFS_CAP_WRITE | VFS_CAP_APPEND | VFS_CAP_SEEK |
            VFS_CAP_TRUNCATE | VFS_CAP_DIR | VFS_CAP_RENAME | VFS_CAP_MKDIR |
            VFS_CAP_STATVFS,
    .mount = fat_vfs_mount,
    .unmount = fat_vfs_unmount,
    .open = fat_vfs_open,
    .read = fat_vfs_read,
    .write = fat_vfs_write,
    .lseek = fat_vfs_lseek,
    .close = fat_vfs_close,
    .tell = fat_vfs_tell,
    .sync = fat_vfs_sync,
    .truncate = fat_vfs_truncate,
    .stat = fat_vfs_stat,
    .unlink = fat_vfs_unlink,
    .rename = fat_vfs_rename,
    .mkdir = fat_vfs_mkdir,
    .statvfs = fat_vfs_statvfs,
#if VFS_ENABLE_DIR
    .opendir = fat_vfs_opendir,
    .readdir = fat_vfs_readdir,
    .closedir = fat_vfs_closedir,
#else
    .opendir = NULL,
    .readdir = NULL,
    .closedir = NULL,
#endif
};

#endif /* VFS_ENABLE_FATFS */
