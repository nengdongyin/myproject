/**
 * @file vfs_littlefs.c
 * @brief littlefs 后端实现（原生 lfs_* API + vfs_bdev_t）
 *
 * 见 vfs_littlefs.h。每个挂载点一个 lfs_t（静态池），lfs_config 的块
 * 读写回调转发到 vfs_bdev_t。文件/目录句柄与每文件 cache 均取自静态池。
 */

#include "vfs_littlefs.h"
#include <lfs.h>
#include <string.h>
#include <errno.h>
#include "port.h"

/* ================================================================
 *  静态池
 * ================================================================ */

typedef struct
{
    uint8_t used;                              /**< 挂载槽占用标记 */
    lfs_t lfs;                                 /**< littlefs 实例 */
    struct lfs_config cfg;                     /**< littlefs 配置（指向下方缓冲） */
    const vfs_bdev_t *bdev;                    /**< 对应的分区切片 */
    uint8_t read_buffer[VFS_LFS_CACHE_SIZE];   /**< littlefs 读 cache 缓冲 */
    uint8_t prog_buffer[VFS_LFS_CACHE_SIZE];   /**< littlefs 编程 cache 缓冲 */
    uint8_t lookahead_buffer[VFS_LFS_LOOKAHEAD]; /**< littlefs lookahead 缓冲 */
} lfs_mnt_t;

typedef struct
{
    uint8_t used;                          /**< 文件句柄槽占用标记 */
    lfs_file_t file;                       /**< littlefs 文件句柄 */
    struct lfs_file_config cfg;            /**< 每文件配置（cache 指向下方） */
    uint8_t cache[VFS_LFS_FILE_CACHE];     /**< 每文件 cache 缓冲（零堆） */
} lfs_file_slot_t;

typedef struct
{
    uint8_t used;      /**< 目录句柄槽占用标记 */
    lfs_dir_t dir;     /**< littlefs 目录句柄 */
} lfs_dir_slot_t;

/** @brief littlefs 挂载槽池（每挂载点一个 lfs_t） */
static lfs_mnt_t g_lfs_mnts[VFS_MAX_MOUNTS];
/** @brief littlefs 文件句柄池（静态池，零堆） */
static lfs_file_slot_t g_lfs_files[VFS_MAX_OPEN_FILES];
#if VFS_ENABLE_DIR
/** @brief littlefs 目录句柄池（静态池，零堆） */
static lfs_dir_slot_t g_lfs_dirs[VFS_MAX_OPEN_DIRS];
#endif

/**
 * @brief 分配一个 littlefs 文件句柄槽
 * @details 用后端池锁 SYS_LOCK_FILE 保护静态池；分配时把每文件 cache 绑定到 cfg。
 * @return 空闲槽指针；池满返回 NULL
 */
static lfs_file_slot_t *lfs_slot_alloc(void)
{
    system_lock(SYS_LOCK_FILE);
    for (int i = 0; i < VFS_MAX_OPEN_FILES; i++)
    {
        if (!g_lfs_files[i].used)
        {
            memset(&g_lfs_files[i], 0, sizeof(g_lfs_files[i]));
            g_lfs_files[i].used = 1;
            g_lfs_files[i].cfg.buffer = g_lfs_files[i].cache;
            system_unlock(SYS_LOCK_FILE);
            return &g_lfs_files[i];
        }
    }
    system_unlock(SYS_LOCK_FILE);
    return NULL;
}

/** @brief 释放 littlefs 文件句柄槽（清零后归还池） */
static void lfs_slot_free(lfs_file_slot_t *s)
{
    if (!s)
        return;
    system_lock(SYS_LOCK_FILE);
    memset(s, 0, sizeof(*s));
    system_unlock(SYS_LOCK_FILE);
}

/* ================================================================
 *  errno 映射
 * ================================================================ */

/**
 * @brief 把 littlefs 返回码映射为 VFS 负 errno
 * @details 统一负 errno 契约：未知/损坏错误归到 -EIO。
 */
