/**
 * @file rawflash.h
 * @brief 通用块区存储核心 —— 掉电安全的静态块区读写（可独立于 VFS 使用）
 *
 * 本模块把"一个分区切片（@ref vfs_bdev_t）+ 一段数据容量"当作一个掉电安全的
 * 顺序读写块区：数据区顺序写入，末尾擦除块存 footer（长度 + CRC，双份冗余），
 * **写意图以 close 落 footer 为提交点**，故数据写一半掉电时 footer 无效、整区
 * 视为空（不读到半截脏数据）。
 *
 * @section standalone 独立性
 * 本核心只依赖"介质/分区切片层"（vfs_bdev_t 及其块/字节访问），**不依赖 VFS
 * 核心**（无 vfs_file_t / vfs_mount_t / 句柄池 / 挂载点）。因此它既能被 VFS 的
 * rawflash 后端薄适配，也可脱离 VFS 直接使用（如存配置/日志 blob）。
 *
 * @section layout 布局
 * @verbatim
 *   数据区 [0, cap)                元数据块（分区最后一块）
 *   write 顺序落此处，cursor 递增    footer 主份 + 冗余份（各带 CRC）
 * @endverbatim
 * 元数据块 = bdev->blk_count-1；数据容量 cap 由调用方给定（≤ 数据区容量）。
 *
 * @section model 使用模型
 * @code
 *   rawflash_ctx_t ctx;
 *   rawflash_ctx_init(&ctx, bdev, cap);
 *   if (rawflash_open(&ctx, RAWFLASH_MODE_WRITE, 0, 0) == 0) {
 *       rawflash_write(&ctx, buf, len);
 *       rawflash_close(&ctx);   // 此步才提交 footer
 *   }
 * @endcode
 */

#ifndef RAWFLASH_H
#define RAWFLASH_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h> /* off_t / ssize_t */
#include "vfs_bdev.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** @brief 会话模式：只读 */
#define RAWFLASH_MODE_READ  0
/** @brief 会话模式：写（打开即整区擦除，从 0 顺序写） */
#define RAWFLASH_MODE_WRITE 1
/** @brief 会话模式：读-改-写（打开不擦，首次 write 才擦） */
#define RAWFLASH_MODE_RDWR  2

/** @brief 定位原点：文件头 */
#define RAWFLASH_SEEK_SET 0
/** @brief 定位原点：当前偏移 */
#define RAWFLASH_SEEK_CUR 1
/** @brief 定位原点：文件尾 */
#define RAWFLASH_SEEK_END 2

    /**
     * @brief rawflash 会话上下文（调用方拥有，零堆）
     *
     * 与 VFS 无关的纯状态：绑定分区、数据容量、写/读游标、可见长度、模式。
     */
    typedef struct rawflash_ctx
    {
        const vfs_bdev_t *bdev; /**< 绑定的分区切片 */
        uint32_t meta_blk;      /**< 元数据块号（= blk_count-1） */
        uint32_t data_cap;      /**< 数据容量上界（字节） */
        uint32_t cursor;        /**< 写游标 / 读位置 */
        uint32_t rdlen;         /**< 读模式下的有效长度（来自 footer） */
        uint8_t mode;           /**< RAWFLASH_MODE_* */
        uint8_t append;         /**< 1=追加会话（close 需先擦 meta 再写 footer） */
    } rawflash_ctx_t;

    /** @brief footer 结构体字节大小（供装配层做块大小合法性校验） */
    size_t rawflash_footer_size(void);

    /**
     * @brief 初始化会话上下文（绑定分区与数据容量）
     * @param ctx      待初始化上下文（不能为 NULL）
     * @param bdev     绑定的分区切片（不能为 NULL）
     * @param data_cap 数据容量上界（字节，≤ 数据区容量）
     */
    void rawflash_ctx_init(rawflash_ctx_t *ctx, const vfs_bdev_t *bdev, uint32_t data_cap);

    /**
     * @brief 读取"可见文件长度"（footer 有效长度，夹取到 data_cap）
     * @param bdev     分区切片
     * @param data_cap 容量上界
     * @return 可见长度（字节）；footer 无效时返回 0
     */
    uint32_t rawflash_visible_len(const vfs_bdev_t *bdev, uint32_t data_cap);

    /**
     * @brief 擦除整个分区（含元数据块）—— 即"删除"该块区
     * @param bdev 分区切片
     * @return 0 成功；< 0 负 errno
     */
    int rawflash_erase(const vfs_bdev_t *bdev);

    /**
     * @brief 打开/重用会话
     * @details 按 mode 与 append/truncate 决定擦除与装载策略：
     *  - append：读 footer 得长度 L → 校验不可擦段全 0xFF（否则 -EIO）→ 擦其后
     *    整块 → 续写，close 提交。
     *  - mode=WRITE 或 truncate：整区擦除，从 0 写（WRITE 模式）。
     *  - mode=RDWR 且非 truncate：不擦除，按只读装载旧内容，首次 write 才擦。
     *  - mode=READ：只读 footer，绝不擦/写。
     * @param ctx      上下文（已 ctx_init）
     * @param mode     RAWFLASH_MODE_*
     * @param append   1=尾部续写
     * @param truncate 1=打开即截断（整区擦除后写）
     * @return 0 成功；< 0 负 errno（-EIO 脏尾 / 底层错误）
     */
    int rawflash_open(rawflash_ctx_t *ctx, int mode, int append, int truncate);

    /**
     * @brief 从当前游标读取（不越过有效长度）
     * @return >= 0 读取字节数（0=EOF）；< 0 负 errno
     */
    ssize_t rawflash_read(rawflash_ctx_t *ctx, void *buf, size_t n);

    /**
     * @brief 从当前写游标顺序写入
     * @return >= 0 写入字节数；< 0 负 errno（-ENOSPC 超容量 / -EBADF 非写模式）
     */
    ssize_t rawflash_write(rawflash_ctx_t *ctx, const void *buf, size_t n);

    /**
     * @brief 只读/RDWR 模式下移动读游标
     * @return >= 0 新绝对偏移；< 0 负 errno（-ENOTSUP 写模式 / -EINVAL 越界）
     */
    off_t rawflash_lseek(rawflash_ctx_t *ctx, off_t off, int whence);

    /** @brief 返回当前游标位置（句柄无效返回 -EBADF） */
    off_t rawflash_tell(const rawflash_ctx_t *ctx);

    /** @brief 刷缓存：直接写 flash，无缓存，恒返回 0 */
    int rawflash_sync(rawflash_ctx_t *ctx);

    /**
     * @brief 截断到 0（= 擦除整区并进入写模式）
     * @return 0 成功；< 0 负 errno
     */
    int rawflash_truncate(rawflash_ctx_t *ctx);

    /**
     * @brief 关闭会话：写模式下提交 footer（append 先擦 meta 块）
     * @return 0 成功；写模式 footer 写入失败返回负 errno
     */
    int rawflash_close(rawflash_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* RAWFLASH_H */
