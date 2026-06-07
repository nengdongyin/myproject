/**
 * @file    crop_driver.h
 * @brief   Crop / ROI IP 纯硬件驱动 — 零框架依赖。
 *
 * 不引用 vsc_types.h、param_manager.h 或任何 VSC 内部头文件。
 * 可被 VSC 适配器、参数管理器、裸机测试代码独立复用。
 *
 * 寄存器映射：
 *   CROP_CTRL   (base + 0x00)  : bit0 = enable
 *   CROP_ROI_X  (base + 0x04)
 *   CROP_ROI_Y  (base + 0x08)
 *   CROP_ROI_W  (base + 0x0C)
 *   CROP_ROI_H  (base + 0x10)
 */

#ifndef CROP_DRIVER_H
#define CROP_DRIVER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════
 *  设备结构体（由调用方分配内存，S0 兼容）
 * ═══════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t base_addr;       /* FPGA 寄存器基地址               */
    uint32_t max_w;           /* 硬件最大裁剪宽度                */
    uint32_t max_h;           /* 硬件最大裁剪高度                */
    uint32_t roi_x;           /* 当前 ROI 起始 X                 */
    uint32_t roi_y;           /* 当前 ROI 起始 Y                 */
    uint32_t roi_w;           /* 当前 ROI 宽度                   */
    uint32_t roi_h;           /* 当前 ROI 高度                   */
    uint8_t  align_w;         /* 宽度对齐约束                    */
    uint8_t  align_h;         /* 高度对齐约束                    */
} crop_dev_t;

/* ═══════════════════════════════════════════════════════════════════════
 *  API
 * ═══════════════════════════════════════════════════════════════════════ */

/** @brief 初始化设备结构体（不写寄存器） */
void crop_init(crop_dev_t *dev, uint32_t base_addr);

/** @brief 设置硬件能力边界（只能收紧，用于 Instance override） */
int  crop_set_limits(crop_dev_t *dev, uint32_t max_w, uint32_t max_h);
void crop_get_limits(const crop_dev_t *dev, uint32_t *max_w, uint32_t *max_h);

/** @brief 设置 / 获取 ROI */
void crop_set_roi(crop_dev_t *dev, uint32_t x, uint32_t y,
                  uint32_t w, uint32_t h);
void crop_get_roi(const crop_dev_t *dev, uint32_t *x, uint32_t *y,
                  uint32_t *w, uint32_t *h);

/** @brief 使能 / 禁用（写寄存器） */
void crop_enable(crop_dev_t *dev);
void crop_disable(crop_dev_t *dev);

/** @brief 将当前配置写入硬件寄存器 */
void crop_commit(const crop_dev_t *dev);

#ifdef __cplusplus
}
#endif

#endif /* CROP_DRIVER_H */
