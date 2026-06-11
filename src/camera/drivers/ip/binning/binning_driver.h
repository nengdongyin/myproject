/**
 * @file    binning_driver.h
 * @brief   Binning IP 纯硬件驱动 — 零框架依赖。
 *
 * 寄存器映射：
 *   BIN_CTRL     (base + 0x00)  : bit0 = enable, bit1..4 = factor
 *   BIN_STATUS   (base + 0x04)  : ro
 */

#ifndef BINNING_DRIVER_H
#define BINNING_DRIVER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t base_addr;
    uint8_t  factor_x;         /* 1/2/4 */
    uint8_t  factor_y;
} binning_dev_t;

void binning_init(binning_dev_t *dev, uint32_t base_addr);
int  binning_set_factors(binning_dev_t *dev, uint8_t fx, uint8_t fy);
void binning_get_factors(const binning_dev_t *dev, uint8_t *fx, uint8_t *fy);
void binning_enable(binning_dev_t *dev);
void binning_disable(binning_dev_t *dev);
void binning_commit(const binning_dev_t *dev);

#ifdef __cplusplus
}
#endif

#endif
