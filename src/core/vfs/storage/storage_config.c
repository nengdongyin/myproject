/**
 * @file storage_config.c
 * @brief 存储资源配置（设备/分区/挂载点）—— 资源与代码分离的落点
 *
 * 布局（RP2350 内部 flash，基址 0x10000000，共 4MB；几何单一数据源在
 * fal_cfg.h 的 FAL_PART_TABLE，见下）：
 * @verbatim
 *   0x000000 .. 0x100000  app           (FAL: app)
 *   0x110000 .. 0x170000  param banks   (FAL: param_boot/factory/user0..3)
 *   0x170000 .. 0x180000  rawflash 0    (FAL: raw0)
 *   0x180000 .. 0x190000  rawflash 1    (FAL: raw1)
 *   0x200000 .. 0x400000  littlefs      (FAL: lfs) → /lfs
 * @endverbatim
 *
 * @section table_driven 表驱动装配
 * 本文件全部"资源"都集中在三张静态数据表里，通用装配逻辑（init）不随分区
 * 变化而改动：
 *   - @ref s_parts     分区几何表（一行 = 一个分区切片）
 *   - @ref s_raw_cfgs  rawflash 文件表（一行 = 一个静态注册"文件"）
 *   - @ref s_mounts    挂载表（一行 = 一个挂载点）
 * **新增/删除一个分区，只需增删 FAL 分区表（fal_cfg.h）与 s_parts 的一行。**
 *
 * @section fal_role FAL 的角色
 * FAL 分区表（fal_cfg.h 的 FAL_PART_TABLE）是 flash 分区的**单一几何数据源**：
 * 本文件 s_parts 里的 FAL 分区按名字引用（fal_partition_find），offset/len 取自
 * 分区表，不再硬编码偏移，避免与 param_manager（flashdb/mkv 也用 FAL 分区）
 * 双源漂移导致重叠。非 FAL 介质（如 SD）仍通过 get_storage 取整介质。
 */

#include "storage_config.h"
#include "vfs.h"
#include "vfs_littlefs.h"
#include "vfs_rawflash.h"
#include "vfs_fatfs.h"
#include "storage_drv_fal.h"
#include "storage_drv_sd.h"
#include "fal.h"
#include <errno.h>

/* ---- flash 分区几何：单一数据源在 fal_cfg.h 的 FAL_PART_TABLE ----
 * 本文件通过 s_parts 里的 fal_part 名字引用（fal_partition_find），
 * 不再硬编码偏移/大小；详见文件头 @section fal_role。 */

/* ================================================================
 *  配置数据表（唯一改配置处）
 * ================================================================ */

/** @brief 介质来源获取器：按名字取得（或探测）一个 vfs_storage_t */
typedef const vfs_storage_t *(*storage_getter_t)(const char *name);

/** @brief 分区可选性：介质缺失时的处理方式 */
typedef enum
{
    PART_REQUIRED = 0, /**< 必需：介质缺失 → 整体装配失败 */
    PART_OPTIONAL = 1, /**< 可选：介质缺失 → 跳过该分区（如未插 SD 卡） */
} part_optional_t;

/** @brief 分区几何表条目：一行 = 一个分区切片 */
typedef struct
{
    const char *bdev_name;      /**< 分区名（bdev 注册名 + 挂载表引用名） */
    storage_getter_t get_storage; /**< 整介质获取器（非 FAL 介质，如 SD；FAL 介质为 NULL） */
    const char *media_name;     /**< 传给 get_storage 的名字（磁盘名等；FAL 介质为 NULL） */
    const char *fal_part;       /**< FAL 分区名（几何单一数据源）；非 FAL 介质为 NULL */
    part_optional_t optional;   /**< PART_REQUIRED / PART_OPTIONAL */
} part_cfg_t;

/** @brief 分区几何表：加一个分区 = 加一行（FAL 介质填 fal_part，非 FAL 填 get_storage） */
static const part_cfg_t s_parts[] = {
    {
        .bdev_name = "flash_lfs",
        .fal_part  = FAL_PART_LFS_NAME,
        .optional  = PART_REQUIRED,
    },
    {
        .bdev_name = "flash_raw0",
        .fal_part  = FAL_PART_RAW0_NAME,
        .optional  = PART_REQUIRED,
    },
    {
        .bdev_name = "flash_raw1",
        .fal_part  = FAL_PART_RAW1_NAME,
        .optional  = PART_REQUIRED,
    },
#if VFS_ENABLE_SD
    {
        .bdev_name   = "sd_card",
        .get_storage = storage_drv_sd_storage,
        .media_name  = STORAGE_SD_DISK_NAME,
        .optional    = PART_OPTIONAL,
    },
#endif
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
    {
        .file_name = "boot_log",
        .bdev_name = "flash_raw0",
        .flags     = 0,
    },
    {
        .file_name = "factory",
        .bdev_name = "flash_raw1",
        .flags     = 0,
    },
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
    {
        .mnt_point = STORAGE_MNT_FILES,
        .fstype    = VFS_FSTYPE_LITTLEFS,
        .bdev_name = "flash_lfs",
        .flags     = VFS_MNT_AUTOFORMAT,
        .fs_data   = NULL,
    },
    {
        .mnt_point = STORAGE_MNT_RAW,
        .fstype    = VFS_FSTYPE_RAWFLASH,
        .bdev_name = NULL,
        .flags     = 0,
        .fs_data   = &s_raw_vol,
    },
#if VFS_ENABLE_FATFS
    {
        .mnt_point = STORAGE_MNT_SD,
        .fstype    = VFS_FSTYPE_FATFS,
        .bdev_name = "sd_card",
        .flags     = VFS_MNT_AUTOFORMAT,
        .fs_data   = NULL,
    },
#endif
};