static int lfs_to_errno(int e)
{
    switch (e)
    {
    case LFS_ERR_OK:
        return 0;
    case LFS_ERR_IO:
        return -EIO;
    case LFS_ERR_CORRUPT:
        return -EIO;
    case LFS_ERR_NOENT:
        return -ENOENT;
    case LFS_ERR_EXIST:
        return -EEXIST;
    case LFS_ERR_NOTDIR:
        return -ENOTDIR;
    case LFS_ERR_ISDIR:
        return -EISDIR;
    case LFS_ERR_NOTEMPTY:
        return -ENOTEMPTY;
    case LFS_ERR_BADF:
        return -EBADF;
    case LFS_ERR_FBIG:
        return -EFBIG;
    case LFS_ERR_INVAL:
        return -EINVAL;
    case LFS_ERR_NOSPC:
        return -ENOSPC;
    case LFS_ERR_NOMEM:
        return -ENOMEM;
    default:
        return -EIO;
    }
}

/* ================================================================
 *  块设备回调（转发到 vfs_bdev_t）
 * ================================================================ */

/** @brief littlefs 块设备读回调：块号+块内偏移 → vfs_bdev 字节读 */
static int lfs_bd_read(const struct lfs_config *c, lfs_block_t block,
                       lfs_off_t off, void *buffer, lfs_size_t size)
{
    lfs_mnt_t *m = (lfs_mnt_t *)c->context;
    int rc = vfs_bdev_read_bytes(m->bdev, block * c->block_size + off, buffer, size);
    return (rc < 0) ? LFS_ERR_IO : 0;
}

/** @brief littlefs 块设备编程回调：块号+块内偏移 → vfs_bdev 字节写 */
static int lfs_bd_prog(const struct lfs_config *c, lfs_block_t block,
                       lfs_off_t off, const void *buffer, lfs_size_t size)
{
    lfs_mnt_t *m = (lfs_mnt_t *)c->context;
    int rc = vfs_bdev_prog_bytes(m->bdev, block * c->block_size + off, buffer, size);
    return (rc < 0) ? LFS_ERR_IO : 0;
}

/** @brief littlefs 块设备擦除回调：按块号擦 1 个块 */
static int lfs_bd_erase(const struct lfs_config *c, lfs_block_t block)
{
    lfs_mnt_t *m = (lfs_mnt_t *)c->context;
    int rc = vfs_bdev_erase(m->bdev, (uint32_t)block, 1);
    return (rc < 0) ? LFS_ERR_IO : 0;
}

/** @brief littlefs 块设备同步回调：底层写直达，无缓冲需刷 */
static int lfs_bd_sync(const struct lfs_config *c)
{
    (void)c;
    return 0;
}

/** @brief 用 bdev 填充 lfs_config */
static void lfs_build_cfg(lfs_mnt_t *m, const vfs_bdev_t *bdev)
{
    struct lfs_config *c = &m->cfg;
    memset(c, 0, sizeof(*c));
    m->bdev = bdev;
    c->context = m;
    c->read = lfs_bd_read;
    c->prog = lfs_bd_prog;
    c->erase = lfs_bd_erase;
    c->sync = lfs_bd_sync;
    c->read_size = VFS_LFS_READ_SIZE;
    c->prog_size = VFS_LFS_PROG_SIZE;
    c->block_size = bdev->blk_size;
    c->block_count = bdev->blk_count;
    c->cache_size = VFS_LFS_CACHE_SIZE;
    c->lookahead_size = VFS_LFS_LOOKAHEAD;
    c->block_cycles = VFS_LFS_BLOCK_CYCLES;
    c->read_buffer = m->read_buffer;
    c->prog_buffer = m->prog_buffer;
    c->lookahead_buffer = m->lookahead_buffer;
    c->name_max = VFS_NAME_MAX;
}

/* ================================================================
 *  backend 回调
 * ================================================================ */

/**
 * @brief 挂载 littlefs 卷
 * @details 先校验 bdev 与几何参数（read ≤ prog ≤ block、cache ≥ prog、block
 *          为 prog 整数倍），再从静态池取一个空闲挂载槽，构建 lfs_config 并
 *          调用 lfs_mount；失败返回负 errno（LFS_ERR_CORRUPT → -EIO）。
 * @return 0 成功；-EINVAL 几何非法，-ENOSPC 挂载槽满，< 0 lfs 错误码
 */
