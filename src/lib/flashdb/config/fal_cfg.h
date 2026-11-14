#ifndef _FAL_CFG_H_
#define _FAL_CFG_H_

#include "fal_def.h"

#define FAL_PART_HAS_TABLE_CFG

#define RP2350_FLASH_DEV_NAME "rp2350_flash"

#define RP2350_FLASH_BASE 0x10000000u
#define RP2350_FLASH_SIZE (4 * 1024 * 1024)
#define RP2350_FLASH_BLK_SIZE 4096u

#define PART_APP_SIZE (1024 * 1024)
#define PART_PARAM_BANK_SIZE (64 * 1024)

/* ---- VFS 用分区（rawflash + littlefs）：与 param 区同源于此表（单一几何数据源） ---- */
/* param 区结束偏移 = app + 6 个 param bank（boot + factory + user0..3） */
#define PART_PARAM_REGION_END (PART_APP_SIZE + PART_PARAM_BANK_SIZE * 7)
/* rawflash 单分区大小（本表定义 2 个，共 128KB） */
#define PART_RAW_SIZE          (64 * 1024)
/* littlefs 分区（2MB @ 2MB；与 rawflash 之间留 448KB 空档） */
#define PART_LFS_OFFSET        (2 * 1024 * 1024)
#define PART_LFS_SIZE          (2 * 1024 * 1024)

/* VFS 分区名（FAL_PART_TABLE 与 storage_config.c 的 s_parts 共用，避免双写漂移） */
#define FAL_PART_RAW0_NAME     "raw0"
#define FAL_PART_RAW1_NAME     "raw1"
#define FAL_PART_LFS_NAME      "lfs"

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
/* 布局不变量：param 区 / rawflash / littlefs 不得重叠，且不得越界 */
_Static_assert(PART_PARAM_REGION_END + 2 * PART_RAW_SIZE <= PART_LFS_OFFSET,
               "rawflash 与 littlefs 重叠");
_Static_assert(PART_LFS_OFFSET + PART_LFS_SIZE <= RP2350_FLASH_SIZE,
               "littlefs 超出 flash 范围");
#endif

extern const struct fal_flash_dev rp2350_onchip_flash;

#define FAL_FLASH_DEV_TABLE   \
    {                         \
        &rp2350_onchip_flash, \
    }

#define FAL_PART_TABLE                                                                                                                    \
    {                                                                                                                                     \
        {FAL_PART_MAGIC_WORD, "app", RP2350_FLASH_DEV_NAME, 0, PART_APP_SIZE, 0},                                                         \
        {FAL_PART_MAGIC_WORD, "param_boot", RP2350_FLASH_DEV_NAME, PART_APP_SIZE + PART_PARAM_BANK_SIZE * 1, PART_PARAM_BANK_SIZE, 0},    \
        {FAL_PART_MAGIC_WORD, "param_factory", RP2350_FLASH_DEV_NAME, PART_APP_SIZE + PART_PARAM_BANK_SIZE * 2, PART_PARAM_BANK_SIZE, 0}, \
        {FAL_PART_MAGIC_WORD, "param_user0", RP2350_FLASH_DEV_NAME, PART_APP_SIZE + PART_PARAM_BANK_SIZE * 3, PART_PARAM_BANK_SIZE, 0},   \
        {FAL_PART_MAGIC_WORD, "param_user1", RP2350_FLASH_DEV_NAME, PART_APP_SIZE + PART_PARAM_BANK_SIZE * 4, PART_PARAM_BANK_SIZE, 0},   \
        {FAL_PART_MAGIC_WORD, "param_user2", RP2350_FLASH_DEV_NAME, PART_APP_SIZE + PART_PARAM_BANK_SIZE * 5, PART_PARAM_BANK_SIZE, 0},   \
        {FAL_PART_MAGIC_WORD, "param_user3", RP2350_FLASH_DEV_NAME, PART_APP_SIZE + PART_PARAM_BANK_SIZE * 6, PART_PARAM_BANK_SIZE, 0},   \
        {FAL_PART_MAGIC_WORD, FAL_PART_RAW0_NAME, RP2350_FLASH_DEV_NAME, PART_PARAM_REGION_END, PART_RAW_SIZE, 0}, \
        {FAL_PART_MAGIC_WORD, FAL_PART_RAW1_NAME, RP2350_FLASH_DEV_NAME, PART_PARAM_REGION_END + PART_RAW_SIZE, PART_RAW_SIZE, 0}, \
        {FAL_PART_MAGIC_WORD, FAL_PART_LFS_NAME, RP2350_FLASH_DEV_NAME, PART_LFS_OFFSET, PART_LFS_SIZE, 0}, \
    }

#endif /* _FAL_CFG_H_ */
