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
    }

#endif /* _FAL_CFG_H_ */
