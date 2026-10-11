/**
 * @file storage_config.c
 * @brief 存储资源配置（设备/分区/挂载点）—— 资源与代码分离的落点
 *
 * 布局（RP2350 内部 flash，基址 0x10000000，共 4MB）：
 * @verbatim
 *   0x000000 .. 0x100000  app           (FAL: app)
 *   0x110000 .. 0x170000  param banks   (FAL: param_boot/factory/user0..3)
 *   0x170000 .. 0x190000  rawflash demo (本文件定义，2×64KB)
 *   0x200000 .. 0x400000  littlefs      (本文件定义，2MB) → /lfs
 * @endverbatim
 *
 * @section table_driven 表驱动装配
 * 本文件全部"资源"都集中在三张静态数据表里，通用装配逻辑（init）不随分区
 * 变化而改动：
 *   - @ref s_parts     分区几何表（一行 = 一个分区切片）
 *   - @ref s_raw_cfgs  rawflash 文件表（一行 = 一个静态注册"文件"）
 *   - @ref s_mounts    挂载表（一行 = 一个挂载点）
 * **新增/删除一个分区，只需增删 s_parts 的一行（及引用的挂载行）。**
 *
 * @section fal_role FAL 的角色
 * FAL 仅作为"介质/几何来源之一"出现在本装配层：分区几何来自本文件的表，
 * FAL 只被 storage_drv_fal_* 用来取整块 flash 的 read/prog/erase 与边界。
 * 若不用 FAL，替换 storage_drv_* 与 s_parts 的来源即可，VFS 核心不受影响。
 */

#include "storage_config.h"
#include "vfs.h"
#include "vfs_littlefs.h"
#include "vfs_rawflash.h"
#include "storage_drv_fal.h"
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

/* ================================================================
 *  配置数据表（唯一改配置处）
 * ================================================================ */

/** @brief 分区几何表条目：一行 = 一个分区切片 */
typedef struct
{
    const char *bdev_name; /**< 分区名（bdev 注册名 + 挂载表引用名） */
    const char *fal_flash; /**< 介质/几何来源：FAL flash 设备名 */
    uint32_t offset;       /**< 设备内起始字节偏移（须块对齐） */
    uint32_t size;         /**< 分区大小（须块对齐） */
} part_cfg_t;

/** @brief 分区几何表：加一个分区 = 加一行 */
static const part_cfg_t s_parts[] = {
    {"flash_lfs", STOR_FLASH_DEV, LFS_OFFSET, LFS_SIZE},
    {"flash_raw0", STOR_FLASH_DEV, RAW0_OFFSET, RAW0_SIZE},
    {"flash_raw1", STOR_FLASH_DEV, RAW1_OFFSET, RAW1_SIZE},
};

/** @brief rawflash 文件表条目：一行 = 一个静态注册"文件" */
typedef struct
{
    const char *file_name; /**< 逻辑文件名（挂载点下 rel） */
    const char *bdev_name; /**< 映射到的分区名 */
    uint8_t flags;         /**< VFS_RAWRO 等 */
} raw_cfg_t;

/** @brief rawflash 文件表：数据容量由分区几何自动推导 */
static const raw_cfg_t s_raw_cfgs[] = {
    {"boot_log", "flash_raw0", 0},
    {"factory", "flash_raw1", 0},
};

/* ---- 运行时派生物（由上面的表在 init 时生成，核心侧持备份） ---- */
/** @brief bdev 配置表（由 s_parts 转换而来，供 vfs_bdev_register_table） */
static vfs_bdev_cfg_t s_bdev_cfgs[sizeof(s_parts) / sizeof(s_parts[0])];
/** @brief rawflash 条目表（由 s_raw_cfgs + 已注册分区转换而来） */
static vfs_rawflash_entry_t s_raw_entries[sizeof(s_raw_cfgs) / sizeof(s_raw_cfgs[0])];
/** @brief rawflash 卷定义（作为 /raw 挂载点的 fs_data 传入） */
static vfs_rawflash_vol_t s_raw_vol = {s_raw_entries,
                                       sizeof(s_raw_cfgs) / sizeof(s_raw_cfgs[0])};

/** @brief 挂载表：一行 = 一个挂载点 */
static const vfs_mount_cfg_t s_mounts[] = {
    {STORAGE_MNT_FILES, VFS_FSTYPE_LITTLEFS, "flash_lfs", 0, NULL},
    {STORAGE_MNT_RAW, VFS_FSTYPE_RAWFLASH, NULL, 0, &s_raw_vol},
};

