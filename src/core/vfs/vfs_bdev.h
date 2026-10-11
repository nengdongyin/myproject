/**
 * @file vfs_bdev.h
 * @brief 块设备抽象 —— 物理存储设备与分区切片
 *
 * 采用"两层"模型：整设备（@ref vfs_storage_t，见 storage_drv.h）+ 分区切片：
 *
 * @verbatim
 *   vfs_storage_t  物理存储设备（整块 flash / 整张 SD 卡）
 *         │  按字节偏移 read/prog/erase（由 storage_drv_* 提供）
 *         ▼
 *   vfs_bdev_t     分区切片（storage 内 offset + 长度），文件系统挂在其上
 * @endverbatim
 *
 * 本文件只做"切片 + 块/字节访问转发"，不实现任何具体介质：介质的 read/prog/
 * erase 由 @ref vfs_storage_t 提供，其实现位于装配/驱动层（如 storage_drv_fal.c），
 * 核心不依赖 FAL 等任何具体框架。
 *
 * 该层是"多存储器"（需求 7）的关键：littlefs/fatfs 只认 vfs_bdev_t，
 * 不关心底层是 flash 还是 SD。
 */

#ifndef VFS_BDEV_H
#define VFS_BDEV_H

#include <stdint.h>
#include <stddef.h>
#include "vfs_config.h"
#include "storage_drv.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief 分区切片
     *
     * 是某个 vfs_storage_t 内的一段区域，由 offset + 长度界定。
     * 文件系统后端（littlefs/fatfs）的块设备即此结构。
     *
     * @note offset/blk_count 为 uint32_t：单分区容量上限 4GB（>4GB 的介质
     *       需将分区拆分为多个切片分别挂载）。
     */
    typedef struct vfs_bdev
    {
        const char *name;            /**< 分区名（如 "flash_app"） */
        const vfs_storage_t *storage;/**< 所属物理设备 */
        uint32_t offset;             /**< 起始位置：storage 内字节偏移 */
        uint32_t blk_size;           /**< 块大小（通常 = storage->blk_size） */
        uint32_t blk_count;          /**< 分区块数（大小 = blk_count × blk_size） */
        const void *ctx;             /**< 可选驱动私有（如分区描述符指针） */
    } vfs_bdev_t;

    /**
     * @brief 初始化一个分区切片
     *
     * @param d          待初始化的 bdev
     * @param name       分区名
     * @param s          所属物理设备
     * @param byte_offset 在 storage 内的起始字节偏移
     * @param blk_size   块大小
     * @param blk_count  块数
     */
    void vfs_bdev_init(vfs_bdev_t *d, const char *name, const vfs_storage_t *s,
                       uint32_t byte_offset, uint32_t blk_size, uint32_t blk_count);

    /**
     * @brief 分区总字节数
     * @param d 分区切片（不能为 NULL）
     * @return 总字节数 = blk_count × blk_size
     */
    uint32_t vfs_bdev_size(const vfs_bdev_t *d);

    /**
     * @brief 按"分区内块号"读取 nblk 个块
     * @param d    分区切片
     * @param blk  分区内起始块号（0 = 分区起点）
     * @param buf  输出缓冲区（不能为 NULL）
     * @param nblk 读取块数
     * @return 0 成功，-EINVAL 越界/参数错，-EIO 硬件错误
     */
    int vfs_bdev_read(const vfs_bdev_t *d, uint32_t blk, void *buf, uint32_t nblk);

    /**
     * @brief 按块号编程（写）nblk 个块
     * @param d    分区切片
     * @param blk  分区内起始块号（0 = 分区起点）
     * @param buf  数据缓冲区（不能为 NULL）
     * @param nblk 写入块数
     * @return 0 成功，-EINVAL 越界/参数错，-EIO 硬件错误
     */
    int vfs_bdev_prog(const vfs_bdev_t *d, uint32_t blk, const void *buf, uint32_t nblk);

    /**
     * @brief 按块号擦除 nblk 个块
     * @param d    分区切片
     * @param blk  分区内起始块号（0 = 分区起点）
     * @param nblk 擦除块数
     * @return 0 成功，-EINVAL 越界/参数错，-EIO 硬件错误
     */
    int vfs_bdev_erase(const vfs_bdev_t *d, uint32_t blk, uint32_t nblk);

    /**
     * @brief 按字节偏移读取 n 字节（不要求块对齐，用于元数据/裸 flash 流式读写）
     * @param d        分区切片
     * @param byte_off 分区内字节偏移（0 = 分区起点）
     * @param buf      输出缓冲区（不能为 NULL）
     * @param n        读取字节数
     * @return 0 成功，-EINVAL 越界/参数错，-EIO 硬件错误
     */
    int vfs_bdev_read_bytes(const vfs_bdev_t *d, uint32_t byte_off, void *buf, size_t n);

    /**
     * @brief 按字节偏移编程 n 字节（不要求块对齐）
     * @param d        分区切片
     * @param byte_off 分区内字节偏移（0 = 分区起点）
     * @param buf      数据缓冲区（不能为 NULL）
     * @param n        写入字节数
     * @return 0 成功，-EINVAL 越界/参数错，-EIO 硬件错误
     */
    int vfs_bdev_prog_bytes(const vfs_bdev_t *d, uint32_t byte_off, const void *buf, size_t n);

    /**
     * @brief 按名字查找已注册的块设备切片
     * @param name 分区名（vfs_bdev_register / vfs_bdev_register_table 时用的名字）
     * @return 命中返回切片指针，未注册返回 NULL
     */
    const vfs_bdev_t *vfs_bdev_find(const char *name);

    /**
     * @brief 分区切片配置（表驱动注册用）
     *
     * 描述"如何从某介质切出一段分区"，供 vfs_bdev_register_table 批量注册。
     * 相比逐个声明静态 vfs_bdev_t + 手写注册调用，加分区只需在配置表加一行。
     */
    typedef struct vfs_bdev_cfg
    {
        const char *name;             /**< 分区名（全局唯一，挂载表按此名绑定） */
        const vfs_storage_t *storage; /**< 所属物理设备（介质驱动产出） */
        uint32_t offset;              /**< 介质内起始字节偏移（须块对齐） */
        uint32_t blk_size;            /**< 块大小（通常 = storage->blk_size） */
        uint32_t blk_count;           /**< 分区块数 */
    } vfs_bdev_cfg_t;

    /**
     * @brief 表驱动批量注册块设备（核心内部持静态 bdev 池）
     * @details 逐条把配置写入核心持有的静态 vfs_bdev_t 池并登记（vfs_bdev_register
     *          存的是指针，故切片须具静态生命周期，由核心池保证）。部分失败不致命：
     *          继续处理其余条目，返回最后一个错误码。
     * @param tbl 配置数组（不能为 NULL）
     * @param n   条目数
     * @return 0 全部成功；否则最后一个失败的错误码（-EINVAL/-ENOSPC/-EEXIST）
     */
    int vfs_bdev_register_table(const vfs_bdev_cfg_t *tbl, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* VFS_BDEV_H */
