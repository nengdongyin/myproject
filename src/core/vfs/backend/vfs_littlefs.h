/**
 * @file vfs_littlefs.h
 * @brief littlefs 文件系统后端 —— 基于原生 lfs_* API + vfs_bdev_t
 *
 * 直接调用 littlefs 原生 API（RTOS 无关），块设备来自 vfs_bdev_t（分区切片，
 * 介质可为内部 flash 或 SD 等）。每个挂载点持有一个独立的 lfs_t（多分区 = 多 lfs_t）。
 *
 * @section threadsafe 线程安全
 * 本后端不启用 littlefs 自带线程安全。并发访问由 VFS 核心的"每挂载点锁"
 * （mp->lock，见需求6）统一串行化。若 littlefs 构建开启了 LFS_THREADSAFE，
 * 请在 core 侧关闭以避免双重加锁。
 *
 * @section mem 内存
 * lfs_t / 每文件 lfs_file_t / 每目录 lfs_dir_t 均取自静态池（零堆）；
 * 每文件 cache 由本后端的静态缓冲提供（lfs_file_opencfg）。
 */

#ifndef VFS_LITTLEFS_H
#define VFS_LITTLEFS_H

#include "vfs.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* ---- littlefs 几何参数（可用 -D 覆盖） ---- */
/** @brief littlefs 最小读粒度（字节） */
#ifndef VFS_LFS_READ_SIZE
#define VFS_LFS_READ_SIZE     16
#endif
/** @brief littlefs 编程页大小（须 ≤ 块大小且为 read_size 整数倍） */
#ifndef VFS_LFS_PROG_SIZE
#define VFS_LFS_PROG_SIZE     256
#endif
/** @brief littlefs 全局 cache 大小（须 ≥ prog_size） */
#ifndef VFS_LFS_CACHE_SIZE
#define VFS_LFS_CACHE_SIZE    512
#endif
/** @brief littlefs lookahead 缓冲字节数（须为 8 的倍数） */
#ifndef VFS_LFS_LOOKAHEAD
#define VFS_LFS_LOOKAHEAD     32
#endif
/** @brief littlefs 块擦除磨损均衡周期 */
#ifndef VFS_LFS_BLOCK_CYCLES
#define VFS_LFS_BLOCK_CYCLES  512
#endif
/** @brief 每文件 cache 缓冲（须 ≥ prog_size） */
#ifndef VFS_LFS_FILE_CACHE
#define VFS_LFS_FILE_CACHE    VFS_LFS_PROG_SIZE
#endif

    /** @brief littlefs 后端虚函数表 */
    extern const vfs_fs_ops_t vfs_littlefs_ops;

    /**
     * @brief 格式化某挂载点对应的 littlefs 分区（破坏性）
     *
     * 在 mount 失败（未格式化/损坏）时可按需调用，之后重新 mount。
     * @param mp 已解析出 storage_dev 的挂载点（不能为 NULL）
     * @return 0 成功，< 0 负 errno
     */
    int vfs_littlefs_format(vfs_mount_t *mp);

#ifdef __cplusplus
}
#endif

#endif /* VFS_LITTLEFS_H */
