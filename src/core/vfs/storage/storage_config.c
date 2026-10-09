/**
 * @file storage_config.c
 * @brief 存储资源配置（设备/分区/挂载点）—— 需求7 的"资源与代码分离"落点
 *
 * 布局（RP2350 内部 flash，基址 0x10000000，共 4MB）：
 * @verbatim
 *   0x000000 .. 0x100000  app           (FAL: app)
 *   0x110000 .. 0x170000  param banks   (FAL: param_boot/factory/user0..3)
 *   0x170000 .. 0x190000  rawflash demo (本文件定义，2×64KB)
 *   0x200000 .. 0x400000  littlefs      (本文件定义，2MB) → /lfs
 * @endverbatim
 *
 * 仅依赖 FAL（仓库已有的 flash 抽象）+ VFS 后端，不依赖任何 RTOS/fs 子系统。
 */

#include "storage_config.h"
#include "vfs.h"
#include "vfs_littlefs.h"
#include "vfs_rawflash.h"
#include "fal.h"
#include <errno.h>

/* ---- 物理 flash 设备名（见 fal_cfg.h 的 RP2350_FLASH_DEV_NAME） ---- */
/** @brief FAL flash 设备名（分区几何的基准设备） */
#define STOR_FLASH_DEV "rp2350_flash"

/* ---- littlefs 分区几何 ---- */
/** @brief littlefs 分区起始偏移（设备内相对偏移，须块对齐） */
#define LFS_OFFSET 0x00200000u
/** @brief littlefs 分区大小（2MB） */
#define LFS_SIZE   0x00200000u

/* ---- rawflash 分区（两块 64KB） ---- */
/** @brief rawflash 第 0 分区起始偏移 */
#define RAW0_OFFSET 0x00170000u
/** @brief rawflash 第 0 分区大小（64KB） */
#define RAW0_SIZE   0x00010000u
/** @brief rawflash 第 1 分区起始偏移 */
#define RAW1_OFFSET 0x00180000u
/** @brief rawflash 第 1 分区大小（64KB） */
#define RAW1_SIZE   0x00010000u

/* ---- bdev 实例（静态，生命周期同进程） ---- */
/** @brief littlefs 分区切片（/lfs 挂载源） */
static vfs_bdev_t s_bdev_lfs;
/** @brief rawflash 第 0 分区切片（boot_log） */
static vfs_bdev_t s_bdev_raw0;
/** @brief rawflash 第 1 分区切片（factory） */
static vfs_bdev_t s_bdev_raw1;

/* ---- rawflash 静态注册表：文件名 → 分区 + 容量 ---- */
/**
 * @brief rawflash 静态注册表：每个"文件"映射到一个分区
 * @note 数据容量 = 数据区容量上限 = (块数-1)*块大小 = 15*4KB = 0xF000
 */
static const vfs_rawflash_entry_t s_raw_entries[] = {
    {"boot_log", &s_bdev_raw0, 0xF000u, 0},
    {"factory", &s_bdev_raw1, 0xF000u, 0},
};
/** @brief rawflash 卷定义（作为 /raw 挂载点的 fs_data 传入） */
static const vfs_rawflash_vol_t s_raw_vol = {s_raw_entries, 2};

/**
 * @brief 初始化并挂载所有存储卷
 * @details 按依赖顺序装配：先 vfs_init 建核心锁，再 fal_init 初始化 flash，
 *          然后用自定义 offset/size 构造各 bdev（几何集中在此文件），注册块
 *          设备与文件系统类型，最后用静态挂载表一次性挂载 /lfs 与 /raw。
 * @return 0 成功；< 0 失败步骤的负 errno（挂载阶段为最后一个失败码，
 *         中间步骤失败立即返回）
 */
int storage_config_init(void)
{
    int rc;

    /* 0) 初始化 VFS 核心（建核心锁）。必须最先执行，否则内部锁失效、并发不受保护 */
    rc = vfs_init();
    if (rc != 0)
        return rc;

    fal_init(); /* 幂等 */

    /* 1) 定义分区（几何集中于此）：flash 设备 + offset/size → bdev */
    rc = vfs_bdev_fal_flash_init(&s_bdev_lfs, "flash_lfs", STOR_FLASH_DEV,
                                 LFS_OFFSET, LFS_SIZE);
    if (rc != 0)
        return rc;
    rc = vfs_bdev_fal_flash_init(&s_bdev_raw0, "flash_raw0", STOR_FLASH_DEV,
                                 RAW0_OFFSET, RAW0_SIZE);
    if (rc != 0)
        return rc;
    rc = vfs_bdev_fal_flash_init(&s_bdev_raw1, "flash_raw1", STOR_FLASH_DEV,
                                 RAW1_OFFSET, RAW1_SIZE);
    if (rc != 0)
        return rc;

    /* 2) 注册块设备 */
    rc = vfs_bdev_register("flash_lfs", &s_bdev_lfs);
    if (rc != 0)
        return rc;
    rc = vfs_bdev_register("flash_raw0", &s_bdev_raw0);
    if (rc != 0)
        return rc;
    rc = vfs_bdev_register("flash_raw1", &s_bdev_raw1);
    if (rc != 0)
        return rc;

    /* 3) 注册文件系统类型（重复注册视为可接受） */
    rc = vfs_register(VFS_FSTYPE_LITTLEFS, &vfs_littlefs_ops);
    if (rc != 0 && rc != -EALREADY)
        return rc;
    rc = vfs_register(VFS_FSTYPE_RAWFLASH, &vfs_rawflash_ops);
    if (rc != 0 && rc != -EALREADY)
        return rc;

    /* 4) 挂载表 */
    static const vfs_mount_cfg_t mounts[] = {
        {STORAGE_MNT_FILES, VFS_FSTYPE_LITTLEFS, "flash_lfs", 0, NULL},
        {STORAGE_MNT_RAW, VFS_FSTYPE_RAWFLASH, NULL, 0, &s_raw_vol},
    };
    return vfs_mount_table(mounts, sizeof(mounts) / sizeof(mounts[0]));
}

void storage_config_deinit(void)
{
    vfs_deinit();
}
