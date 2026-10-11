/**
 * @file vfs_rawflash.c
 * @brief 裸 flash 文件系统后端 —— VFS 适配层（核心逻辑在 rawflash/rawflash.c）
 *
 * 本文件只做"VFS 会话 ↔ rawflash 核心"的胶水：静态注册表查找、句柄池、
 * 写-写互斥、参数/标志转换。掉电安全的块区读写全部委托给 rawflash 核心
 * （见 rawflash.h），故核心可脱离 VFS 独立复用。
 *
 * 设计要点：
 *  - 文件 = 静态注册表里的一条，映射到一个分区（vfs_bdev_t）。
 *  - 写意图打开登记同文件写-写互斥（同一 entry 不允许两个写档）。
 *  - 写意图以 close 为提交点：close 之前 footer 仍无效，掉电读不到半截数据。
 */

#include "vfs_rawflash.h"
#include "rawflash.h"
#include <string.h>
#include <errno.h>
#include "port.h"

/* ================================================================
 *  每句柄槽 + 静态池
 * ================================================================ */

/** @brief 一个打开会话：命中条目 + 是否写档（互斥键）+ rawflash 核心上下文 */
typedef struct
{
    uint8_t used;                            /**< 槽位占用标记 */
    uint8_t is_write;                        /**< 1=写档（WRITE/RDWR），用于写-写互斥 */
    const vfs_rawflash_entry_t *entry;       /**< 命中的静态注册条目 */
    rawflash_ctx_t ctx;                      /**< rawflash 核心会话上下文 */
} raw_slot_t;

typedef struct
{
    uint8_t used;                      /**< 槽位占用标记 */
    const vfs_rawflash_vol_t *vol;     /**< 所属卷（静态注册表） */
    size_t idx;                        /**< 下一次 readdir 要返回的条目下标 */
} raw_dir_t;

/** @brief rawflash 文件句柄池（静态池，零堆） */
static raw_slot_t g_raw_files[VFS_MAX_OPEN_FILES];
#if VFS_ENABLE_DIR
/** @brief rawflash 目录句柄池（静态池，零堆） */
static raw_dir_t g_raw_dirs[VFS_MAX_OPEN_DIRS];
#endif

/**
 * @brief 分配 rawflash 文件句柄槽，并在写意图下做写-写互斥
 * @details 用后端池锁 SYS_LOCK_FILE 保护整个"查重 + 分配"临界区：
 *          want_write 时若同 entry 已有写档则拒绝（-EBUSY），保证同一文件
 *          不会有两个写会话交错（append 与整擦写一并保护）。
 * @param e          目标条目（并发检测键；可为 NULL）
 * @param want_write 1=写意图（含 append），需独占；0=只读
 * @param err_out    失败时输出负 errno（-EBUSY 写冲突，-ENFILE 池满）
 * @return 空闲槽指针；失败返回 NULL 并写 *err_out
 */
static raw_slot_t *raw_slot_alloc_guarded(const vfs_rawflash_entry_t *e, int want_write, int *err_out)
{
    raw_slot_t *slot = NULL;
    int err = 0;
    system_lock(SYS_LOCK_FILE);
    if (want_write && e)
    {
        for (int i = 0; i < VFS_MAX_OPEN_FILES; i++)
        {
            if (g_raw_files[i].used && g_raw_files[i].entry == e && g_raw_files[i].is_write)
            {
                err = -EBUSY;
                goto out;
            }
        }
    }
    for (int i = 0; i < VFS_MAX_OPEN_FILES; i++)
    {
        if (!g_raw_files[i].used)
        {
            memset(&g_raw_files[i], 0, sizeof(g_raw_files[i]));
            g_raw_files[i].used = 1;
            g_raw_files[i].entry = e;
            g_raw_files[i].is_write = want_write ? 1 : 0;
            slot = &g_raw_files[i];
            break;
        }
    }
    if (!slot)
        err = -ENFILE;
out:
    system_unlock(SYS_LOCK_FILE);
    if (err_out)
        *err_out = err;
    return slot;
}

