/**
 * @file    decimation_driver.c
 * @brief   Decimation IP 纯硬件驱动实现。
 */

#include "decimation_driver.h"
#include <string.h>

static inline void reg_write(uint32_t base, uint32_t off, uint32_t val)
{
    (void)base; (void)off; (void)val;
}

void decimation_init(decimation_dev_t *dev, uint32_t base_addr)
{
    memset(dev, 0, sizeof(*dev));
    dev->base_addr    = base_addr;
    dev->factor_x     = 1;
    dev->factor_y     = 1;
    dev->max_factor_x = 8;
    dev->max_factor_y = 8;
}

void decimation_set_factors(decimation_dev_t *dev, uint8_t fx, uint8_t fy)
{
    if (fx < 1) fx = 1;
    if (fy < 1) fy = 1;
    if (fx > dev->max_factor_x) fx = dev->max_factor_x;
    if (fy > dev->max_factor_y) fy = dev->max_factor_y;
    dev->factor_x = fx;
    dev->factor_y = fy;
}

void decimation_get_factors(const decimation_dev_t *dev, uint8_t *fx, uint8_t *fy)
{
    *fx = dev->factor_x;
    *fy = dev->factor_y;
}

void decimation_enable(decimation_dev_t *dev)
{
    reg_write(dev->base_addr, 0x00, 0x01);
}

void decimation_disable(decimation_dev_t *dev)
{
    reg_write(dev->base_addr, 0x00, 0x00);
}

void decimation_commit(const decimation_dev_t *dev)
{
    uint32_t val = ((uint32_t)dev->factor_y << 8) | dev->factor_x;
    reg_write(dev->base_addr, 0x04, val);
}