static int lfs_vfs_mount(vfs_mount_t *mp)
{
    const vfs_bdev_t *bdev = mp->storage_dev;
    if (!bdev || bdev->blk_size == 0 || bdev->blk_count == 0)
        return -EINVAL;
    /* 几何合法性：read ≤ prog ≤ block；cache 为 read/prog 整数倍且 ≥ prog；
     * lookahead 为 8 的倍数；每文件 cache 为 prog 整数倍；block 为 prog 整数倍 */
    if (VFS_LFS_READ_SIZE == 0 || VFS_LFS_PROG_SIZE < VFS_LFS_READ_SIZE ||
        (VFS_LFS_PROG_SIZE % VFS_LFS_READ_SIZE) != 0 ||
        VFS_LFS_CACHE_SIZE < VFS_LFS_PROG_SIZE ||
        (VFS_LFS_CACHE_SIZE % VFS_LFS_READ_SIZE) != 0 ||
        (VFS_LFS_CACHE_SIZE % VFS_LFS_PROG_SIZE) != 0 ||
        (VFS_LFS_LOOKAHEAD % 8) != 0 ||
        VFS_LFS_FILE_CACHE < VFS_LFS_PROG_SIZE ||
        (VFS_LFS_FILE_CACHE % VFS_LFS_PROG_SIZE) != 0 ||
        VFS_LFS_PROG_SIZE > bdev->blk_size ||
        (bdev->blk_size % VFS_LFS_PROG_SIZE) != 0)
        return -EINVAL;

    lfs_mnt_t *m = NULL;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++)
    {
        if (!g_lfs_mnts[i].used)
        {
            m = &g_lfs_mnts[i];
            memset(m, 0, sizeof(*m));
            m->used = 1;
            break;
        }
    }
    if (!m)
        return -ENOSPC;

    lfs_build_cfg(m, bdev);
    int rc = lfs_mount(&m->lfs, &m->cfg);
    if (rc < 0)
    {
        m->used = 0;
        return lfs_to_errno(rc); /* LFS_ERR_CORRUPT → -EIO，需先 format */
    }
    mp->fs_ctx = m;
    return 0;
}

/** @brief 卸载 littlefs 卷：lfs_unmount 后释放挂载槽 */
static int lfs_vfs_unmount(vfs_mount_t *mp)
{
    lfs_mnt_t *m = (lfs_mnt_t *)mp->fs_ctx;
    if (!m)
        return 0;
    lfs_unmount(&m->lfs);
    m->used = 0;
    mp->fs_ctx = NULL;
    return 0;
}

/**
 * @brief 格式化 littlefs 分区（破坏性）
 * @details 借用一个空闲挂载槽的 cfg/buffers 完成 lfs_format，用完立即释放，
 *          避免与 mount 并发借用同一槽。
 * @return 0 成功；-EINVAL 无 bdev，-ENOSPC 无空闲槽，< 0 lfs 错误码
 */
int vfs_littlefs_format(vfs_mount_t *mp)
{
    const vfs_bdev_t *bdev = mp->storage_dev;
    if (!bdev)
        return -EINVAL;

    /* 借一个空闲挂载槽的 cfg/buffers（标记占用，避免与 mount 并发借用同槽） */
    lfs_mnt_t *m = NULL;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++)
    {
        if (!g_lfs_mnts[i].used)
        {
            m = &g_lfs_mnts[i];
            memset(m, 0, sizeof(*m));
            m->used = 1;
            break;
        }
    }
    if (!m)
        return -ENOSPC;

    lfs_build_cfg(m, bdev);
    int rc = lfs_to_errno(lfs_format(&m->lfs, &m->cfg));
    m->used = 0; /* 释放借用槽 */
    return rc;
}

/**
 * @brief 打开/创建 littlefs 文件
 * @details 把 VFS_O_* 逐位映射为 LFS_O_*，分配文件句柄槽后调用
 *          lfs_file_opencfg 并绑定每文件 cache。
 * @return 0 成功；-ENODEV 挂载上下文缺失，-ENFILE 句柄池满，< 0 lfs 错误码
 */
