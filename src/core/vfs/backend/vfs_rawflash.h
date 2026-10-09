/**
 * @file vfs_rawflash.h
 * @brief 裸 flash 文件系统后端 —— 基于 FAL 分区的静态注册伪 FS
 *
 * 面向"无文件系统的裸 flash"：每个"文件"就是一个 FAL 分区的整块区域，
 * 文件名与容量在编译期**静态注册**。打开未注册的文件名直接失败（-ENOENT）。
 *
 * @section layout 分区布局
 * @verbatim
 *   ┌──────────────────────────────┬───────────────────────┐
 *   │        数据区 [0, L-B)         │   元数据块 [L-B, L)     │
 *   │  write 顺序落此处，cursor 递增   │  footer 主份 + 冗余份    │
 *   └──────────────────────────────┴───────────────────────┘
 *   B = 一个擦除块（blk_size），元数据独占最后一块
 * @endverbatim
 *
 * @section semantics 语义
 *  - open(只读)：绝不擦/写，纯读；数据长度来自 footer。
 *  - open(O_WRONLY / 任一 O_TRUNC)：打开即擦【整个分区】，从 0 流式写，
 *    close 落 footer。同一 open 会话内可多次 write（游标续写）。
 *  - open(O_RDWR，无 O_TRUNC)：读-改-写会话——打开**不擦除**，先按只读装载
 *    （可读旧内容）；**首次 write** 时才擦除整区并转流式写（从 0 整区重写），
 *    此后本会话只写（read 返回 -EBADF）。写总是整区重写，不支持就地部分覆盖；
 *    若打开后从不 write，close 不改动分区。
 *  - open(O_APPEND)（需 VFS_RAWFLASH_ENABLE_APPEND=1）：尾部续写——读 footer
 *    得有效长度 L，校验 L 所在块尾部是否全 0xFF（脏尾=上次掉电残留 → -EIO），
 *    再擦除其后整块并顺序追加，close 落 footer。同一文件写-写并发返回 -EBUSY。
 *  - 不支持 rename、mkdir；不支持写模式 lseek。
 *  - 掉电：数据写一半时 footer 仍为 0xFF → 视为空文件（不读到半截脏数据）。
 *
 * @section usage 使用示例
 * @code
 *   static vfs_bdev_t s_bdev;
 *   vfs_bdev_fal_init(&s_bdev, "flash_log", "log_part");
 *
 *   static const vfs_rawflash_entry_t s_table[] = {
 *       { "boot_log", &s_bdev, 32*1024, 0 },
 *   };
 *   static const vfs_rawflash_vol_t s_vol = { s_table, 1 };
 *
 *   vfs_register(VFS_FSTYPE_RAWFLASH, &vfs_rawflash_ops);
 *   vfs_mount_cfg_t cfg = { "/raw", VFS_FSTYPE_RAWFLASH, NULL, 0, &s_vol };
 * @endcode
 */

#ifndef VFS_RAWFLASH_H
#define VFS_RAWFLASH_H

#include "vfs.h"

/**
 * @brief 追加写支持（尾部续写）。默认关闭，行为与旧版一致。
 *
 * 置 1 后 open(O_WRONLY|O_APPEND) 走尾部续写：读 footer 得有效长度 L，
 * 校验 L 所在块尾部（不可擦段）是否全 0xFF——发现非 0xFF（上次掉电残留）
 * 返回 -EIO；随后擦除其后整块（可擦段）并顺序追加，close 提交新 footer。
 * 同一文件写-写并发由后端自锁拒绝（-EBUSY）。关闭时 core 把 O_APPEND 拦为
 * -ENOTSUP。
 */
#ifndef VFS_RAWFLASH_ENABLE_APPEND
#define VFS_RAWFLASH_ENABLE_APPEND 0
#endif

#ifdef __cplusplus
extern "C"
{
#endif

/** @brief 条目标志：只读 */
#define VFS_RAWRO (1u << 0)

    /** @brief 一个静态注册的"文件"（= 一个 FAL 分区） */
    typedef struct vfs_rawflash_entry
    {
        const char *name;       /**< 逻辑文件名（挂在挂载点下的 rel） */
        const vfs_bdev_t *bdev; /**< 对应分区切片 */
        uint32_t size;          /**< 预留最大数据长度（字节），≤ 数据区容量 */
        uint8_t flags;          /**< VFS_RAWRO 等 */
    } vfs_rawflash_entry_t;

    /** @brief 一个 rawflash 卷（挂载点 fs_data 指向此结构） */
    typedef struct vfs_rawflash_vol
    {
        const vfs_rawflash_entry_t *entries; /**< 静态注册表数组（编译期常量） */
        size_t count;                        /**< 注册条目个数 */
    } vfs_rawflash_vol_t;

    /** @brief 裸 flash 后端虚函数表（直接在 storage_config.c 引用） */
    extern const vfs_fs_ops_t vfs_rawflash_ops;

#ifdef __cplusplus
}
#endif

#endif /* VFS_RAWFLASH_H */
