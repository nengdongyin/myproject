/**
 * @file vfs_bdev.c
 * @brief 块设备抽象实现 + FAL 适配器
 *
 * 统一按"分区内块号"访问，屏蔽底层是 flash 还是 SD。
 * FAL 适配器把现有 FAL 分区表（fal_cfg.h 的 FAL_PART_TABLE）包装成 vfs_bdev_t，
 * offset/长度直接复用分区表，避免偏移两处维护不同步。
 */

#include "vfs_bdev.h"
#include <string.h>
#include <errno.h>

#if VFS_ENABLE_FAL_BDEV
#include "fal.h"
#endif

void vfs_bdev_init(vfs_bdev_t *d, const char *name, const vfs_storage_t *s,
                   uint32_t byte_offset, uint32_t blk_size, uint32_t blk_count)
{
    if (!d)
        return;
    memset(d, 0, sizeof(*d));
    d->name = name;
    d->storage = s;
    d->offset = byte_offset;
    d->blk_size = blk_size;
    d->blk_count = blk_count;
}

uint32_t vfs_bdev_size(const vfs_bdev_t *d)
{
    if (!d)
        return 0;
    return d->blk_count * d->blk_size;
}

int vfs_bdev_read(const vfs_bdev_t *d, uint32_t blk, void *buf, uint32_t nblk)
{
    if (!d || !d->storage || !d->storage->read || !buf || d->blk_size == 0)
        return -EINVAL;
    /* 越界检查：防止 uint32_t 加法回绕 */
    if (blk > d->blk_count || nblk > d->blk_count - blk)
        return -EINVAL;

    int rc = d->storage->read((vfs_storage_t *)d->storage,
                              d->offset + blk * d->blk_size,
                              buf, (size_t)nblk * d->blk_size);
    return (rc < 0) ? -EIO : 0;
}

int vfs_bdev_prog(const vfs_bdev_t *d, uint32_t blk, const void *buf, uint32_t nblk)
{
    if (!d || !d->storage || !d->storage->prog || !buf || d->blk_size == 0)
        return -EINVAL;
    if (blk > d->blk_count || nblk > d->blk_count - blk)
        return -EINVAL;

    int rc = d->storage->prog((vfs_storage_t *)d->storage,
                              d->offset + blk * d->blk_size,
                              buf, (size_t)nblk * d->blk_size);
    return (rc < 0) ? -EIO : 0;
}

int vfs_bdev_erase(const vfs_bdev_t *d, uint32_t blk, uint32_t nblk)
{
    if (!d || !d->storage || !d->storage->erase || d->blk_size == 0)
        return -EINVAL;
    if (blk > d->blk_count || nblk > d->blk_count - blk)
        return -EINVAL;

    int rc = d->storage->erase((vfs_storage_t *)d->storage,
                               d->offset + blk * d->blk_size,
                               (size_t)nblk * d->blk_size);
    return (rc < 0) ? -EIO : 0;
}

int vfs_bdev_read_bytes(const vfs_bdev_t *d, uint32_t byte_off, void *buf, size_t n)
{
    if (!d || !d->storage || !d->storage->read || !buf)
        return -EINVAL;
    uint32_t total = vfs_bdev_size(d);
    if (byte_off > total || n > (size_t)(total - byte_off))
        return -EINVAL;

    int rc = d->storage->read((vfs_storage_t *)d->storage,
                              d->offset + byte_off, buf, n);
    return (rc < 0) ? -EIO : 0;
}

int vfs_bdev_prog_bytes(const vfs_bdev_t *d, uint32_t byte_off, const void *buf, size_t n)
{
    if (!d || !d->storage || !d->storage->prog || !buf)
        return -EINVAL;
    uint32_t total = vfs_bdev_size(d);
    if (byte_off > total || n > (size_t)(total - byte_off))
        return -EINVAL;

    int rc = d->storage->prog((vfs_storage_t *)d->storage,
                              d->offset + byte_off, buf, n);
    return (rc < 0) ? -EIO : 0;
}

#if VFS_ENABLE_FAL_BDEV

/* ================================================================
 *  FAL 适配器
 * ================================================================ */

