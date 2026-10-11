#ifndef VFS_TEST_MOCK_STORAGE_H
#define VFS_TEST_MOCK_STORAGE_H

/**
 * @file mock_storage.h
 * @brief PC 单测用的"内存 flash"介质 —— 行为对齐真实 NOR flash
 *
 * 提供：整介质（vfs_storage_t）+ 其上切出的一分区切片（vfs_bdev_t）。
 * prog 语义严格模拟 flash「只能把 1 置 0（按位与）」——若目标非 0xFF，
 * 写入会被静默位与，用于验证 rawflash 的「先擦后写 / 0xFF 校验」纪律。
 */

#include <stdint.h>
#include <stddef.h>
#include "vfs_bdev.h"

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct vfs_mock_flash
    {
        vfs_storage_t storage;    /**< 整介质驱动 */
        vfs_bdev_t bdev;          /**< 切出的分区切片 */
        uint8_t *mem;             /**< 介质字节缓冲（长度 = total） */
        uint32_t total;           /**< 介质总字节数 */
        uint32_t read_calls;      /**< read 调用计数 */
        uint32_t prog_calls;      /**< prog 调用计数 */
        uint32_t erase_calls;     /**< erase 调用计数 */
    } vfs_mock_flash_t;

    /**
     * @brief 初始化内存介质（全部置 0xFF，即擦除态）
     * @param m         待初始化对象
     * @param blk_size  块大小（字节）
     * @param blk_count 块数
     * @return 0 成功；< 0 分配失败
     */
    int vfs_mock_flash_init(vfs_mock_flash_t *m, uint32_t blk_size, uint32_t blk_count);

    /** @brief 释放介质缓冲 */
    void vfs_mock_flash_free(vfs_mock_flash_t *m);

    /**
     * @brief 从介质切出一段分区作为 bdev
     * @param m           已初始化的介质
     * @param name        分区名
     * @param byte_offset 介质内起始字节偏移
     * @param blk_count   分区块数
     */
    void vfs_mock_make_bdev(vfs_mock_flash_t *m, const char *name,
                            uint32_t byte_offset, uint32_t blk_count);

#ifdef __cplusplus
}
#endif
#endif /* VFS_TEST_MOCK_STORAGE_H */
