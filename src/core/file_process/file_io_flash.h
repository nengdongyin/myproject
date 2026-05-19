/**
 * @file file_io_flash.h
 * @brief Flash 直接 I/O 后端接口声明
 *
 * 基于 FAL（Flash Abstraction Layer）的 Flash 直接 I/O 后端实现，
 * 不依赖文件系统，直接对 Flash 分区进行读写操作。
 * 通过工厂模式为每个 file_processing_t 实例创建独立的 file_io_t 实例，
 * 支持多文件并发操作。
 *
 * @section features 功能特性
 * - 直接读写 Flash 分区，无需文件系统开销
 * - 以 "分区名/文件名" 格式的路径寻址 FAL 分区
 * - 写入前自动擦除目标分区
 * - 分区末尾 64 字节 footer 存储 magic/version/data_len 元数据
 * - 通过 footer 在读取时还原实际数据长度
 * - 工厂模式：每个实例独立，可并发操作不同分区
 *
 * @section footer_format Footer 元数据格式
 * 每个 Flash 分区末尾 64 字节（FLASH_FOOTER_SIZE）保留为 footer 区域：
 * @code
 *   Offset   Size   Field
 *   0        4      magic    (0x46544F4F = "FOOT")
 *   4        4      version  (1)
 *   8        4      data_len (实际写入数据的总字节数)
 *   12       52     reserved (全为零)
 * @endcode
 *
 * @section usage 使用步骤
 * @code
 *   1. 创建实例: file_io_t *io = file_io_flash_create();
 *   2. 传入框架: file_processing_t *fp = fp_create(io, "cfg_partition");
 *      （fp_create 接管 io 的所有权，销毁时自动释放）
 *   3. 打开写入: fp_open_for_write(fp, "settings.bin");
 *      → 内部解析路径: "cfg_partition" → fal_partition_find()
 *      → 擦除分区 → 开始写入
 *   4. 关闭文件: fp_close(fp);
 *      → 写入 footer (data_len = 实际写入字节数)
 * @endcode
 */

#ifndef FILE_IO_FLASH_H
#define FILE_IO_FLASH_H

#include "file_io_policy.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief 创建 Flash I/O 后端实例
     *
     * 动态分配 file_io_t 实例，ops 绑定到 FAL 实现。
     * 每个实例的 ctx 独立，支持并发操作不同分区。
     *
     * @return 成功返回 file_io_t 指针，内存不足返回 NULL
     */
    file_io_t *file_io_flash_create(void);

    /**
     * @brief 销毁 Flash I/O 后端实例
     *
     * 释放 file_io_t 所占内存。调用前需确保该实例未被任何
     * file_processing_t 持有（通常由 fp_destroy 代劳）。
     *
     * @param io 由 file_io_flash_create() 创建的实例，可为 NULL
     */
    void file_io_flash_destroy(file_io_t *io);

#ifdef __cplusplus
}
#endif

#endif /* FILE_IO_FLASH_H */