/** @brief 释放 rawflash 文件句柄槽（清零后归还池） */
static void raw_slot_free(raw_slot_t *s)
{
    if (!s)
        return;
    system_lock(SYS_LOCK_FILE);
    memset(s, 0, sizeof(*s));
    system_unlock(SYS_LOCK_FILE);
}

/** @brief 按相对路径在静态注册表中查找条目 */
static const vfs_rawflash_entry_t *raw_entry_find(const vfs_rawflash_vol_t *vol, const char *rel)
{
    if (!vol || !rel)
        return NULL;
    for (size_t i = 0; i < vol->count; i++)
    {
        if (vol->entries[i].name && strcmp(vol->entries[i].name, rel) == 0)
            return &vol->entries[i];
    }
    return NULL;
}

/* ================================================================
 *  backend 回调
 * ================================================================ */

/**
 * @brief 挂载 rawflash 卷：校验静态注册表与各分区几何一致性
 * @return 0 成功；-EINVAL 注册表为空或某分区容量/块大小不足
 */
static int raw_mount(vfs_mount_t *mp)
{
    const vfs_rawflash_vol_t *vol = (const vfs_rawflash_vol_t *)mp->fs_ctx;
    if (!vol || !vol->entries || vol->count == 0)
        return -EINVAL;

    /* 校验每个分区的容量一致性 */
    for (size_t i = 0; i < vol->count; i++)
    {
        const vfs_rawflash_entry_t *e = &vol->entries[i];
        if (!e->name || !e->bdev || e->bdev->blk_count < 2 ||
            e->bdev->blk_size < (uint32_t)(2 * rawflash_footer_size()))
            return -EINVAL;
        uint32_t cap = (e->bdev->blk_count - 1) * e->bdev->blk_size;
        if (e->size > cap)
            return -EINVAL; /* 注册容量超过数据区 → 配置错误 */
    }
    return 0;
}

/** @brief 卸载 rawflash 卷（无持久状态需释放，直接返回 0） */
static int raw_unmount(vfs_mount_t *mp)
{
    (void)mp;
    return 0;
}

/**
 * @brief 打开一个静态注册的"文件"
 * @details 参数/标志转换后委托 rawflash 核心：写意图（O_WRONLY/O_RDWR/O_TRUNC）
 *          会整区擦除并把写游标置 0；只读模式绝不擦/写，仅读 footer 得长度。
 *          未注册文件名直接 -ENOENT；只读条目被写 → -EROFS。
 * @return 0 成功；-ENOENT/-EROFS/-EBUSY/-ENFILE，其余负 errno
 */
static int raw_open(vfs_file_t *f, const char *rel, int flags)
{
    const vfs_rawflash_vol_t *vol = (const vfs_rawflash_vol_t *)f->mp->fs_ctx;
    const vfs_rawflash_entry_t *e = raw_entry_find(vol, rel);
    if (!e)
        return -ENOENT; /* 未注册文件 → 打开失败 */

    int acc = flags & 0x3;
    int is_write = (acc == VFS_O_WRONLY) || (acc == VFS_O_RDWR);
    int trunc = (flags & VFS_O_TRUNC) ? 1 : 0;
#if VFS_RAWFLASH_ENABLE_APPEND
    int append = (flags & VFS_O_APPEND) && !trunc; /* 截断优先，走整擦路径 */
#else
    int append = 0;
#endif

    if ((e->flags & VFS_RAWRO) && (is_write || trunc || append || (flags & VFS_O_CREAT)))
        return -EROFS;

    int want_write = is_write || trunc || append;
    int aerr = 0;
    raw_slot_t *s = raw_slot_alloc_guarded(e, want_write, &aerr);
    if (!s)
        return aerr;

    rawflash_ctx_init(&s->ctx, e->bdev, e->size);

    int mode = (acc == VFS_O_WRONLY) ? RAWFLASH_MODE_WRITE
               : (acc == VFS_O_RDWR) ? RAWFLASH_MODE_RDWR
                                     : RAWFLASH_MODE_READ;
    int rc = rawflash_open(&s->ctx, mode, append, trunc);
    if (rc < 0)
    {
        raw_slot_free(s);
        return rc;
    }
    f->priv = s;
    return 0;
}