static int lfs_vfs_open(vfs_file_t *f, const char *rel, int flags)
{
    lfs_mnt_t *m = (lfs_mnt_t *)f->mp->fs_ctx;
    if (!m)
        return -ENODEV;

    int acc = flags & 0x3;
    int of = 0;
    if (acc == VFS_O_WRONLY)
        of |= LFS_O_WRONLY;
    else if (acc == VFS_O_RDWR)
        of |= LFS_O_RDWR;
    else
        of |= LFS_O_RDONLY;
    if (flags & VFS_O_CREAT)
        of |= LFS_O_CREAT;
    if (flags & VFS_O_EXCL)
        of |= LFS_O_EXCL;
    if (flags & VFS_O_TRUNC)
        of |= LFS_O_TRUNC;
    if (flags & VFS_O_APPEND)
        of |= LFS_O_APPEND;

    lfs_file_slot_t *s = lfs_slot_alloc();
    if (!s)
        return -ENFILE;

    int rc = lfs_file_opencfg(&m->lfs, &s->file, rel, of, &s->cfg);
    if (rc < 0)
    {
        lfs_slot_free(s);
        return lfs_to_errno(rc);
    }
    f->priv = s;
    return 0;
}

/** @brief 读取 littlefs 文件：返回实际字节数，< 0 负 errno */
static ssize_t lfs_vfs_read(vfs_file_t *f, void *buf, size_t n)
{
    lfs_file_slot_t *s = (lfs_file_slot_t *)f->priv;
    lfs_mnt_t *m = (lfs_mnt_t *)f->mp->fs_ctx;
    if (!s || !m)
        return -EBADF;
    lfs_ssize_t rc = lfs_file_read(&m->lfs, &s->file, buf, n);
    return (rc < 0) ? lfs_to_errno((int)rc) : (ssize_t)rc;
}

/** @brief 写入 littlefs 文件：返回实际字节数，< 0 负 errno */
static ssize_t lfs_vfs_write(vfs_file_t *f, const void *buf, size_t n)
{
    lfs_file_slot_t *s = (lfs_file_slot_t *)f->priv;
    lfs_mnt_t *m = (lfs_mnt_t *)f->mp->fs_ctx;
    if (!s || !m)
        return -EBADF;
    lfs_ssize_t rc = lfs_file_write(&m->lfs, &s->file, buf, n);
    return (rc < 0) ? lfs_to_errno((int)rc) : (ssize_t)rc;
}

/** @brief 移动 littlefs 文件偏移：映射 whence 后返回新绝对偏移 */
static off_t lfs_vfs_lseek(vfs_file_t *f, off_t off, int whence)
{
    lfs_file_slot_t *s = (lfs_file_slot_t *)f->priv;
    lfs_mnt_t *m = (lfs_mnt_t *)f->mp->fs_ctx;
    if (!s || !m)
        return -EBADF;

    int w;
    if (whence == VFS_SEEK_SET)
        w = LFS_SEEK_SET;
    else if (whence == VFS_SEEK_CUR)
        w = LFS_SEEK_CUR;
    else if (whence == VFS_SEEK_END)
        w = LFS_SEEK_END;
    else
        return -EINVAL;

    lfs_soff_t rc = lfs_file_seek(&m->lfs, &s->file, off, w);
    return (rc < 0) ? lfs_to_errno((int)rc) : (off_t)rc;
}

/** @brief 返回 littlefs 文件当前偏移 */
static off_t lfs_vfs_tell(vfs_file_t *f)
{
    lfs_file_slot_t *s = (lfs_file_slot_t *)f->priv;
    lfs_mnt_t *m = (lfs_mnt_t *)f->mp->fs_ctx;
    if (!s || !m)
        return -EBADF;
    lfs_soff_t rc = lfs_file_tell(&m->lfs, &s->file);
    return (rc < 0) ? lfs_to_errno((int)rc) : (off_t)rc;
}

