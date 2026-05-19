/**
 * @file file_io_fs.h
 * @brief 文件系统 I/O 后端接口声明
 *
 * 基于 Zephyr RTOS 文件系统 API 的 I/O 后端实现。
 * 通过工厂模式为每个 file_processing_t 实例创建独立的 file_io_t 实例，
 * 支持多文件并发操作。
 *
 * @section features 功能特性
 * - 支持最多 2 个并发打开文件（FS_MAX_OPEN = 2）
 * - 读/写/关闭及文件删除操作
 * - 关闭文件时可删除临时文件（abort 语义）
 * - 线程安全：fd 分配/释放使用 LOCK/UNLOCK 保护
 * - 工厂模式：每个实例独立，可并发操作不同文件
 *
 * @section usage 使用步骤
 * @code
 *   1. 初始化: file_io_fs_init();
 *   2. 创建实例: file_io_t *io = file_io_fs_create();
 *   3. 传入框架: file_processing_t *fp = fp_create(io, "/data");
 *      （fp_create 接管 io 的所有权，销毁时自动释放）
 * @endcode
 */

#ifndef FILE_IO_FS_H
#define FILE_IO_FS_H

#include "file_io_policy.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief 创建文件系统 I/O 后端实例
     *
     * 动态分配 file_io_t 实例，ops 绑定到 Zephyr FS 实现。
     * 每个实例的 ctx 独立，支持并发操作不同文件。
     *
     * @return 成功返回 file_io_t 指针，内存不足返回 NULL
     */
    file_io_t *file_io_fs_create(void);

    /**
     * @brief 销毁文件系统 I/O 后端实例
     *
     * 释放 file_io_t 所占内存。调用前需确保该实例未被任何
     * file_processing_t 持有（通常由 fp_destroy 代劳）。
     *
     * @param io 由 file_io_fs_create() 创建的实例，可为 NULL
     */
    void file_io_fs_destroy(file_io_t *io);

    /**
     * @brief 初始化文件系统 I/O 后端
     *
     * 将全局文件描述符池 g_fs_io 清零。必须在首次使用后端之前调用，
     * 重复调用是安全的（每次都会清零）。
     *
     * @note 此函数不会初始化 Zephyr FS 子系统本身（需在应用层通过 fs_mount 等函数完成）
     */
    void file_io_fs_init(void);

#ifdef __cplusplus
}
#endif

#endif /* FILE_IO_FS_H */