/* ================================================================
 *  通用装配逻辑（不随分区变化）
 * ================================================================ */

/**
 * @brief 由分区几何表 + 介质驱动生成 bdev 配置表（可选介质缺失时跳过）
 * @details FAL 介质的几何取自 FAL 分区表（fal_partition_find，单一数据源）；
 *          非 FAL 介质取整介质（get_storage）。
 * @return >= 0 生成的 bdev 条目数；-ENOENT 必需介质缺失，-EINVAL 未对齐/越界
 */
static int build_bdev_cfgs(void)
{
    size_t n = sizeof(s_parts) / sizeof(s_parts[0]);
    size_t built = 0;
    for (size_t i = 0; i < n; i++)
    {
        const part_cfg_t *p = &s_parts[i];
        const vfs_storage_t *st = NULL;
        uint64_t offset = 0;
        uint64_t size = 0;

        if (p->fal_part)
        {
            /* 几何以 FAL 分区表为准（与 param 区同源，避免双源漂移/重叠） */
            const struct fal_partition *fp = fal_partition_find(p->fal_part);
            if (!fp)
            {
                if (p->optional)
                    continue;
                return -ENOENT;
            }
            st = storage_drv_fal_storage(fp->flash_name);
            offset = (uint64_t)fp->offset;
            size = (uint64_t)fp->len;
        }
        else
        {
            /* 非 FAL 介质：整介质（如 SD 整卡） */
            st = p->get_storage(p->media_name);
        }

        if (!st || st->blk_size == 0)
        {
            if (p->optional)
                continue;   /* 可选介质缺失 → 跳过该分区，不中断整体 */
            return -ENOENT;
        }
        if (!p->fal_part)
            size = (uint64_t)st->blk_count * st->blk_size; /* 非 FAL：整介质 */

        /* 对齐与边界校验 */
        uint64_t dev_total = (uint64_t)st->blk_count * st->blk_size;
        if ((offset % st->blk_size) != 0 || (size % st->blk_size) != 0)
            return -EINVAL;
        if (offset > dev_total || size > dev_total - offset)
            return -EINVAL;

        vfs_bdev_cfg_t *c = &s_bdev_cfgs[built];
        c->name = p->bdev_name;
        c->storage = st;
        c->offset = offset;
        c->blk_size = st->blk_size;
        c->blk_count = (uint32_t)(size / st->blk_size);
        built++;
    }
    return (int)built;
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
 * @brief 按挂载表挂载已注册 bdev 的卷
 * @details 对 bdev_name 非空但 bdev 未注册的挂载条目（如未插卡的可选 SD 卷），
 *          跳过挂载；其余条目交 vfs_mount_table 一次性挂载。
 * @return vfs_mount_table 的返回码（0 全部成功，否则最后一个失败码）
 */
static int mount_registered(void)
{
    vfs_mount_cfg_t mnts[sizeof(s_mounts) / sizeof(s_mounts[0])];
    size_t nm = 0;
    for (size_t i = 0; i < sizeof(s_mounts) / sizeof(s_mounts[0]); i++)
    {
        const vfs_mount_cfg_t *m = &s_mounts[i];
        if (m->bdev_name && !vfs_bdev_find(m->bdev_name))
            continue; /* bdev 未注册（可选介质缺失）→ 跳过该挂载点 */
        mnts[nm++] = *m;
    }
    return vfs_mount_table(mnts, nm);
}

/**
 * @brief 初始化并挂载所有存储卷
 * @details 通用装配流程（与具体分区无关）：vfs_init → fal_init → 由表生成
 *          bdev 配置 → 表驱动注册 bdev → 生成 rawflash 条目 → 注册 FS 类型
 *          → 按挂载表挂载（可选介质缺失时跳过其挂载点）。
 * @return 0 成功；< 0 失败步骤的负 errno
 */
int storage_config_init(void)
{
    int rc;

    /* 0) 初始化 VFS 核心（建核心锁）。必须最先执行 */
    rc = vfs_init();
    if (rc != 0)
        return rc;

    /* FAL 初始化失败则无法解析分区几何，直接失败（勿继续，否则 fal_partition_find 拿不到分区） */
    if (fal_init() < 0)
        return -EIO;

    /* 1) 由分区几何表 + 介质驱动生成 bdev 配置，表驱动注册 */
    int nbdev = build_bdev_cfgs();
    if (nbdev < 0)
        return nbdev;
    if (nbdev > 0)
    {
        rc = vfs_bdev_register_table(s_bdev_cfgs, (size_t)nbdev);
        if (rc != 0)
            return rc;
    }

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
#if VFS_ENABLE_FATFS
    rc = vfs_register(VFS_FSTYPE_FATFS, &vfs_fatfs_ops);
    if (rc != 0 && rc != -EALREADY)
        return rc;
#endif

    /* 4) 按挂载表挂载（可选介质缺失时其 bdev 未注册，自动跳过该挂载点） */
    return mount_registered();
}

void storage_config_deinit(void)
{
    vfs_deinit();
}
