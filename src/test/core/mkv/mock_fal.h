#ifndef MOCK_FAL_H
#define MOCK_FAL_H

/**
 * @file mock_fal.h
 * @brief mkv 原生单测用的 RAM-backed FAL mock
 *
 * 提供 mkv.c 所需的 fal_* 符号 (fal_init / fal_partition_find /
 * fal_flash_device_find / fal_partition_read/write/erase)，底层是一块
 * 64KB RAM 缓冲，erase 置 0xFF，模拟 NOR Flash 语义。
 * 测试可自由设定 blk_size 与分区大小，以在极小 sector 上快速触发 GC。
 */

#include "fal.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief mock 分区名 (mkv_init 传入此名) */
#define MOCK_PART_NAME  "mock_part"

/**
 * @brief 复位 mock：整块 RAM 置 0xFF，设定 blk_size 与分区大小
 * @param blk_size      擦除块大小 (= mkv 的 sector_size，因 MULT 默认 1)
 * @param partition_len 分区总长 (必须是 blk_size 的整数倍，且 ≥ 2×blk_size)
 */
void mock_fal_reset(uint32_t blk_size, uint32_t partition_len);

/** @brief 直接访问底层 RAM (调试/断言用) */
const uint8_t *mock_fal_ram(void);
uint32_t mock_fal_ram_size(void);

#ifdef __cplusplus
}
#endif

#endif /* MOCK_FAL_H */
