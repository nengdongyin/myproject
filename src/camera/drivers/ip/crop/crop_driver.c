/**
 * @file    crop_driver.c
 * @brief   Crop / ROI IP 纯硬件驱动实现。
 *
 * 零框架依赖。不引用 vsc_types.h 或 param_manager.h。
 * 寄存器访问通过 volatile 指针（实际项目替换为 FPGA 总线宏）。
 */

#include "crop_driver.h"
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════
 *  虚拟寄存器访问（实际项目替换为 FPGA 寄存器读写宏）
 * ═══════════════════════════════════════════════════════════════════════ */

#define CROP_REG_CTRL   0x00
#define CROP_REG_ROI_X  0x04
#define CROP_REG_ROI_Y  0x08
#define CROP_REG_ROI_W  0x0C
#define CROP_REG_ROI_H  0x10

static inline void reg_write(uint32_t base, uint32_t off, uint32_t val)
{
    (void)base; (void)off; (void)val;
    /* volatile uint32_t *r = (volatile uint32_t *)(uintptr_t)(base + off);
     * *r = val;  — actual FPGA write, disabled for host-based tests */
}

/* ═══════════════════════════════════════════════════════════════════════
 *  公开 API
 * ═══════════════════════════════════════════════════════════════════════ */

void crop_init(crop_dev_t *dev, uint32_t base_addr)
{
    memset(dev, 0, sizeof(*dev));
    dev->base_addr = base_addr;
    dev->max_w     = 8192;
    dev->max_h     = 8192;
    dev->roi_w     = 1920;
    dev->roi_h     = 1080;
    dev->align_w   = 8;
    dev->align_h   = 8;
}

int crop_set_limits(crop_dev_t *dev, uint32_t max_w, uint32_t max_h)
{
    /* 只能收紧，不能放宽 */
    if (max_w > dev->max_w || max_h > dev->max_h)
        return -1;
    dev->max_w = max_w;
    dev->max_h = max_h;
    /* 确保 ROI 不超过新的 max */
    if (dev->roi_w > dev->max_w) dev->roi_w = dev->max_w;
    if (dev->roi_h > dev->max_h) dev->roi_h = dev->max_h;
    return 0;
}

void crop_get_limits(const crop_dev_t *dev, uint32_t *max_w, uint32_t *max_h)
{
    *max_w = dev->max_w;
    *max_h = dev->max_h;
}

void crop_set_roi(crop_dev_t *dev, uint32_t x, uint32_t y,
                  uint32_t w, uint32_t h)
{
    dev->roi_x = x;
    dev->roi_y = y;
    /* clamp to limits */
    dev->roi_w = (w > dev->max_w) ? dev->max_w : w;
    dev->roi_h = (h > dev->max_h) ? dev->max_h : h;
    /* alignment */
    if (dev->roi_w > 0 && dev->align_w > 1)
        dev->roi_w -= dev->roi_w % dev->align_w;
    if (dev->roi_h > 0 && dev->align_h > 1)
        dev->roi_h -= dev->roi_h % dev->align_h;
    if (dev->roi_w == 0) dev->roi_w = dev->align_w;
    if (dev->roi_h == 0) dev->roi_h = dev->align_h;
}

void crop_get_roi(const crop_dev_t *dev, uint32_t *x, uint32_t *y,
                  uint32_t *w, uint32_t *h)
{
    *x = dev->roi_x;
    *y = dev->roi_y;
    *w = dev->roi_w;
    *h = dev->roi_h;
}

void crop_enable(crop_dev_t *dev)
{
    reg_write(dev->base_addr, CROP_REG_CTRL, 0x01);
}

void crop_disable(crop_dev_t *dev)
{
    reg_write(dev->base_addr, CROP_REG_CTRL, 0x00);
}

void crop_commit(const crop_dev_t *dev)
{
    reg_write(dev->base_addr, CROP_REG_ROI_X, dev->roi_x);
    reg_write(dev->base_addr, CROP_REG_ROI_Y, dev->roi_y);
    reg_write(dev->base_addr, CROP_REG_ROI_W, dev->roi_w);
    reg_write(dev->base_addr, CROP_REG_ROI_H, dev->roi_h);
    /* 如果之前 enable 了，commit 后保持 enable */
}
