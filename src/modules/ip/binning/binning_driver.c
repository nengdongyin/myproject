/**
 * @file    binning_driver.c
 * @brief   Binning IP 纯硬件驱动实现。零框架依赖。
 */

#include "binning_driver.h"
#include <string.h>

#define BIN_REG_CTRL 0x00

static inline void reg_write(uint32_t base, uint32_t off, uint32_t val)
{
    (void)base; (void)off; (void)val;
}

void binning_init(binning_dev_t *dev, uint32_t base_addr)
{
    memset(dev, 0, sizeof(*dev));
    dev->base_addr = base_addr;
    dev->factor_x  = 2;
    dev->factor_y  = 2;
}

int binning_set_factors(binning_dev_t *dev, uint8_t fx, uint8_t fy)
{
    /* only 1/2/4 supported */
    if (fx != 1 && fx != 2 && fx != 4) return -1;
    if (fy != 1 && fy != 2 && fy != 4) return -1;
    dev->factor_x = fx;
    dev->factor_y = fy;
    return 0;
}

void binning_get_factors(const binning_dev_t *dev, uint8_t *fx, uint8_t *fy)
{
    *fx = dev->factor_x;
    *fy = dev->factor_y;
}

void binning_enable(binning_dev_t *dev)
{
    uint32_t ctrl = 0x01 | ((dev->factor_x & 0x7) << 1) | ((dev->factor_y & 0x7) << 4);
    reg_write(dev->base_addr, BIN_REG_CTRL, ctrl);
}

void binning_disable(binning_dev_t *dev)
{
    reg_write(dev->base_addr, BIN_REG_CTRL, 0x00);
}

void binning_commit(const binning_dev_t *dev)
{
    uint32_t ctrl = 0x01 | ((dev->factor_x & 0x7) << 1) | ((dev->factor_y & 0x7) << 4);
    reg_write(dev->base_addr, BIN_REG_CTRL, ctrl);
}
