/**
 * @file storage_drv.h
 * @brief 存储介质驱动接口 —— 物理存储设备抽象（VFS 分层中的最底层）
 *
 * 本层只描述"一块物理存储介质"如何按字节偏移做 read/prog/erase，**与具体
 * 文件系统、与 VFS 核心完全无关**。VFS 核心、rawflash、各 FS 后端都只认
 * 这里定义的 @ref vfs_storage_t，不关心底层是内部 flash 还是 SD 卡。
 *
 * @section layering 分层职责
 * @verbatim
 *   介质驱动 storage_drv_*   ← 本文件的 vfs_storage_t（read/prog/erase）
 *         │
 *         ▼
 *   分区切片 vfs_bdev_t       ← 某介质内 [offset, offset+len) 的一段
 *         │
 *         ▼
 *   FS 后端 / rawflash 核心
 * @endverbatim
 *
 * 具体驱动（FAL flash / 裸 flash / SD 卡）**不在核心内**实现：由装配/驱动层
 * （src/core/vfs/storage/ 下的 storage_drv_fal.* 等）各自提供 vfs_storage_t 实例。
 * 核心（vfs_core/vfs_bdev/rawflash/backends）只带本接口头，不依赖任何具体介质
 * 或框架（如 FAL）；FAL 引用被限制在 storage/ 子目录内。
 */

#ifndef VFS_STORAGE_DRV_H
#define VFS_STORAGE_DRV_H

#include <stdint.h>
#include <stddef.h>
#include "vfs_config.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief 物理存储设备（整块 flash / 整张 SD 卡）
     *
     * 代表整块存储介质，提供按字节偏移的 read/prog/erase。
     * 具体介质驱动（FAL flash / 裸 flash / SD 卡）各自实现三个回调函数。
     */
    typedef struct vfs_storage
    {
        const char *name;      /**< 设备名（诊断用） */
        uint32_t blk_size;     /**< 擦除/编程粒度（flash 4K，SD 512） */
        uint32_t blk_count;    /**< 整设备块数 */
        /** 从 byte_off 起读 n 字节（byte_off 为设备内相对偏移，非绝对地址） */
        int (*read)(struct vfs_storage *s, uint32_t byte_off, void *buf, size_t n);
        /** 从 byte_off 起编程（写）n 字节（byte_off 为设备内相对偏移） */
        int (*prog)(struct vfs_storage *s, uint32_t byte_off, const void *buf, size_t n);
        /** 从 byte_off 起擦除 n 字节（n 为块大小整数倍，byte_off 为设备内相对偏移） */
        int (*erase)(struct vfs_storage *s, uint32_t byte_off, size_t n);
        void *ctx;             /**< 驱动私有上下文（fal_flash_dev* / sd_ctx* 等） */
    } vfs_storage_t;

#ifdef __cplusplus
}
#endif

#endif /* VFS_STORAGE_DRV_H */
