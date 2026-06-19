/**
 * @file    decimation_driver.h
 * @brief   Decimation IP 纯硬件驱动 — 零框架依赖。
 *
 * 与 binning 类似，但通过跳像素/行实现降采样（不做像素合并）。
 * 寄存器映射：
 *   DEC_CTRL   (base + 0x00) : bit0 = enable
 *   DEC_FACTOR (base + 0x04) : [7:0]=factor_x, [15:8]=factor_y
 */

#ifndef DECIMATION_DRIVER_H
#define DECIMATION_DRIVER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t base_addr;
    uint8_t  factor_x;
    uint8_t  factor_y;
    uint8_t  max_factor_x;
    uint8_t  max_factor_y;
} decimation_dev_t;

void decimation_init(decimation_dev_t *dev, uint32_t base_addr);
void decimation_set_factors(decimation_dev_t *dev, uint8_t fx, uint8_t fy);
void decimation_get_factors(const decimation_dev_t *dev, uint8_t *fx, uint8_t *fy);
void decimation_enable(decimation_dev_t *dev);
void decimation_disable(decimation_dev_t *dev);
void decimation_commit(const decimation_dev_t *dev);

#ifdef __cplusplus
}
#endif

#endif
