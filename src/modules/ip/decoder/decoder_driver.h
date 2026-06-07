/**
 * @file    decoder_driver.h
 * @brief   Bayer→RGB 解码器纯硬件驱动 — 零框架依赖。
 */

#ifndef DECODER_DRIVER_H
#define DECODER_DRIVER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { DEC_PATTERN_RGGB = 0, DEC_PATTERN_BGGR, DEC_PATTERN_GRBG, DEC_PATTERN_GBRG } decoder_pattern_t;

typedef struct {
    uint32_t base_addr;
    uint32_t fmt_in;           /* VSC_FMT_RAW8/10/12 */
    uint32_t fmt_out;          /* VSC_FMT_RGB888 */
    decoder_pattern_t pattern;
} decoder_dev_t;

void decoder_init(decoder_dev_t *dev, uint32_t base_addr);
bool decoder_supports_input(const decoder_dev_t *dev, uint32_t pixel_format);
void decoder_set_pattern(decoder_dev_t *dev, decoder_pattern_t p);
void decoder_enable(decoder_dev_t *dev);
void decoder_disable(decoder_dev_t *dev);
void decoder_commit(const decoder_dev_t *dev);

#ifdef __cplusplus
}
#endif
#endif
