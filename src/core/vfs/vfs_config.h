/**
 * @file vfs_config.h
 * @brief VFS 编译期配置 —— 容量上限与特性开关
 *
 * 所有上限均为编译期常量，配合静态池实现"零堆"分配。
 * 每个宏都可用编译参数（-D）在具体工程中覆盖，无需改本文件。
 *
 * 约定：默认值面向"轻量嵌入式"，非必要特性一律默认关闭以省 ROM/RAM。
 */

#ifndef VFS_CONFIG_H
#define VFS_CONFIG_H

/* ================================================================
 *  容量上限（静态池尺寸）
 * ================================================================ */

/** @brief 最大挂载点数（每挂载点占一个 vfs_mount_t + 一把锁） */
#ifndef VFS_MAX_MOUNTS
#define VFS_MAX_MOUNTS        8
#endif

/** @brief 最大文件系统类型注册槽 */
#ifndef VFS_MAX_TYPES
#define VFS_MAX_TYPES         6
#endif

/** @brief 最大同时打开文件数（文件句柄池） */
#ifndef VFS_MAX_OPEN_FILES
#define VFS_MAX_OPEN_FILES    6
#endif

/** @brief 最大同时打开目录数（目录句柄池） */
#ifndef VFS_MAX_OPEN_DIRS
#define VFS_MAX_OPEN_DIRS     2
#endif

/** @brief 最大块设备（分区切片）注册数 */
#ifndef VFS_MAX_BDEVS
#define VFS_MAX_BDEVS         8
#endif

/** @brief 文件名最大长度（不含 '\\0'） */
#ifndef VFS_NAME_MAX
#define VFS_NAME_MAX          63
#endif

/** @brief 路径/挂载点最大长度（含 '\\0'） */
#ifndef VFS_PATH_MAX
#define VFS_PATH_MAX          128
#endif

/* ================================================================
 *  特性开关（编译期裁剪）
 * ================================================================ */

/** @brief 目录操作（opendir/readdir/closedir）。嵌入式少用，但保留 */
#ifndef VFS_ENABLE_DIR
#define VFS_ENABLE_DIR            1
#endif

/** @brief 动态挂载（vfs_mount/vfs_unmount）。默认关，静态表为主 */
#ifndef VFS_ENABLE_DYNAMIC_MOUNT
#define VFS_ENABLE_DYNAMIC_MOUNT  0
#endif

/** @brief rename 操作 */
#ifndef VFS_ENABLE_RENAME
#define VFS_ENABLE_RENAME         0
#endif

/** @brief statvfs（卷容量查询） */
#ifndef VFS_ENABLE_STATVFS
#define VFS_ENABLE_STATVFS        0
#endif

/** @brief 同文件独占锁（跨任务写冲突检测） */
#ifndef VFS_ENABLE_FILE_LOCK
#define VFS_ENABLE_FILE_LOCK      0
#endif

/** @brief stdio 适配层（_open_r / _sys_open 等） */
#ifndef VFS_ENABLE_STDIO
#define VFS_ENABLE_STDIO          0
#endif

/** @brief 多任务保护：每挂载点互斥锁 */
#ifndef VFS_ENABLE_MP_LOCK
#define VFS_ENABLE_MP_LOCK        1
#endif

/** @brief FAL 块设备适配器（裸 flash 分区 → vfs_bdev） */
#ifndef VFS_ENABLE_FAL_BDEV
#define VFS_ENABLE_FAL_BDEV       1
#endif

/**
 * @brief FatFs 后端（默认关：需工程先引入 FatFs 库 + SD 块设备）
 *
 * 置 1 后把 vfs_fatfs.c 纳入构建；FatFs 缺失时保持 0。
 */
#ifndef VFS_ENABLE_FATFS
#define VFS_ENABLE_FATFS          0
#endif

#endif /* VFS_CONFIG_H */
