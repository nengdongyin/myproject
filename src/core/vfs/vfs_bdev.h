/**
 * @file vfs_bdev.h
 * @brief 块设备抽象 —— 物理存储设备与分区切片
 *
 * 采用"两层"模型，对齐 FAL 的 fal_flash_dev（整设备）+ fal_partition（分区）：
 *
 * @verbatim
 *   vfs_storage_t  物理存储设备（整块 flash / 整张 SD 卡）
 *         │  按字节偏移 read/prog/erase
 *         ▼
 *   vfs_bdev_t     分区切片（storage 内 offset + 长度），文件系统挂在其上
 * @endverbatim
 *
 * 分区偏移（offset）来源：
 *  - 内部 flash：直接复用 FAL 分区表（见 vfs_bdev_fal_init）
 *  - SD 卡：解析 MBR 分区表得到 LBA 起始 + 扇区数
 *
 * 该层是"多存储器"（需求 7）的关键：littlefs/fatfs 只认 vfs_bdev_t，
 * 不关心底层是 flash 还是 SD。
 */

#ifndef VFS_BDEV_H
#define VFS_BDEV_H

#include <stdint.h>
#include <stddef.h>
#include "vfs_config.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief 物理存储设备（≈ fal_flash_dev）
     *
     * 代表整块存储介质，提供按字节偏移的 read/prog/erase。
     * 具体后端（FAL flash / SD 卡）各自实现三个回调函数。
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
        void *ctx;             /**< 后端私有上下文（fal_flash_dev* / sd_ctx*） */
    } vfs_storage_t;

    /**
     * @brief 分区切片（≈ fal_partition）
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
        const void *ctx;             /**< 可选后端私有（如 FAL 分区指针） */
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

#if VFS_ENABLE_FAL_BDEV
    /**
     * @brief 从 FAL 分区表构造 bdev（复用分区表的 offset/len）
     *
     * @param d        待填充的 bdev
     * @param name     该 bdev 在 VFS 中的名字
     * @param fal_part 对应的 FAL 分区名
     * @return 0 成功，-ENOENT 分区/设备不存在，-ENOSPC 内部 storage 池满
     */
    int vfs_bdev_fal_init(vfs_bdev_t *d, const char *name, const char *fal_part);

    /**
     * @brief 从 FAL flash 设备 + 自定义 offset/size 构造 bdev
     *
     * 用于"分区几何定义在 storage_config.c"的场景（需求7），无需把分区
     * 写进 FAL 分区表。
     *
     * @param fal_flash FAL flash 设备名（如 "rp2350_flash"）
     * @param offset    设备内字节偏移（须块对齐）
     * @param size      区域大小（须块对齐）
     * @return 0 成功，-EINVAL 未对齐/越界，-ENOENT 设备不存在，-ENOSPC 池满
     */
    int vfs_bdev_fal_flash_init(vfs_bdev_t *d, const char *name,
                                const char *fal_flash, uint32_t offset, uint32_t size);
#endif

#ifdef __cplusplus
}
#endif

#endif /* VFS_BDEV_H */
