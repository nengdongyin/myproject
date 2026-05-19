/**
 * @file file_io_lfs.h
 * @brief LittleFS 直接 I/O 后端接口声明
 *
 * 基于 LittleFS 原生 API 的 I/O 后端实现，绕过 Zephyr 文件系统抽象层，
 * 直接调用 lfs_* 函数，减少调用开销。
 * 通过工厂模式为每个 file_processing_t 实例创建独立的 file_io_t 实例。
 *
 * @section features 功能特性
 * - 直接调用 LittleFS 原生 API，无 Zephyr FS 抽象层开销
 * - 每个 file_io_t 实例持有独立的小内存句柄（lfs_file_t 动态分配）
 * - 读/写/关闭及文件删除操作
 * - abort 时自动删除临时文件
 * - 工厂模式：每个实例独立，可并发操作不同文件
 *
 * @section usage 使用步骤
 * @code
 *   1. 创建实例: file_io_t *io = file_io_lfs_create(&my_lfs);
 *   2. 传入框架: file_processing_t *fp = fp_create(io, "/lfs");
 *      （fp_create 接管 io 的所有权，销毁时自动释放）
 * @endcode
 *
 * @note lfs_t 由调用方负责挂载/格式化，本模块仅持有指针不做生命周期管理
 */

#ifndef FILE_IO_LFS_H
#define FILE_IO_LFS_H

#include "file_io_policy.h"
#include <lfs.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief 创建 LittleFS I/O 后端实例
     *
     * @param lfs 指向已格式化/挂载的 LittleFS 文件系统实例指针（不可为 NULL）
     * @return 成功返回 file_io_t 指针，内存不足返回 NULL
     */
    file_io_t *file_io_lfs_create(lfs_t *lfs);

    /**
     * @brief 销毁 LittleFS I/O 后端实例
     *
     * 释放 file_io_t 所占内存。调用前需确保该实例未被任何
     * file_processing_t 持有（通常由 fp_destroy 代劳）。
     *
     * @param io 由 file_io_lfs_create() 创建的实例，可为 NULL
     */
    void file_io_lfs_destroy(file_io_t *io);

#ifdef __cplusplus
}
#endif

#endif /* FILE_IO_LFS_H */