/* ================================================================
 *  通用装配逻辑（不随分区变化）
 * ================================================================ */

/**
 * @brief 由分区几何表 + 介质驱动生成 bdev 配置表
 * @return 0 成功；-ENOENT 介质设备不存在，-EINVAL 未对齐/越界
 */
static int build_bdev_cfgs(void)
{
    size_t n = sizeof(s_parts) / sizeof(s_parts[0]);
    for (size_t i = 0; i < n; i++)
    {
        const part_cfg_t *p = &s_parts[i];
        const vfs_storage_t *st = storage_drv_fal_storage(p->fal_flash);
        if (!st || st->blk_size == 0)
            return -ENOENT;

        /* 对齐与边界校验（与旧 vfs_bdev_fal_flash_init 的校验一致） */
        uint32_t dev_total = st->blk_count * st->blk_size;
        if ((p->offset % st->blk_size) != 0 || (p->size % st->blk_size) != 0)
            return -EINVAL;
        if (p->offset > dev_total || p->size > dev_total - p->offset)
            return -EINVAL;

        s_bdev_cfgs[i].name = p->bdev_name;
        s_bdev_cfgs[i].storage = st;
        s_bdev_cfgs[i].offset = p->offset;
        s_bdev_cfgs[i].blk_size = st->blk_size;
        s_bdev_cfgs[i].blk_count = p->size / st->blk_size;
    }
    return 0;
}

/**
 * @brief 由 rawflash 文件表 + 已注册分区生成 rawflash 条目表
 * @return 0 成功；-ENOENT 分区未注册或块数不足以容纳元数据块
 */
static int build_raw_entries(void)
{
    size_t n = sizeof(s_raw_cfgs) / sizeof(s_raw_cfgs[0]);
    for (size_t i = 0; i < n; i++)
    {
        const vfs_bdev_t *bd = vfs_bdev_find(s_raw_cfgs[i].bdev_name);
        if (!bd || bd->blk_count < 2)
            return -ENOENT;

        /* 数据容量 = 数据区容量上限 = (块数-1)*块大小（末块为元数据块） */
        s_raw_entries[i].name = s_raw_cfgs[i].file_name;
        s_raw_entries[i].bdev = bd;
        s_raw_entries[i].size = (bd->blk_count - 1) * bd->blk_size;
        s_raw_entries[i].flags = s_raw_cfgs[i].flags;
    }
    return 0;
}

/**
 * @brief 初始化并挂载所有存储卷
 * @details 通用装配流程（与具体分区无关）：vfs_init → fal_init → 由表生成
 *          bdev 配置 → 表驱动注册 bdev → 生成 rawflash 条目 → 注册 FS 类型
 *          → 按挂载表一次性挂载。
 * @return 0 成功；< 0 失败步骤的负 errno
 */
int storage_config_init(void)
{
    int rc;

    /* 0) 初始化 VFS 核心（建核心锁）。必须最先执行 */
    rc = vfs_init();
    if (rc != 0)
        return rc;

    fal_init(); /* 幂等；FAL 作为介质/几何来源之一 */

    /* 1) 由分区几何表 + 介质驱动生成 bdev 配置，表驱动注册 */
    rc = build_bdev_cfgs();
    if (rc != 0)
        return rc;
    rc = vfs_bdev_register_table(s_bdev_cfgs,
                                 sizeof(s_bdev_cfgs) / sizeof(s_bdev_cfgs[0]));
    if (rc != 0)
        return rc;

    /* 2) 由 rawflash 文件表 + 已注册分区生成 rawflash 条目表 */
    rc = build_raw_entries();
    if (rc != 0)
        return rc;

    /* 3) 注册文件系统类型（重复注册视为可接受） */
    rc = vfs_register(VFS_FSTYPE_LITTLEFS, &vfs_littlefs_ops);
    if (rc != 0 && rc != -EALREADY)
        return rc;
    rc = vfs_register(VFS_FSTYPE_RAWFLASH, &vfs_rawflash_ops);
    if (rc != 0 && rc != -EALREADY)
        return rc;

    /* 4) 按挂载表挂载 */
    return vfs_mount_table(s_mounts, sizeof(s_mounts) / sizeof(s_mounts[0]));
}

void storage_config_deinit(void)
{
    vfs_deinit();
}