/** @brief 读取（委托核心，只读不越界） */
static ssize_t raw_read(vfs_file_t *f, void *buf, size_t n)
{
    raw_slot_t *s = (raw_slot_t *)f->priv;
    return s ? rawflash_read(&s->ctx, buf, n) : -EBADF;
}

/** @brief 写入（委托核心，顺序追加/整区重写） */
static ssize_t raw_write(vfs_file_t *f, const void *buf, size_t n)
{
    raw_slot_t *s = (raw_slot_t *)f->priv;
    return s ? rawflash_write(&s->ctx, buf, n) : -EBADF;
}

/** @brief 移动读游标（委托核心；写模式返回 -ENOTSUP） */
static off_t raw_lseek(vfs_file_t *f, off_t off, int whence)
{
    raw_slot_t *s = (raw_slot_t *)f->priv;
    if (!s)
        return -EBADF;
    int w = (whence == VFS_SEEK_SET) ? RAWFLASH_SEEK_SET
            : (whence == VFS_SEEK_CUR) ? RAWFLASH_SEEK_CUR
            : (whence == VFS_SEEK_END) ? RAWFLASH_SEEK_END
                                       : -1;
    if (w < 0)
        return -EINVAL;
    return rawflash_lseek(&s->ctx, off, w);
}

/** @brief 返回当前游标位置 */
static off_t raw_tell(vfs_file_t *f)
{
    raw_slot_t *s = (raw_slot_t *)f->priv;
    return s ? rawflash_tell(&s->ctx) : -EBADF;
}

/** @brief 刷缓存：直接写 flash，无缓存，恒返回 0 */
static int raw_sync(vfs_file_t *f)
{
    (void)f;
    return 0;
}

/**
 * @brief 截断：仅支持截断到 0（= 擦除整个分区并进入写模式）
 * @return 0 成功；-EBADF 句柄无效，-ENOTSUP len 非 0，< 0 底层擦除失败
 */
static int raw_truncate(vfs_file_t *f, off_t len)
{
    raw_slot_t *s = (raw_slot_t *)f->priv;
    if (!s)
        return -EBADF;
    if (len != 0)
        return -ENOTSUP; /* 仅支持截断到 0（= 擦除重写） */
    return rawflash_truncate(&s->ctx);
}

/**
 * @brief 关闭文件：写模式提交 footer，随后释放句柄
 * @details 写意图"close 才落 footer"，close 之前数据虽已写 flash 但不可见。
 * @return 0 成功；写模式 footer 写入失败返回负 errno
 */
static int raw_close(vfs_file_t *f)
{
    raw_slot_t *s = (raw_slot_t *)f->priv;
    if (!s)
        return 0;
    int rc = rawflash_close(&s->ctx);
    raw_slot_free(s);
    f->priv = NULL;
    return rc;
}

/**
 * @brief 查询文件状态：文件长度来自 footer，footer 无效时为 0
 * @return 0 成功；-ENOENT 未注册
 */
static int raw_stat(vfs_mount_t *mp, const char *rel, vfs_stat_t *st)
{
    const vfs_rawflash_vol_t *vol = (const vfs_rawflash_vol_t *)mp->fs_ctx;
    const vfs_rawflash_entry_t *e = raw_entry_find(vol, rel);
    if (!e)
        return -ENOENT;

    st->type = VFS_TYPE_FILE;
    st->size = rawflash_visible_len(e->bdev, e->size);
    return 0;
}

/**
 * @brief 删除文件：擦除整个分区（含元数据块）即视为删除
 * @return 0 成功；-ENOENT 未注册，-EROFS 只读条目
 */
