/**
 * @file    histogram_driver.c
 * @brief   直方图统计 IP 纯硬件驱动。ANALYZER — 零框架依赖。
 */

#include "histogram_driver.h"
#include <string.h>

static inline void reg_write(uint32_t base, uint32_t off, uint32_t val)
{
    (void)base; (void)off; (void)val;
}

void histogram_init(histogram_dev_t *dev, uint32_t base_addr)
{
    memset(dev, 0, sizeof(*dev));
    dev->base_addr   = base_addr;
    dev->max_bins    = 256;
    dev->active_bins = 64;
    dev->supports_rgb = true;
}

bool histogram_supports_format(const histogram_dev_t *dev, uint32_t pixel_format)
{
    (void)dev;
    /* supports RAW8/10/12 and RGB888 — matches P1 schema */
    return (pixel_format == 0x52415738 ||  /* RAW8  */
            pixel_format == 0x52415741 ||  /* RAW10 */
            pixel_format == 0x52415742 ||  /* RAW12 */
            pixel_format == 0x52474238);   /* RGB888 */
}

void histogram_set_bins(histogram_dev_t *dev, uint32_t bins)
{
    if (bins <= dev->max_bins) dev->active_bins = bins;
}

void histogram_enable(histogram_dev_t *dev)
{
    reg_write(dev->base_addr, 0x00, 0x01);
}

void histogram_disable(histogram_dev_t *dev)
{
    reg_write(dev->base_addr, 0x00, 0x00);
}

void histogram_commit(const histogram_dev_t *dev)
{
    reg_write(dev->base_addr, 0x04, dev->active_bins);
    reg_write(dev->base_addr, 0x00, 0x01);
}
