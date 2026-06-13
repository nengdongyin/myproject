/**
 * @file    isc_sensor_smartsens.c
 * @brief   斯特威 (SmartSens) 系列传感器驱动骨架
 *
 * 目标型号: SC031GS, SC132GS 等.
 * 特征: 全局快门, SPI 控制, 部分型号支持片上 AE.
 */

#include "isc_internal.h"

/* ──── 内部 ──── */

static int smartsens_probe(isc_dev_t *dev)
{
    /** TODO: 读 CHIP_ID 寄存器 (SPI 读操作) */
    (void)dev;
    return ISC_ERR_NOT_SUPPORTED;
}

static int smartsens_init(isc_dev_t *dev)
{
    /** TODO: 上电时序 + 初始寄存器序列 (通过 SPI 写入) */
    (void)dev;
    return ISC_ERR_NOT_SUPPORTED;
}

static int smartsens_deinit(isc_dev_t *dev)
{
    /** TODO: 下电 */
    (void)dev;
    return ISC_ERR_NOT_SUPPORTED;
}

static int smartsens_enum_fmts(isc_dev_t *dev, uint8_t index,
                               isc_fmt_desc_t *desc)
{
    (void)dev; (void)index; (void)desc;
    if (index > 0) return ISC_ENUM_END;
    return ISC_ERR_NOT_SUPPORTED;
}

static int smartsens_get_fmt(isc_dev_t *dev, isc_fmt_t *fmt)
{
    (void)dev; (void)fmt;
    return ISC_ERR_NOT_SUPPORTED;
}

static int smartsens_set_fmt(isc_dev_t *dev, const isc_fmt_t *fmt)
{
    (void)dev; (void)fmt;
    return ISC_ERR_NOT_SUPPORTED;
}

static int smartsens_try_fmt(isc_dev_t *dev, isc_fmt_t *fmt)
{
    (void)dev; (void)fmt;
    return ISC_ERR_NOT_SUPPORTED;
}

static int smartsens_query_ctrl(isc_dev_t *dev, isc_ctrl_desc_t *desc)
{
    (void)dev; (void)desc;
    return ISC_ERR_NOT_SUPPORTED;
}

static int smartsens_get_ctrl(isc_dev_t *dev, uint32_t cid,
                              isc_ctrl_value_t *val)
{
    /** TODO: 读 SPI 寄存器, 转换为统一单位 */
    (void)dev; (void)cid; (void)val;
    return ISC_ERR_NOT_SUPPORTED;
}

static int smartsens_set_ctrl(isc_dev_t *dev, uint32_t cid,
                              isc_ctrl_value_t val)
{
    /** TODO: 统一单位 → SPI 寄存器写入 */
    (void)dev; (void)cid; (void)val;
    return ISC_ERR_NOT_SUPPORTED;
}

static int smartsens_stream_on(isc_dev_t *dev)
{
    (void)dev;
    return ISC_ERR_NOT_SUPPORTED;
}

static int smartsens_stream_off(isc_dev_t *dev)
{
    (void)dev;
    return ISC_ERR_NOT_SUPPORTED;
}

static int smartsens_query_timing(isc_dev_t *dev, isc_timing_t *timing)
{
    /** TODO: 读 PLL/行/帧长度寄存器 → 填充 timing */
    (void)dev; (void)timing;
    return ISC_ERR_NOT_SUPPORTED;
}

/* ──── 驱动注册 ──── */
const isc_sensor_ops_t isc_sensor_smartsens = {
    .model        = "smartsens_sc031gs",
    .vendor       = "SmartSens",
    .capabilities = 0,  /* TODO: 填充 ISC_CAP_* 位掩码 */

    .probe      = smartsens_probe,
    .init       = smartsens_init,
    .deinit     = smartsens_deinit,
    .reset      = NULL,

    .enum_fmts  = smartsens_enum_fmts,
    .get_fmt    = smartsens_get_fmt,
    .set_fmt    = smartsens_set_fmt,
    .try_fmt    = smartsens_try_fmt,

    .query_ctrl = smartsens_query_ctrl,
    .get_ctrl   = smartsens_get_ctrl,
    .set_ctrl   = smartsens_set_ctrl,
    .query_menu = NULL,

    .stream_on  = smartsens_stream_on,
    .stream_off = smartsens_stream_off,

    .query_timing     = smartsens_query_timing,
    .try_timing       = NULL,
    .query_constraint = NULL,
    .sensor_ioctl     = NULL,
};