/** @brief FAL 整设备 → vfs_storage 的 read/prog/erase 三回调 */
/** @brief FAL 整设备读回调：把 vfs_storage 偏移转发给 FAL flash read */
static int fal_storage_read(vfs_storage_t *s, uint32_t off, void *buf, size_t n)
{
    const struct fal_flash_dev *dev = (const struct fal_flash_dev *)s->ctx;
    return dev->ops.read((long)off, (uint8_t *)buf, n);
}

/** @brief FAL 整设备编程回调：把 vfs_storage 偏移转发给 FAL flash write */
static int fal_storage_prog(vfs_storage_t *s, uint32_t off, const void *buf, size_t n)
{
    const struct fal_flash_dev *dev = (const struct fal_flash_dev *)s->ctx;
    return dev->ops.write((long)off, (const uint8_t *)buf, n);
}

/** @brief FAL 整设备擦除回调：把 vfs_storage 偏移转发给 FAL flash erase */
static int fal_storage_erase(vfs_storage_t *s, uint32_t off, size_t n)
{
    const struct fal_flash_dev *dev = (const struct fal_flash_dev *)s->ctx;
    return dev->ops.erase((long)off, n);
}

/** @brief 取得（或创建）某 FAL flash 设备对应的持久 vfs_storage */
static vfs_storage_t *fal_storage_get(const struct fal_flash_dev *dev)
{
    /** @brief 持久 vfs_storage 池（函数内静态，生命周期同进程） */
    static vfs_storage_t s_pool[VFS_MAX_BDEVS];
    /** @brief 已创建的 vfs_storage 个数 */
    static int s_n;

    for (int i = 0; i < s_n; i++)
    {
        if (strcmp(s_pool[i].name, dev->name) == 0)
            return &s_pool[i];
    }
    if (s_n >= VFS_MAX_BDEVS)
        return NULL;

    vfs_storage_t *s = &s_pool[s_n++];
    s->name = dev->name;
    s->blk_size = (uint32_t)dev->blk_size;
    s->blk_count = (uint32_t)(dev->len / dev->blk_size);
    s->read = fal_storage_read;
    s->prog = fal_storage_prog;
    s->erase = fal_storage_erase;
    s->ctx = (void *)dev;
    return s;
}

int vfs_bdev_fal_init(vfs_bdev_t *d, const char *name, const char *fal_part)
{
    if (!d || !fal_part)
        return -EINVAL;

    /* 通过分区名在 FAL 分区表中查找 */
    const struct fal_partition *p = fal_partition_find(fal_part);
    if (!p)
        return -ENOENT;

    /* 找到分区所属的 flash 设备 */
    const struct fal_flash_dev *dev = fal_flash_device_find(p->flash_name);
    if (!dev || !dev->ops.read || dev->blk_size == 0)
        return -ENOENT;

    vfs_storage_t *s = fal_storage_get(dev);
    if (!s)
        return -ENOSPC;

    /* 分区切片：offset 来自 FAL 分区表（设备内相对偏移） */
    vfs_bdev_init(d, name, s, (uint32_t)p->offset,
                  (uint32_t)dev->blk_size,
                  (uint32_t)(p->len / dev->blk_size));
    d->ctx = p; /* 保留 FAL 分区指针，便于诊断 */
    return 0;
}

int vfs_bdev_fal_flash_init(vfs_bdev_t *d, const char *name,
                            const char *fal_flash, uint32_t offset, uint32_t size)
{
    if (!d || !fal_flash)
        return -EINVAL;

    const struct fal_flash_dev *dev = fal_flash_device_find(fal_flash);
    if (!dev || !dev->ops.read || dev->blk_size == 0)
        return -ENOENT;
    /* 对齐与边界校验 */
    if ((offset % dev->blk_size) != 0 || (size % dev->blk_size) != 0)
        return -EINVAL;
    if (offset > (uint32_t)dev->len || size > ((uint32_t)dev->len - offset))
        return -EINVAL;

    vfs_storage_t *s = fal_storage_get(dev);
    if (!s)
        return -ENOSPC;

    vfs_bdev_init(d, name, s, offset, (uint32_t)dev->blk_size,
                  (uint32_t)(size / dev->blk_size));
    return 0;
}

#endif /* VFS_ENABLE_FAL_BDEV */