/** @brief 同步 littlefs 文件缓存到介质 */
static int lfs_vfs_sync(vfs_file_t *f)
{
    lfs_file_slot_t *s = (lfs_file_slot_t *)f->priv;
    lfs_mnt_t *m = (lfs_mnt_t *)f->mp->fs_ctx;
    if (!s || !m)
        return -EBADF;
    return lfs_to_errno(lfs_file_sync(&m->lfs, &s->file));
}

/** @brief 截断/扩展 littlefs 文件到 len 字节 */
static int lfs_vfs_truncate(vfs_file_t *f, off_t len)
{
    lfs_file_slot_t *s = (lfs_file_slot_t *)f->priv;
    lfs_mnt_t *m = (lfs_mnt_t *)f->mp->fs_ctx;
    if (!s || !m)
        return -EBADF;
    return lfs_to_errno(lfs_file_truncate(&m->lfs, &s->file, (lfs_off_t)len));
}

/** @brief 关闭 littlefs 文件：lfs_file_close 后释放句柄槽 */
static int lfs_vfs_close(vfs_file_t *f)
{
    lfs_file_slot_t *s = (lfs_file_slot_t *)f->priv;
    lfs_mnt_t *m = (lfs_mnt_t *)f->mp->fs_ctx;
    if (!s)
        return 0;
    int rc = 0;
    if (m)
        rc = lfs_to_errno(lfs_file_close(&m->lfs, &s->file));
    lfs_slot_free(s);
    f->priv = NULL;
    return rc;
}

/** @brief 查询 littlefs 文件/目录状态 */
static int lfs_vfs_stat(vfs_mount_t *mp, const char *rel, vfs_stat_t *st)
{
    lfs_mnt_t *m = (lfs_mnt_t *)mp->fs_ctx;
    if (!m)
        return -ENODEV;
    struct lfs_info info;
    int rc = lfs_stat(&m->lfs, rel, &info);
    if (rc < 0)
        return lfs_to_errno(rc);
    st->type = (info.type == LFS_TYPE_DIR) ? VFS_TYPE_DIR : VFS_TYPE_FILE;
    st->size = (uint32_t)info.size;
    return 0;
}

/** @brief 删除 littlefs 文件/目录 */
static int lfs_vfs_unlink(vfs_mount_t *mp, const char *rel)
{
    lfs_mnt_t *m = (lfs_mnt_t *)mp->fs_ctx;
    if (!m)
        return -ENODEV;
    return lfs_to_errno(lfs_remove(&m->lfs, rel));
}

/** @brief 重命名 littlefs 文件/目录 */
static int lfs_vfs_rename(vfs_mount_t *mp, const char *from, const char *to)
{
    lfs_mnt_t *m = (lfs_mnt_t *)mp->fs_ctx;
    if (!m)
        return -ENODEV;
    return lfs_to_errno(lfs_rename(&m->lfs, from, to));
}

/** @brief 创建 littlefs 目录 */
static int lfs_vfs_mkdir(vfs_mount_t *mp, const char *rel)
{
    lfs_mnt_t *m = (lfs_mnt_t *)mp->fs_ctx;
    if (!m)
        return -ENODEV;
    return lfs_to_errno(lfs_mkdir(&m->lfs, rel));
}

/** @brief 查询 littlefs 卷容量：lfs_fs_size 返回已用块数 */
static int lfs_vfs_statvfs(vfs_mount_t *mp, vfs_statvfs_t *st)
{
    lfs_mnt_t *m = (lfs_mnt_t *)mp->fs_ctx;
    if (!m || !m->bdev)
        return -ENODEV;
    lfs_ssize_t used = lfs_fs_size(&m->lfs);
    if (used < 0)
        return lfs_to_errno((int)used);
    uint32_t total = m->bdev->blk_count * m->bdev->blk_size;
    st->total_bytes = total;
    st->free_bytes = (used > (lfs_ssize_t)m->bdev->blk_count)
                         ? 0
                         : (uint32_t)(m->bdev->blk_count - (uint32_t)used) * m->bdev->blk_size;
    return 0;
}

