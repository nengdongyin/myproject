/**
 * @file vfs_bdev.c
 * @brief 块设备抽象实现 —— 分区切片（介质驱动见 storage_drv_*）
 *
 * 统一按"分区内块号"访问，屏蔽底层是 flash 还是 SD。
 * 本文件只做分区切片与访问转发：具体介质的 read/prog/erase 由 vfs_storage_t
 * 提供（其实现位于装配/驱动层），核心不依赖 FAL 等任何具体框架。
 */

#include "vfs_bdev.h"
#include <string.h>
#include <errno.h>

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
