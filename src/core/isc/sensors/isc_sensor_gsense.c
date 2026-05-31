/**
 * @file    isc_sensor_gsense.c
 * @brief   长光晨芯 (Gsense) 系列传感器驱动骨架
 *
 * 目标型号: GSENSE2020, GSENSE4040 等.
 * 特征: 曝光-读出交叠危险区域需应用显式躲避, 曝光按时钟颗粒计数.
 */

#include "../isc_internal.h"

/* ──── GSENSE 私有约束 ──── */
#define GSENSE_CONSTRAINT_DANGER_ZONE  (ISC_CONSTRAINT_PRIVATE_BASE + 0)

typedef struct {
    uint32_t start_line;
    uint32_t end_line;
    uint32_t start_column;
    uint32_t end_column;
    const char *description;
} gsense_danger_zone_t;

/* ──── 内部 ──── */

static int gsense_probe(isc_dev_t *dev)
{
    /** TODO: 读 CHIP_ID 寄存器确认型号 */
    (void)dev;
    return ISC_ERR_NOT_SUPPORTED;
}

static int gsense_init(isc_dev_t *dev)
{
    /** TODO: 上电时序 + 初始寄存器 (时钟树, IO 配置, 默认模式) */
    (void)dev;
    return ISC_ERR_NOT_SUPPORTED;
}

static int gsense_deinit(isc_dev_t *dev)
{
    /** TODO: 下电 */
    (void)dev;
    return ISC_ERR_NOT_SUPPORTED;
}

static int gsense_enum_fmts(isc_dev_t *dev, uint8_t index, isc_fmt_desc_t *desc)
{
    (void)dev; (void)index; (void)desc;
    if (index > 0) return ISC_ERR_NO_MORE;
    return ISC_ERR_NOT_SUPPORTED;
}

static int gsense_get_fmt(isc_dev_t *dev, isc_fmt_t *fmt)
{
    (void)dev; (void)fmt;
    return ISC_ERR_NOT_SUPPORTED;
}

static int gsense_set_fmt(isc_dev_t *dev, const isc_fmt_t *fmt)
{
    /** TODO: 写 crop, binning/skip, 帧率相关寄存器 */
    (void)dev; (void)fmt;
    return ISC_ERR_NOT_SUPPORTED;
}

static int gsense_try_fmt(isc_dev_t *dev, isc_fmt_t *fmt)
{
    /** TODO: 校验 crop 步进, min/max, reduction 组合 */
    (void)dev; (void)fmt;
    return ISC_ERR_NOT_SUPPORTED;
}

static int gsense_query_ctrl(isc_dev_t *dev, isc_ctrl_desc_t *desc)
{
    (void)dev; (void)desc;
    return ISC_ERR_NOT_SUPPORTED;
}

static int gsense_get_ctrl(isc_dev_t *dev, uint32_t cid, isc_ctrl_value_t *val)
{
    /** TODO: 读寄存器, 转换为统一单位.
     *  GSENSE 曝光用时钟颗粒数, 驱动内部完成: ns → ticks 换算.
     */
    (void)dev; (void)cid; (void)val;
    return ISC_ERR_NOT_SUPPORTED;
}

static int gsense_set_ctrl(isc_dev_t *dev, uint32_t cid, isc_ctrl_value_t val)
{
    /**
     * TODO: 统一单位→寄存器值.
     * 关键: 曝光矫正透明化。
     *   val.i64 是应用期望的 ns 值, 驱动内部:
     *     1. 计算 ticks = ns / tick_period
     *     2. 检测 ticks 是否落入当前模式的 danger zone
     *     3. 若落入 → 自动调整 ticks = danger_zone_start - 1
     *     4. 写寄存器
     * 应用无需感知 danger zone 的存在。
     */
    (void)dev; (void)cid; (void)val;
    return ISC_ERR_NOT_SUPPORTED;
}

static int gsense_stream_on(isc_dev_t *dev)
{
    (void)dev;
    return ISC_ERR_NOT_SUPPORTED;
}

static int gsense_stream_off(isc_dev_t *dev)
{
    (void)dev;
    return ISC_ERR_NOT_SUPPORTED;
}

static int gsense_query_timing(isc_dev_t *dev, isc_timing_t *timing)
{
    /** TODO: 读 PLL/行/帧长度/曝光寄存器 */
    (void)dev; (void)timing;
    return ISC_ERR_NOT_SUPPORTED;
}

static int gsense_query_constraint(isc_dev_t *dev, uint32_t type,
                                   uint32_t index, void *data)
{
    /**
     * TODO: 若 type==GSENSE_CONSTRAINT_DANGER_ZONE:
     *   根据当前模式计算危险区域位置, 填充 gsense_danger_zone_t.
     *   支持多条 (不同行区域), index 递增.
     */
    (void)dev; (void)type; (void)index; (void)data;
    return ISC_ERR_NO_MORE;
}

/* ──── 驱动注册 ──── */
const isc_sensor_ops_t isc_sensor_gsense = {
    .model    = "gsense_2020",
    .vendor   = "Gsense",
    .i2c_addr = 0x30,

    .probe      = gsense_probe,
    .init       = gsense_init,
    .deinit     = gsense_deinit,
    .reset      = NULL,

    .enum_fmts  = gsense_enum_fmts,
    .get_fmt    = gsense_get_fmt,
    .set_fmt    = gsense_set_fmt,
    .try_fmt    = gsense_try_fmt,

    .query_ctrl = gsense_query_ctrl,
    .get_ctrl   = gsense_get_ctrl,
    .set_ctrl   = gsense_set_ctrl,
    .query_menu = NULL,

    .stream_on  = gsense_stream_on,
    .stream_off = gsense_stream_off,

    .query_timing     = gsense_query_timing,
    .query_constraint = gsense_query_constraint,
    .sensor_ioctl     = NULL,
};