#if VFS_ENABLE_DIR
/** @brief 打开 littlefs 目录：分配目录槽并 lfs_dir_open */
static int lfs_vfs_opendir(vfs_dir_t *d, const char *rel)
{
    lfs_mnt_t *m = (lfs_mnt_t *)d->mp->fs_ctx;
    if (!m)
        return -ENODEV;

    system_lock(SYS_LOCK_FILE);
    int slot = -1;
    for (int i = 0; i < VFS_MAX_OPEN_DIRS; i++)
    {
        if (!g_lfs_dirs[i].used)
        {
            g_lfs_dirs[i].used = 1;
            slot = i;
            break;
        }
    }
    system_unlock(SYS_LOCK_FILE);
    if (slot < 0)
        return -ENFILE;

    /* 目录 I/O 不持全局池锁（避免跨卷瓶颈） */
    int rc = lfs_dir_open(&m->lfs, &g_lfs_dirs[slot].dir, rel);
    if (rc < 0)
    {
        system_lock(SYS_LOCK_FILE);
        g_lfs_dirs[slot].used = 0;
        system_unlock(SYS_LOCK_FILE);
        return lfs_to_errno(rc);
    }
    d->priv = &g_lfs_dirs[slot];
    return 0;
}

/** @brief 读取下一个 littlefs 目录项：1 有项，0 结束，< 0 负 errno */
static int lfs_vfs_readdir(vfs_dir_t *d, vfs_dirent_t *ent)
{
    lfs_dir_slot_t *s = (lfs_dir_slot_t *)d->priv;
    lfs_mnt_t *m = (lfs_mnt_t *)d->mp->fs_ctx;
    if (!s || !m)
        return -EBADF;

    struct lfs_info info;
    int rc = lfs_dir_read(&m->lfs, &s->dir, &info);
    if (rc < 0)
        return lfs_to_errno(rc);
    if (rc == 0)
        return 0; /* 结束 */

    memset(ent, 0, sizeof(*ent));
    ent->type = (info.type == LFS_TYPE_DIR) ? VFS_TYPE_DIR : VFS_TYPE_FILE;
    ent->size = (uint32_t)info.size;
    strncpy(ent->name, info.name, VFS_NAME_MAX);
    ent->name[VFS_NAME_MAX] = '\0';
    return 1;
}

/** @brief 关闭 littlefs 目录：lfs_dir_close 后释放目录槽 */
static int lfs_vfs_closedir(vfs_dir_t *d)
{
    lfs_dir_slot_t *s = (lfs_dir_slot_t *)d->priv;
    lfs_mnt_t *m = (lfs_mnt_t *)d->mp->fs_ctx;
    if (!s)
        return 0;
    int rc = 0;
    if (m)
        rc = lfs_to_errno(lfs_dir_close(&m->lfs, &s->dir));
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

const vfs_fs_ops_t vfs_littlefs_ops = {
    .name = "littlefs",
    .caps = VFS_CAP_READ | VFS_CAP_WRITE | VFS_CAP_APPEND | VFS_CAP_SEEK |
            VFS_CAP_TRUNCATE | VFS_CAP_DIR | VFS_CAP_RENAME | VFS_CAP_MKDIR |
            VFS_CAP_STATVFS,
    .mount = lfs_vfs_mount,
    .unmount = lfs_vfs_unmount,
    .open = lfs_vfs_open,
    .read = lfs_vfs_read,
    .write = lfs_vfs_write,
    .lseek = lfs_vfs_lseek,
    .close = lfs_vfs_close,
    .tell = lfs_vfs_tell,
    .sync = lfs_vfs_sync,
    .truncate = lfs_vfs_truncate,
    .stat = lfs_vfs_stat,
    .unlink = lfs_vfs_unlink,
    .rename = lfs_vfs_rename,
    .mkdir = lfs_vfs_mkdir,
    .statvfs = lfs_vfs_statvfs,
#if VFS_ENABLE_DIR
    .opendir = lfs_vfs_opendir,
    .readdir = lfs_vfs_readdir,
    .closedir = lfs_vfs_closedir,
#else
    .opendir = NULL,
    .readdir = NULL,
    .closedir = NULL,
#endif
};
