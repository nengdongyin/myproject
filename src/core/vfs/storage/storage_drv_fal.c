/**
 * @file storage_drv_fal.c
 * @brief FAL 介质驱动实现（装配/驱动层）
 *
 * 把 FAL flash 设备的 read/write/erase 包装成 @ref vfs_storage_t，供 VFS 的
 * bdev 切片使用。本文件是 VFS 与 FAL 的**唯一接触点**（核心 src/core/vfs 不
 * 再引用 fal.h），从 vfs_bdev.c 迁移而来。
 */

#include "storage_drv_fal.h"

#if STORAGE_DRV_FAL_ENABLE

#include "fal.h"
#include <string.h>
#include <errno.h>

/* ================================================================
 *  FAL flash 设备 → vfs_storage_t
 * ================================================================ */

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
    /** @brief 持久 vfs_storage 池（每物理设备一项；函数内静态，生命周期同进程） */
    static vfs_storage_t s_pool[VFS_MAX_STORAGES];
    /** @brief 已创建的 vfs_storage 个数 */
    static int s_n;

    for (int i = 0; i < s_n; i++)
    {
        if (strcmp(s_pool[i].name, dev->name) == 0)
            return &s_pool[i];
    }
    if (s_n >= VFS_MAX_STORAGES)
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

/* ================================================================
 *  对外接口
 * ================================================================ */

const vfs_storage_t *storage_drv_fal_storage(const char *fal_flash)
{
    if (!fal_flash)
        return NULL;

    const struct fal_flash_dev *dev = fal_flash_device_find(fal_flash);
    if (!dev || !dev->ops.read || dev->blk_size == 0)
        return NULL;

    return fal_storage_get(dev);
}

int storage_drv_fal_bdev(vfs_bdev_t *d, const char *name,
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

int storage_drv_fal_partition_bdev(vfs_bdev_t *d, const char *name, const char *fal_part)
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

#endif /* STORAGE_DRV_FAL_ENABLE */