static int raw_unlink(vfs_mount_t *mp, const char *rel)
{
    const vfs_rawflash_vol_t *vol = (const vfs_rawflash_vol_t *)mp->fs_ctx;
    const vfs_rawflash_entry_t *e = raw_entry_find(vol, rel);
    if (!e)
        return -ENOENT;
    if (e->flags & VFS_RAWRO)
        return -EROFS;
    return rawflash_erase(e->bdev);
}

#if VFS_ENABLE_DIR
/**
 * @brief 打开目录：仅支持卷根目录（扁平命名空间）
 * @return 0 成功；-ENOTSUP 非根目录，-ENFILE 目录池满
 */
static int raw_opendir(vfs_dir_t *d, const char *rel)
{
    /* 仅支持卷根目录（rawflash 为扁平命名空间） */
    if (rel[0] != '\0')
        return -ENOTSUP;

    system_lock(SYS_LOCK_FILE);
    for (int i = 0; i < VFS_MAX_OPEN_DIRS; i++)
    {
        if (!g_raw_dirs[i].used)
        {
            g_raw_dirs[i].used = 1;
            g_raw_dirs[i].vol = (const vfs_rawflash_vol_t *)d->mp->fs_ctx;
            g_raw_dirs[i].idx = 0;
            system_unlock(SYS_LOCK_FILE);
            d->priv = &g_raw_dirs[i];
            return 0;
        }
    }
    system_unlock(SYS_LOCK_FILE);
    return -ENFILE;
}

/**
 * @brief 读取下一个静态注册条目
 * @return 1 有项；0 结束；-EBADF 句柄无效
 */
static int raw_readdir(vfs_dir_t *d, vfs_dirent_t *ent)
{
    raw_dir_t *it = (raw_dir_t *)d->priv;
    if (!it || !it->vol)
        return -EBADF;

    const vfs_rawflash_vol_t *vol = it->vol;
    if (it->idx >= vol->count)
        return 0; /* 结束 */

    const vfs_rawflash_entry_t *e = &vol->entries[it->idx++];
    memset(ent, 0, sizeof(*ent));
    ent->type = VFS_TYPE_FILE;
    ent->size = rawflash_visible_len(e->bdev, e->size);
    strncpy(ent->name, e->name, VFS_NAME_MAX);
    ent->name[VFS_NAME_MAX] = '\0';
    return 1; /* 有项 */
}

/** @brief 关闭目录：清零目录句柄槽并置 d->priv = NULL */
static int raw_closedir(vfs_dir_t *d)
{
    raw_dir_t *it = (raw_dir_t *)d->priv;
    if (it)
    {
        system_lock(SYS_LOCK_FILE);
        memset(it, 0, sizeof(*it));
        system_unlock(SYS_LOCK_FILE);
    }
    d->priv = NULL;
    return 0;
}
#endif /* VFS_ENABLE_DIR */

/* ================================================================
 *  虚函数表
 * ================================================================ */

const vfs_fs_ops_t vfs_rawflash_ops = {
    .name = "rawflash",
    .caps = VFS_CAP_READ | VFS_CAP_WRITE | VFS_CAP_TRUNCATE | VFS_CAP_SEEK | VFS_CAP_DIR
#if VFS_RAWFLASH_ENABLE_APPEND
            | VFS_CAP_APPEND
#endif
    ,
    .mount = raw_mount,
    .unmount = raw_unmount,
    .open = raw_open,
    .read = raw_read,
    .write = raw_write,
    .lseek = raw_lseek,
    .close = raw_close,
    .tell = raw_tell,
    .sync = raw_sync,
    .truncate = raw_truncate,
    .stat = raw_stat,
    .unlink = raw_unlink,
    .rename = NULL,
    .mkdir = NULL,
    .statvfs = NULL,
#if VFS_ENABLE_DIR
    .opendir = raw_opendir,
    .readdir = raw_readdir,
    .closedir = raw_closedir,
#else
    .opendir = NULL,
    .readdir = NULL,
    .closedir = NULL,
#endif
};
