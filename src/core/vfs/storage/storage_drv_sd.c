/**
 * @file storage_drv_sd.c
 * @brief SD 卡介质驱动实现 —— Zephyr disk_access → vfs_storage_t
 *
 * 见 storage_drv_sd.h（含"只支持整扇区访问 / 无 erase / 换卡需重启"三条限制）。
 * 整文件在 VFS_ENABLE_SD=1 时才编译；默认 0，因此不开 SD 的工程零影响。
 */

#include "storage_drv_sd.h"

#if VFS_ENABLE_SD

#include <zephyr/storage/disk_access.h>
#include <string.h>
#include <errno.h>

/** @brief SD 逻辑扇区大小（与 FatFs / vfs_fatfs.c 的硬性约定一致） */
#define SD_SECTOR_SIZE 512u

/** @brief 磁盘名缓存长度 */
#define SD_NAME_MAX 16

/** @brief 单卡实例（bdev 持有 &st，故必须静态生命周期） */
typedef struct
{
    uint8_t used;          /**< 实例已绑定 */
    uint8_t write_protect; /**< 卡写保护（DISK_STATUS_WR_PROTECT） */
    vfs_storage_t st;      /**< 对外介质描述 */
    char name[SD_NAME_MAX];/**< 磁盘名（st.name 指向此处） */
} sd_state_t;

static sd_state_t s_sd;

/* ================================================================
 *  介质回调（转发到 Zephyr disk_access）
 * ================================================================ */

/** @brief 整扇区读；非 512 对齐返回 -EINVAL（见 file 头 @section limits） */
static int sd_read(vfs_storage_t *s, uint32_t byte_off, void *buf, size_t n)
{
    if (!s || !s->ctx || !buf)
        return -EINVAL;
    if ((byte_off % SD_SECTOR_SIZE) != 0 || (n % SD_SECTOR_SIZE) != 0)
        return -EINVAL;
    if (n == 0)
        return 0;

    const sd_state_t *st = (const sd_state_t *)s->ctx;
    if (disk_access_read(st->name, buf, byte_off / SD_SECTOR_SIZE,
                         (uint32_t)(n / SD_SECTOR_SIZE)) != 0)
        return -EIO;
    return 0;
}

/** @brief 整扇区写；写保护的卡返回 -EROFS */
static int sd_prog(vfs_storage_t *s, uint32_t byte_off, const void *buf, size_t n)
{
    if (!s || !s->ctx || !buf)
        return -EINVAL;
    if ((byte_off % SD_SECTOR_SIZE) != 0 || (n % SD_SECTOR_SIZE) != 0)
        return -EINVAL;
    if (n == 0)
        return 0;

    const sd_state_t *st = (const sd_state_t *)s->ctx;
    if (st->write_protect)
        return -EROFS;

    if (disk_access_write(st->name, buf, byte_off / SD_SECTOR_SIZE,
                          (uint32_t)(n / SD_SECTOR_SIZE)) != 0)
        return -EIO;
    return 0;
}

/* ================================================================
 *  对外接口
 * ================================================================ */

const vfs_storage_t *storage_drv_sd_storage(const char *disk_name)
{
    if (!disk_name || disk_name[0] == '\0')
        disk_name = STORAGE_SD_DISK_NAME;

    /* 已绑定则只接受同一张盘：几何已快照，换卡需重启 */
    if (s_sd.used && strcmp(s_sd.name, disk_name) != 0)
        return NULL;

    size_t nlen = strlen(disk_name);
    if (nlen >= SD_NAME_MAX)
        return NULL;

    /* 探测：init → status → 几何。卡不在是正常状态，全部以 NULL 表达 */
    if (disk_access_init(disk_name) != 0)
        return NULL;

    int status = disk_access_status(disk_name);
    if (status != DISK_STATUS_OK && status != DISK_STATUS_WR_PROTECT)
        return NULL; /* DISK_STATUS_NOMEDIA / UNINIT */

    uint32_t sector_size = 0;
    if (disk_access_ioctl(disk_name, DISK_IOCTL_GET_SECTOR_SIZE, &sector_size) != 0 ||
        sector_size != SD_SECTOR_SIZE)
        return NULL; /* FatFs 侧硬校验 512，非 512 的盘直接拒 */

    uint32_t sector_count = 0;
    if (disk_access_ioctl(disk_name, DISK_IOCTL_GET_SECTOR_COUNT, &sector_count) != 0 ||
        sector_count == 0)
        return NULL;

    memcpy(s_sd.name, disk_name, nlen + 1);
    s_sd.write_protect = (status == DISK_STATUS_WR_PROTECT) ? 1u : 0u;
    s_sd.st.name = s_sd.name;
    s_sd.st.blk_size = sector_size;
    s_sd.st.blk_count = sector_count;
    s_sd.st.read = sd_read;
    s_sd.st.prog = sd_prog;
    s_sd.st.erase = NULL; /* SD 无擦除语义；FatFs 不调用 */
    s_sd.st.ctx = &s_sd;
    s_sd.used = 1;
    return &s_sd.st;
}

#endif /* VFS_ENABLE_SD */
