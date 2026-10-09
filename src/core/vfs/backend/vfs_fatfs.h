/**
 * @file vfs_fatfs.h
 * @brief FatFs 文件系统后端
 *
 * 直接调用 FatFs 原生 API（f_* / ff.h），块设备通过 vfs_bdev_t 提供（如 SD 卡）。
 * 需工程先引入 FatFs 库并把 VFS_ENABLE_FATFS 置 1。
 *
 * @section diskio diskio 集成
 * 本文件自带 disk_* 回调，把 FatFs 的扇区访问转发到 vfs_bdev_t
 * （约定扇区大小 512，与 SD 一致）。每个挂载点占用一个 FatFs 逻辑盘符（pdrv）。
 */

#ifndef VFS_FATFS_H
#define VFS_FATFS_H

#include "vfs.h"
#include "vfs_config.h"

#if VFS_ENABLE_FATFS

#ifdef __cplusplus
extern "C"
{
#endif

    /** @brief FatFs 后端虚函数表 */
    extern const vfs_fs_ops_t vfs_fatfs_ops;

#ifdef __cplusplus
}
#endif

#endif /* VFS_ENABLE_FATFS */

#endif /* VFS_FATFS_H */
