/**
 * @file storage_drv_sd.h
 * @brief SD 卡介质驱动接口 —— Zephyr disk_access → vfs_storage_t
 *
 * 与 @ref storage_drv_fal 同层（装配/驱动层）：把"一整张卡"包装成 VFS 的
 * `vfs_storage_t`，供 `vfs_bdev_t` 切片。核心（vfs_core / vfs_bdev / backend）
 * 不依赖本文件，也不依赖任何 RTOS 头。
 *
 * @section layering 在 VFS 分层中的位置
 * @verbatim
 *   storage_drv_sd（本文件）   → vfs_storage_t（整卡：read/prog，按字节偏移）
 *          ↓
 *   vfs_bdev_t                 → 卡内切片（一卡一卷时 = 整卡：offset 0）
 *          ↓
 *   vfs_fatfs.c（自带 diskio） → FatFs 只看到 bdev 的相对扇区
 * @endverbatim
 *
 * @section limits 有意保留的限制（当前只覆盖"一张卡、一个 FAT 卷"）
 *  - 只支持 **512 字节对齐**的整扇区访问（Zephyr disk API 以扇区为单位）：
 *    非对齐返回 -EINVAL。FatFs 的 diskio 天然按扇区对齐，一卡一 FAT 卷够用；
 *    若要在 SD 上挂 littlefs / rawflash，需先在此补 bounce buffer（读改写）。
 *  - 不提供 erase（`vfs_storage_t.erase == NULL`）：SD 内部 FTL 自行管理擦除，
 *    `vfs_bdev_erase()` 对 SD 返回 -EINVAL。FatFs 从不调用擦除。
 *  - 一次只绑定一张卡：几何在首次探测时快照，**换卡需重启**。
 */

#ifndef STORAGE_DRV_SD_H
#define STORAGE_DRV_SD_H

#include "storage_drv.h"

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * @brief 默认 Zephyr 磁盘名
 *
 * 对应 devicetree 里生成的 disk 名（见具体盘驱动的 `CONFIG_*_DISK_NAME`，
 * 常见值 "SD" / "SDMMC"）。可用 -D 覆盖。
 */
#ifndef STORAGE_SD_DISK_NAME
#define STORAGE_SD_DISK_NAME "SD"
#endif

    /**
     * @brief 探测 SD 卡并取得其 vfs_storage_t
     *
     * 每次调用都会重新探测（disk_access_init + status + ioctl 读几何），
     * 因此"卡不在"时返回 NULL，插入卡后再调一次即可拿到实例。
     *
     * @param disk_name Zephyr 磁盘名；NULL 或空串时用 STORAGE_SD_DISK_NAME
     * @return 静态 `vfs_storage_t` 指针（bdev 持有该指针，生命周期同进程）；
     *         NULL = 卡不在 / 盘驱动未就绪 / 扇区大小非 512 / 容量为 0 /
     *                盘名与已绑定实例不一致（换卡需重启）
     * @note 未启用 VFS_ENABLE_SD 时本函数不存在调用点（TU 为空）。
     */
    const vfs_storage_t *storage_drv_sd_storage(const char *disk_name);

#ifdef __cplusplus
}
#endif

#endif /* STORAGE_DRV_SD_H */
