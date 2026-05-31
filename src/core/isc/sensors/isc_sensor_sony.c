/**
 * @file    isc_sensor_sony.c
 * @brief   Sony IMX 系列传感器驱动骨架
 *
 * 目标型号: IMX477, IMX290, IMX462 等.
 * I2C 控制, LVDS 输出, 支持 ROI+binning, trigger_set 协调.
 */

#include "../isc_internal.h"

/* ──── 内部 ──── */

static int sony_probe(isc_dev_t *dev)
{
    /** TODO: 读 CHIP_ID 寄存器 (I2C addr=dev->ops->i2c_addr, reg=0x0016) */
    /** TODO: 确认型号, 如: IMX477→0x0477, IMX290→0x0290 */
    (void)dev;
    return ISC_ERR_NOT_SUPPORTED;
}

static int sony_init(isc_dev_t *dev)
{
    /** TODO: 上电时序 (PWDN=1→delay 1ms→PWDN=0→delay 10ms→XCLR=1→delay 5ms) */
    /** TODO: 写入初始寄存器表 (PLL 配置, IO pad, 时钟分频…) */
    (void)dev;
    return ISC_ERR_NOT_SUPPORTED;
}

static int sony_deinit(isc_dev_t *dev)
{
    /** TODO: XCLR=0, PWDN=1 */
    (void)dev;
    return ISC_ERR_NOT_SUPPORTED;
}

static int sony_enum_fmts(isc_dev_t *dev, uint8_t index, isc_fmt_desc_t *desc)
{
    /** TODO: 按 index 填充 desc (Bayer10, Bayer8, Grey10…) */
    (void)dev; (void)index; (void)desc;
    if (index > 0) return ISC_ERR_NO_MORE;
    return ISC_ERR_NOT_SUPPORTED;
}

static int sony_get_fmt(isc_dev_t *dev, isc_fmt_t *fmt)
{
    /** TODO: 从 dev->current_fmt 或读回寄存器获取当前格式 */
    (void)dev; (void)fmt;
    return ISC_ERR_NOT_SUPPORTED;
}

static int sony_set_fmt(isc_dev_t *dev, const isc_fmt_t *fmt)
{
    /** TODO: 写 crop 寄存器 (H_OFFSET, V_OFFSET, H_SIZE, V_SIZE) */
    /** TODO: 写 binning/skip 寄存器 (根据 fmt->reduction) */
    /** TODO: 写帧率相关寄存器 (PLL 重配) */
    (void)dev; (void)fmt;
    return ISC_ERR_NOT_SUPPORTED;
}

static int sony_try_fmt(isc_dev_t *dev, isc_fmt_t *fmt)
{
    /** TODO: 校验并修正 crop 步进, min/max, reduction 组合 */
    /** TODO: 若 reduction 与 crop 互斥, 修正 reduction→NONE */
    (void)dev; (void)fmt;
    return ISC_ERR_NOT_SUPPORTED;
}

static int sony_query_ctrl(isc_dev_t *dev, isc_ctrl_desc_t *desc)
{
    /** TODO: 按 desc->cid 填充控制项属性 */
    /** TODO: 若 cid 含 NEXT_CTRL, 返回下一个控制项 */
    (void)dev; (void)desc;
    return ISC_ERR_NOT_SUPPORTED;
}

static int sony_get_ctrl(isc_dev_t *dev, uint32_t cid, isc_ctrl_value_t *val)
{
    /** TODO: 读对应寄存器, 转换为统一单位:
     *   EXPOSURE:    val->i64 = read_exposure_reg() → ns 换算
     *   ANALOG_GAIN: val->f   = read_gain_reg() / 1024.0f
     *   FRAME_RATE:  val->f   = pixel_clock / (line_length * frame_length)
     */
    (void)dev; (void)cid; (void)val;
    return ISC_ERR_NOT_SUPPORTED;
}

static int sony_set_ctrl(isc_dev_t *dev, uint32_t cid, isc_ctrl_value_t val)
{
    /** TODO: 转换统一单位→寄存器值:
     *   EXPOSURE:    reg = val.i64 / line_period_ns
     *   ANALOG_GAIN: reg = (uint16_t)(val.f * 1024.0f)
     *   FRAME_RATE:  若触及 PLL 重配 → 先关 FPGA 触发:
     *     fpga_ops->ioctl(ISC_FPGA_TRIGGER_SET, &en0) → 写 PLL → trigger_set en1
     */
    (void)dev; (void)cid; (void)val;
    return ISC_ERR_NOT_SUPPORTED;
}

static int sony_stream_on(isc_dev_t *dev)
{
    /** TODO: 写 MODE_SEL 寄存器 (streaming=1) */
    (void)dev;
    return ISC_ERR_NOT_SUPPORTED;
}

static int sony_stream_off(isc_dev_t *dev)
{
    /** TODO: 写 MODE_SEL 寄存器 (streaming=0) */
    (void)dev;
    return ISC_ERR_NOT_SUPPORTED;
}

static int sony_query_timing(isc_dev_t *dev, isc_timing_t *timing)
{
    /** TODO: 读 PLL 配置 → pixel_clock_hz */
    /** TODO: 读 LINE_LENGTH / FRAME_LENGTH 寄存器 */
    /** TODO: 读曝光寄存器值 → exposure_lines */
    /** TODO: 根据当前 crop 计算 readout_lines */
    (void)dev; (void)timing;
    return ISC_ERR_NOT_SUPPORTED;
}

/* ──── 驱动注册 ──── */
const isc_sensor_ops_t isc_sensor_sony = {
    .model    = "sony_imx477",
    .vendor   = "Sony",
    .i2c_addr = 0x1A,

    .probe      = sony_probe,
    .init       = sony_init,
    .deinit     = sony_deinit,
    .reset      = NULL,

    .enum_fmts  = sony_enum_fmts,
    .get_fmt    = sony_get_fmt,
    .set_fmt    = sony_set_fmt,
    .try_fmt    = sony_try_fmt,

    .query_ctrl = sony_query_ctrl,
    .get_ctrl   = sony_get_ctrl,
    .set_ctrl   = sony_set_ctrl,
    .query_menu = NULL,

    .stream_on  = sony_stream_on,
    .stream_off = sony_stream_off,

    .query_timing     = sony_query_timing,
    .query_constraint = NULL,
    .sensor_ioctl     = NULL,
};
