/**
 * @file    histogram_driver.h
 * @brief   直方图统计 IP 纯硬件驱动 — ANALYZER（TAP observer）。
 *
 * 只有 SINK pad，无 SOURCE pad。不修改像素流，只读取统计数据。
 * 零框架依赖。
 */

#ifndef HISTOGRAM_DRIVER_H
#define HISTOGRAM_DRIVER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t base_addr;
    uint32_t max_bins;         /* 硬件最大 bin 数 */
    uint32_t active_bins;      /* 当前使用的 bin 数 */
    bool     supports_rgb;     /* 是否支持 RGB 三通道 */
} histogram_dev_t;

void histogram_init(histogram_dev_t *dev, uint32_t base_addr);
bool histogram_supports_format(const histogram_dev_t *dev, uint32_t pixel_format);
void histogram_set_bins(histogram_dev_t *dev, uint32_t bins);
void histogram_enable(histogram_dev_t *dev);
void histogram_disable(histogram_dev_t *dev);
void histogram_commit(const histogram_dev_t *dev);

#ifdef __cplusplus
}
#endif
#endif
