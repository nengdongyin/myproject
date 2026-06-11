/**
 * @file    decoder_driver.c
 * @brief   Bayer→RGB 解码器纯硬件驱动实现。零框架依赖。
 */

#include "decoder_driver.h"
#include <string.h>

#define DEC_REG_CTRL    0x00
#define DEC_REG_PATTERN 0x04

/* supported input formats */
static const uint32_t dec_supported[] = {
    0x52415738, /* VSC_FMT_RAW8  */
    0x52415741, /* VSC_FMT_RAW10 */
    0x52415742, /* VSC_FMT_RAW12 */
    0
};

static inline void reg_write(uint32_t base, uint32_t off, uint32_t val)
{
    (void)base; (void)off; (void)val;
}

void decoder_init(decoder_dev_t *dev, uint32_t base_addr)
{
    memset(dev, 0, sizeof(*dev));
    dev->base_addr = base_addr;
    dev->fmt_in    = 0x52415741;  /* RAW10 */
    dev->fmt_out   = 0x52474238;  /* RGB888 */
    dev->pattern   = DEC_PATTERN_RGGB;
}

bool decoder_supports_input(const decoder_dev_t *dev, uint32_t pixel_format)
{
    (void)dev;
    for (int i = 0; dec_supported[i] != 0; i++)
        if (dec_supported[i] == pixel_format) return true;
    return false;
}

void decoder_set_pattern(decoder_dev_t *dev, decoder_pattern_t p)
{
    dev->pattern = p;
}

void decoder_enable(decoder_dev_t *dev)
{
    reg_write(dev->base_addr, DEC_REG_PATTERN, dev->pattern);
    reg_write(dev->base_addr, DEC_REG_CTRL, 0x01);
}

void decoder_disable(decoder_dev_t *dev)
{
    reg_write(dev->base_addr, DEC_REG_CTRL, 0x00);
}

void decoder_commit(const decoder_dev_t *dev)
{
    reg_write(dev->base_addr, DEC_REG_PATTERN, dev->pattern);
    reg_write(dev->base_addr, DEC_REG_CTRL, 0x01);
}
