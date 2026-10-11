/**
 * @file storage_drv_fal.h
 * @brief FAL 介质驱动 —— 把 FAL flash 设备包装为 vfs_storage_t（装配/驱动层）
 *
 * FAL 只作为"介质驱动/几何提供者之一"出现在装配层，**不属于 VFS 核心**：
 * 本文件位于 src/core/vfs/storage/，其存在与否由 STORAGE_DRV_FAL_ENABLE 控制。
 * 若工程不使用 FAL，可不编译本驱动，改用其它 storage_drv_*（如裸 flash / SD）。
 *
 * 提供两种几何来源：
 *  - @ref storage_drv_fal_partition_bdev：几何取自 FAL 分区表
 *  - @ref storage_drv_fal_bdev：几何由调用方以 offset/size 给出（无需写进分区表）
 */

#ifndef STORAGE_DRV_FAL_H
#define STORAGE_DRV_FAL_H

#include "vfs.h"
#include "storage_drv.h"

/** @brief 是否编入 FAL 介质驱动。默认开启；纯自定义分区场景可置 0 并剔除本文件 */
#ifndef STORAGE_DRV_FAL_ENABLE
#define STORAGE_DRV_FAL_ENABLE 1
#endif

#if STORAGE_DRV_FAL_ENABLE

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief 取得（或创建）某 FAL flash 设备对应的持久 vfs_storage_t
     * @param fal_flash FAL flash 设备名（如 "rp2350_flash"）
     * @return 命中/新建返回 vfs_storage_t 指针；设备不存在或池满返回 NULL
     */
    const vfs_storage_t *storage_drv_fal_storage(const char *fal_flash);

    /**
     * @brief 由 FAL flash 设备 + 自定义 offset/size 构造分区切片
     * @param d         待填充的 bdev
     * @param name      该 bdev 在 VFS 中的名字
     * @param fal_flash FAL flash 设备名（如 "rp2350_flash"）
     * @param offset    设备内字节偏移（须块对齐）
     * @param size      区域大小（须块对齐）
     * @return 0 成功，-EINVAL 未对齐/越界，-ENOENT 设备不存在，-ENOSPC 池满
     */
    int storage_drv_fal_bdev(vfs_bdev_t *d, const char *name,
                             const char *fal_flash, uint32_t offset, uint32_t size);

    /**
     * @brief 从 FAL 分区表构造 bdev（复用分区表 offset/len）
     * @param d        待填充的 bdev
     * @param name     该 bdev 在 VFS 中的名字
     * @param fal_part 对应的 FAL 分区名
     * @return 0 成功，-ENOENT 分区/设备不存在，-ENOSPC 内部 storage 池满
     */
    int storage_drv_fal_partition_bdev(vfs_bdev_t *d, const char *name, const char *fal_part);

#ifdef __cplusplus
}
#endif

#endif /* STORAGE_DRV_FAL_ENABLE */

#endif /* STORAGE_DRV_FAL_H */
