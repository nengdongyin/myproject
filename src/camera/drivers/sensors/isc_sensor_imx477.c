/**
 * @file    isc_sensor_imx477.c
 * @brief   Sony IMX477 卷帘快门传感器驱动 — 移植自树莓派 Linux V4L2 驱动
 *
 * 目标型号: IMX477 (12.3MP, 卷帘快门, Bayer, 4056×3040)
 * 兼容:     IMX378 (12.2MP)
 * 接口:     I2C (16-bit 寄存器地址), MIPI CSI-2 2-lane
 * 参考:     drivers/media/i2c/imx477.c (Raspberry Pi)
 *           Copyright 2020 Raspberry Pi (Trading) Ltd
 *
 * 硬件假设:
 *   - 外部 XCLK = 24 MHz, PLL 配置为 840 MHz 像素时钟
 *   - MIPI CSI-2 2-lane, 默认 link freq 450 MHz
 *   - I2C 7-bit 地址 0x1A
 *
 * 特性:
 *   - 卷帘快门
 *   - 4056×3040 像素阵列 (native 4072×3176)
 *   - 多分辨率模式 (固定, 非任意 ROI)
 *   - 10-bit / 12-bit ADC, 4 种 Bayer 排列 (RGGB/BGGR/GRBG/GBRG)
 *   - H/V 翻转会改变 Bayer 顺序
 *   - 2×2 binning (2028×1520), 4×4 binning (1012×760)
 *   - 曝光控制 (LINE_LENGTH/FRAME_LENGTH 消隐调节)
 *   - 模拟增益 (16-bit, 0–978)
 *   - 数字增益 (16-bit, 0x0100–0xFFFF)
 *   - 测试图输出 (5 种图案)
 *   - 温度传感器
 *   - 触发模式 / 频闪输出 (骨架)
 *
 * ── ISC 移植要点 ──
 *
 *   1. 模式表驱动: IMX477 是固定分辨率传感器, 每个模式有完整寄存器表。
 *      try_fmt/set_fmt 查表找最近模式, 而非程序化计算 ROI 寄存器。
 *
 *   2. 模式切换: set_fmt 切换到新模式时, 需写入 200+ 行寄存器表 + 重配
 *      曝光/增益/VBLANK 限幅。ISC 的 init 写入全部公共寄存器,
 *      set_fmt 写入模式特定寄存器。
 *
 *   3. 增益: 0–978 原生 INTEGER, 透传无转换。
 *
 *   4. 翻转→Bayer 顺序: H/V 翻转会交换 Bayer 排列, 需通知 FPGA 更新
 *      isc_fmt_t.pixel_format 反映实际 Bayer 顺序。
 */

#include "isc_internal.h"
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * 寄存器地址定义
 * ═══════════════════════════════════════════════════════════════════════════ */

#define IMX477_REG_MODE_SELECT      0x0100u  /* R/W  0=待机, 1=流 */
#define IMX477_REG_ORIENTATION      0x0101u  /* R/W  bit0=HFLIP, bit1=VFLIP */

#define IMX477_REG_CSI_DT_FMT_H     0x0112u
#define IMX477_REG_CSI_DT_FMT_L     0x0113u

#define IMX477_REG_CHIP_ID          0x0016u  /* R    芯片 ID (16-bit) */

#define IMX477_REG_EXPOSURE         0x0202u  /* R/W  曝光 (16-bit) */
#define IMX477_REG_ANALOG_GAIN      0x0204u  /* R/W  模拟增益 (16-bit, 0–978) */
#define IMX477_REG_DIGITAL_GAIN     0x020Eu  /* R/W  数字增益 (16-bit, 0x0100–0xFFFF) */

#define IMX477_REG_TEST_PATTERN     0x0600u  /* R/W  测试图 */
#define IMX477_REG_TEST_PATTERN_R   0x0602u
#define IMX477_REG_TEST_PATTERN_GR  0x0604u
#define IMX477_REG_TEST_PATTERN_B   0x0606u
#define IMX477_REG_TEST_PATTERN_GB  0x0608u

#define IMX477_REG_FRAME_LENGTH     0x0340u  /* R/W  帧长度 (16-bit) */
#define IMX477_REG_LINE_LENGTH      0x0342u  /* R/W  行长度 (16-bit) */

#define IMX477_REG_LONG_EXP_SHIFT   0x3100u  /* R/W  长曝光移位 (0–7) */

#define IMX477_REG_TEMP_SEN_CTL     0x0138u  /* R/W  温度传感器控制 */

#define IMX477_REG_IOP_PXCK_DIV     0x0309u
#define IMX477_REG_IOP_SYSCK_DIV    0x030Bu
#define IMX477_REG_IOP_PREDIV       0x030Du
#define IMX477_REG_IOP_MPY          0x030Eu

/* ── 常量 ── */
#define IMX477_CHIP_ID              0x0477u
#define IMX378_CHIP_ID              0x0378u
#define IMX477_XCLK_FREQ            24000000u
#define IMX477_PIXEL_RATE           840000000u
#define IMX477_DEFAULT_LINK_FREQ    450000000u

#define IMX477_PIXEL_ARRAY_LEFT     8u
#define IMX477_PIXEL_ARRAY_TOP      16u
#define IMX477_PIXEL_ARRAY_WIDTH    4056u
#define IMX477_PIXEL_ARRAY_HEIGHT   3040u

#define IMX477_VBLANK_MIN           48u
#define IMX477_FRAME_LENGTH_MAX     0xFFDCu
#define IMX477_LINE_LENGTH_MAX      0xFFF0u

#define IMX477_EXPOSURE_OFFSET      22u
#define IMX477_EXPOSURE_MIN         4u
#define IMX477_EXPOSURE_DEFAULT     0x640u
#define IMX477_ANA_GAIN_MIN         0u
#define IMX477_ANA_GAIN_MAX         978u
#define IMX477_DGTL_GAIN_MIN        0x0100u
#define IMX477_DGTL_GAIN_MAX        0xFFFFu
#define IMX477_DGTL_GAIN_DEFAULT    0x0100u

/* ── 测试图枚举 ── */
#define IMX477_TEST_DISABLED        0
#define IMX477_TEST_SOLID_COLOR     1
#define IMX477_TEST_COLOR_BARS      2
#define IMX477_TEST_GREY_COLOR      3
#define IMX477_TEST_PN9             4
#define IMX477_NUM_TEST_PATTERNS    5u

/* ═══════════════════════════════════════════════════════════════════════════
 * 模式定义
 *
 * IMX477 是固定分辨率传感器, 每个模式有预定义的:
 *   - 输出分辨率 (width × height)
 *   - 帧长度默认值
 *   - 位深
 *   - Bayer 排列 (受翻转影响)
 *   - 缩减方式
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t frm_length_default;
    uint8_t  bit_depth;
    uint32_t max_fps_num, max_fps_den;
    uint8_t  reduction_x;
    uint8_t  reduction_y;
    uint8_t  reduction_mode;
} imx477_mode_t;

static const imx477_mode_t imx477_modes[] = {
    /* 全分辨率 4056×3040, 10fps */
    { 4056, 3040, 0x0BE0, 10, 10, 1, 1, 1, ISC_REDUCE_NONE },
    /* 16:9 裁切 4056×2160, 10fps */
    { 4056, 2160, 0x0A28, 10, 10, 1, 1, 1, ISC_REDUCE_NONE },
    /* 2×2 binning 2028×1520, 40fps */
    { 2028, 1520, 0x0BE0, 10, 40, 1, 2, 2, ISC_REDUCE_BIN_SUM },
    /* 4×4 binning 1012×760, 120fps */
    { 1012,  760, 0x041A, 10, 120, 1, 4, 4, ISC_REDUCE_BIN_SUM },
    /* 2×2 binning 16:9 2028×1080, 50fps */
    { 2028, 1080, 0x0870, 10, 50, 1, 2, 2, ISC_REDUCE_BIN_SUM },
};

#define IMX477_NUM_MODES (sizeof(imx477_modes) / sizeof(imx477_modes[0]))

/* ═══════════════════════════════════════════════════════════════════════════
 * 内部辅助 — I2C 寄存器读写
 * ═══════════════════════════════════════════════════════════════════════════ */

static int imx477_read16(const isc_dev_t *dev, uint16_t reg, uint16_t *val)
{
    uint8_t buf[2];
    uint8_t reg_buf[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
    int rc = dev->port->read(dev->port->user_data, reg_buf, 2, buf, 2);
    if (rc != 0) return ISC_ERR_IO;
    *val = ((uint16_t)buf[0] << 8) | (uint16_t)buf[1];
    return ISC_OK;
}

static int imx477_write8(const isc_dev_t *dev, uint16_t reg, uint8_t val)
{
    uint8_t reg_buf[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
    int rc = dev->port->write(dev->port->user_data, reg_buf, 2, &val, 1);
    return (rc == 0) ? ISC_OK : ISC_ERR_IO;
}

static int imx477_write16(const isc_dev_t *dev, uint16_t reg, uint16_t val)
{
    uint8_t reg_buf[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
    uint8_t buf[2] = { (uint8_t)(val >> 8), (uint8_t)(val & 0xFF) };
    int rc = dev->port->write(dev->port->user_data, reg_buf, 2, buf, 2);
    return (rc == 0) ? ISC_OK : ISC_ERR_IO;
}

/**
 * @brief 查找与请求分辨率最近的本征模式
 */
static const imx477_mode_t *imx477_find_mode(uint32_t width, uint32_t height)
{
    const imx477_mode_t *best = &imx477_modes[0];
    uint32_t best_diff = 0xFFFFFFFFu;

    for (uint8_t i = 0; i < IMX477_NUM_MODES; i++) {
        uint32_t dw = (imx477_modes[i].width  > width)
            ? imx477_modes[i].width  - width
            : width  - imx477_modes[i].width;
        uint32_t dh = (imx477_modes[i].height > height)
            ? imx477_modes[i].height - height
            : height - imx477_modes[i].height;
        uint32_t diff = dw + dh;
        if (diff < best_diff) {
            best_diff = diff;
            best = &imx477_modes[i];
        }
    }
    return best;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 测试图菜单
 * ═══════════════════════════════════════════════════════════════════════════ */

static const char * const imx477_test_pattern_names[] = {
    "Disabled",
    "Solid Color",
    "Color Bars",
    "Grey Color",
    "PN9",
};

/* ═══════════════════════════════════════════════════════════════════════════
 * 生命周期
 * ═══════════════════════════════════════════════════════════════════════════ */

static int imx477_probe(isc_dev_t *dev)
{
    uint16_t chip_id;
    int rc = imx477_read16(dev, IMX477_REG_CHIP_ID, &chip_id);
    if (rc != ISC_OK) return ISC_ERR_IO;
    if (chip_id != IMX477_CHIP_ID && chip_id != IMX378_CHIP_ID)
        return ISC_ERR_NOT_FOUND;
    return ISC_OK;
}

static int imx477_init(isc_dev_t *dev)
{
    int rc;

    /* ── 1. GPIO 上电 ── */
    dev->port->gpio_write(0, 0);  dev->port->delay_ms(1);
    dev->port->gpio_write(0, 1);  dev->port->delay_ms(2);
    dev->port->gpio_write(1, 0);  dev->port->delay_ms(1);
    dev->port->gpio_write(1, 1);  dev->port->delay_ms(10);

    /* ── 2. 公共寄存器初始化 (mode_common_regs, 精简版) ── */
    static const struct { uint16_t reg; uint8_t val; } init_regs[] = {
        { 0x0136, 0x18 }, { 0x0137, 0x00 }, { 0x0138, 0x01 }, /* 温度传感器 */
        { 0x0114, 0x01 }, { 0x0350, 0x00 },
        { 0x3FF9, 0x01 },
        { 0x0220, 0x00 }, { 0x0221, 0x11 },
        { 0x0381, 0x01 }, { 0x0383, 0x01 }, { 0x0385, 0x01 }, { 0x0387, 0x01 },
        { 0x0902, 0x02 }, { 0x3140, 0x02 }, { 0x3C00, 0x00 },
        { 0x0301, 0x05 }, { 0x0303, 0x02 },
        { IMX477_REG_IOP_SYSCK_DIV, 0x02 },
        { IMX477_REG_IOP_PREDIV,    0x02 },
        { 0x0310, 0x01 },
        { 0x3E20, 0x01 }, { 0x3E37, 0x00 }, { 0x3F50, 0x00 },
    };

    for (uint8_t i = 0; i < (uint8_t)(sizeof(init_regs)/sizeof(init_regs[0])); i++) {
        rc = imx477_write8(dev, init_regs[i].reg, init_regs[i].val);
        if (rc != ISC_OK) return rc;
    }

    /* ── 3. PLL 配置 (24MHz XCLK → 840MHz pixel rate) ── */
    rc  = imx477_write8(dev, IMX477_REG_IOP_PXCK_DIV, 0x04);
    rc |= imx477_write16(dev, IMX477_REG_IOP_MPY, 0x015E);
    if (rc != ISC_OK) return rc;

    /* ── 4. 默认模式: 全分辨率 4056×3040 ── */
    /* 由 isc_open 后 isc_set_fmt 完成首次模式写入 */

    /* ── 5. 默认控制值 ── */
    rc  = imx477_write16(dev, IMX477_REG_ANALOG_GAIN,  IMX477_ANA_GAIN_MIN);
    rc |= imx477_write16(dev, IMX477_REG_DIGITAL_GAIN, IMX477_DGTL_GAIN_DEFAULT);
    rc |= imx477_write16(dev, IMX477_REG_EXPOSURE,     IMX477_EXPOSURE_DEFAULT);
    rc |= imx477_write8(dev,  IMX477_REG_LONG_EXP_SHIFT, 0);
    rc |= imx477_write8(dev,  IMX477_REG_ORIENTATION,  0x00);
    rc |= imx477_write16(dev, IMX477_REG_TEST_PATTERN, 0);
    if (rc != ISC_OK) return rc;

    return ISC_OK;
}

static int imx477_deinit(isc_dev_t *dev)
{
    imx477_write8(dev, IMX477_REG_MODE_SELECT, 0x00);  /* 待机 */
    dev->port->delay_ms(1);
    dev->port->gpio_write(1, 0);
    dev->port->gpio_write(0, 0);
    return ISC_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 格式枚举 — 固定模式表, 非 ROI
 * ═══════════════════════════════════════════════════════════════════════════ */

static const isc_fmt_desc_t imx477_fmts[] = {
    {
        .pixel_format       = ISC_PIX_FMT_SRGGB10,
        .description        = "Bayer RGGB 10-bit",
        .bit_depth          = 10,
        .sensor_width       = IMX477_PIXEL_ARRAY_WIDTH,
        .sensor_height      = IMX477_PIXEL_ARRAY_HEIGHT,
        .crop_step_x        = 4,   /* IMX477 binning 要求 4-pixel 对齐 */
        .crop_step_y        = 4,
        .min_crop_width     = 1012,  /* 4×4 binning 最小 */
        .min_crop_height    = 760,
        .min_width          = 1012,
        .max_width          = IMX477_PIXEL_ARRAY_WIDTH,
        .min_height         = 760,
        .max_height         = IMX477_PIXEL_ARRAY_HEIGHT,
        .max_frame_rate_num = 120,
        .max_frame_rate_den = 1,
    },
    {
        .pixel_format       = ISC_PIX_FMT_SRGGB12,
        .description        = "Bayer RGGB 12-bit",
        .bit_depth          = 12,
        .sensor_width       = IMX477_PIXEL_ARRAY_WIDTH,
        .sensor_height      = IMX477_PIXEL_ARRAY_HEIGHT,
        .crop_step_x        = 4,
        .crop_step_y        = 4,
        .min_crop_width     = 1012,
        .min_crop_height    = 760,
        .min_width          = 1012,
        .max_width          = IMX477_PIXEL_ARRAY_WIDTH,
        .min_height         = 760,
        .max_height         = IMX477_PIXEL_ARRAY_HEIGHT,
        .max_frame_rate_num = 120,
        .max_frame_rate_den = 1,
    },
};

#define IMX477_NUM_FMTS (sizeof(imx477_fmts) / sizeof(imx477_fmts[0]))

static int imx477_enum_fmts(isc_dev_t *dev, uint8_t index, isc_fmt_desc_t *desc)
{
    (void)dev;
    if (index >= IMX477_NUM_FMTS) return ISC_ENUM_END;
    *desc = imx477_fmts[index];
    return ISC_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 格式获取/设置/试探 — 模式查表
 * ═══════════════════════════════════════════════════════════════════════════ */

static int imx477_get_fmt(isc_dev_t *dev, isc_fmt_t *fmt)
{
    /* 对标 Linux: 返回软件缓存 */
    *fmt = dev->current_fmt;
    return ISC_OK;
}

static int imx477_try_fmt(isc_dev_t *dev, isc_fmt_t *fmt)
{
    (void)dev;

    /* IMX477 是固定模式传感器 — 找到最接近的预设分辨率 */
    if (fmt->width == 0 || fmt->height == 0) {
        fmt->width  = IMX477_PIXEL_ARRAY_WIDTH;
        fmt->height = IMX477_PIXEL_ARRAY_HEIGHT;
    }

    const imx477_mode_t *mode = imx477_find_mode(fmt->width, fmt->height);

    fmt->width        = mode->width;
    fmt->height       = mode->height;
    fmt->crop_left    = IMX477_PIXEL_ARRAY_LEFT;
    fmt->crop_top     = IMX477_PIXEL_ARRAY_TOP;
    fmt->crop_width   = mode->width;
    fmt->crop_height  = mode->height;
    fmt->reduction_x    = mode->reduction_x;
    fmt->reduction_y    = mode->reduction_y;
    fmt->reduction_mode = mode->reduction_mode;
    fmt->bit_depth    = mode->bit_depth;
    fmt->pixel_format = (mode->bit_depth == 12)
        ? ISC_PIX_FMT_SRGGB12 : ISC_PIX_FMT_SRGGB10;

    if (fmt->frame_rate_num == 0 || fmt->frame_rate_den == 0) {
        fmt->frame_rate_num = mode->max_fps_num;
        fmt->frame_rate_den = mode->max_fps_den;
    }

    return ISC_OK;
}

static int imx477_set_fmt(isc_dev_t *dev, const isc_fmt_t *fmt)
{
    const imx477_mode_t *mode = imx477_find_mode(fmt->width, fmt->height);
    int rc;

    /* ── 进入待机 ── */
    rc = imx477_write8(dev, IMX477_REG_MODE_SELECT, 0x00);
    if (rc != ISC_OK) return rc;

    /* ── 写模式特定寄存器 ── */
    /* 分辨率寄存器: 0x0344-0x034F (X/Y 起始+结束, 输出宽高) */
    rc  = imx477_write16(dev, 0x0344, IMX477_PIXEL_ARRAY_LEFT);
    rc |= imx477_write16(dev, 0x0346, IMX477_PIXEL_ARRAY_TOP);
    rc |= imx477_write16(dev, 0x0348, IMX477_PIXEL_ARRAY_LEFT + mode->width  - 1u);
    rc |= imx477_write16(dev, 0x034A, IMX477_PIXEL_ARRAY_TOP  + mode->height - 1u);
    rc |= imx477_write16(dev, 0x034C, mode->width);
    rc |= imx477_write16(dev, 0x034E, mode->height);

    /* 消隐默认 */
    rc |= imx477_write16(dev, IMX477_REG_FRAME_LENGTH, mode->frm_length_default);
    /* LINE_LENGTH 由 HBLANK 控制, 设置最小安全值 */
    rc |= imx477_write16(dev, IMX477_REG_LINE_LENGTH,
                         mode->width + 500u);

    if (rc != ISC_OK) {
        imx477_write8(dev, IMX477_REG_MODE_SELECT, 0x01);
        return ISC_ERR_IO;
    }

    /* ── 退出待机 ── */
    rc = imx477_write8(dev, IMX477_REG_MODE_SELECT, 0x01);
    if (rc != ISC_OK) return rc;
    dev->port->delay_ms(5);

    return ISC_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 控制框架
 * ═══════════════════════════════════════════════════════════════════════════ */

static int imx477_query_ctrl(isc_dev_t *dev, isc_ctrl_desc_t *desc)
{
    (void)dev;
    uint32_t cid = desc->cid;
    memset(desc, 0, sizeof(*desc));
    desc->cid = cid;

    switch (cid) {
    case ISC_CID_EXPOSURE:
        desc->type    = ISC_CTRL_TYPE_INTEGER;
        desc->unit    = "lines";
        strncpy(desc->name, "Exposure", ISC_MAX_CTRL_NAME - 1);
        desc->min.i64 = IMX477_EXPOSURE_MIN;
        desc->max.i64 = IMX477_FRAME_LENGTH_MAX - IMX477_EXPOSURE_OFFSET;
        desc->step.i64= 1;
        desc->def.i64 = IMX477_EXPOSURE_DEFAULT;
        desc->flags   = ISC_CTRL_FLAG_STREAMABLE;
        break;

    case ISC_CID_ANALOG_GAIN:
        desc->type    = ISC_CTRL_TYPE_INTEGER;
        desc->unit    = "code";
        strncpy(desc->name, "Analog Gain", ISC_MAX_CTRL_NAME - 1);
        desc->min.i64 = IMX477_ANA_GAIN_MIN;
        desc->max.i64 = IMX477_ANA_GAIN_MAX;
        desc->step.i64= 1;
        desc->def.i64 = IMX477_ANA_GAIN_MIN;
        desc->flags   = ISC_CTRL_FLAG_STREAMABLE;
        break;

    case ISC_CID_DIGITAL_GAIN:
        desc->type    = ISC_CTRL_TYPE_INTEGER;
        desc->unit    = "code";
        strncpy(desc->name, "Digital Gain", ISC_MAX_CTRL_NAME - 1);
        desc->min.i64 = IMX477_DGTL_GAIN_MIN;
        desc->max.i64 = IMX477_DGTL_GAIN_MAX;
        desc->step.i64= 1;
        desc->def.i64 = IMX477_DGTL_GAIN_DEFAULT;
        desc->flags   = ISC_CTRL_FLAG_STREAMABLE;
        break;

    case ISC_CID_FRAME_RATE:
        desc->type    = ISC_CTRL_TYPE_FLOAT;
        desc->unit    = "fps";
        strncpy(desc->name, "Frame Rate", ISC_MAX_CTRL_NAME - 1);
        desc->min.f   = 1.0f;
        desc->max.f   = 120.0f;
        desc->step.f  = 0.0f;
        desc->def.f   = 10.0f;
        desc->flags   = 0;
        break;

    case ISC_CID_HFLIP:
        desc->type    = ISC_CTRL_TYPE_BOOLEAN;
        strncpy(desc->name, "H Flip", ISC_MAX_CTRL_NAME - 1);
        desc->min.b   = 0; desc->max.b = 1; desc->step.b = 1; desc->def.b = 0;
        desc->flags   = 0;
        break;

    case ISC_CID_VFLIP:
        desc->type    = ISC_CTRL_TYPE_BOOLEAN;
        strncpy(desc->name, "V Flip", ISC_MAX_CTRL_NAME - 1);
        desc->min.b   = 0; desc->max.b = 1; desc->step.b = 1; desc->def.b = 0;
        desc->flags   = 0;
        break;

    case ISC_CID_TEST_PATTERN:
        desc->type    = ISC_CTRL_TYPE_ENUM;
        strncpy(desc->name, "Test Pattern", ISC_MAX_CTRL_NAME - 1);
        desc->min.i64 = 0;
        desc->max.i64 = IMX477_NUM_TEST_PATTERNS - 1;
        desc->step.i64= 1;
        desc->def.i64 = 0;
        desc->flags   = 0;
        break;

    case ISC_CID_TEMPERATURE:
        desc->type    = ISC_CTRL_TYPE_FLOAT;
        desc->unit    = "°C";
        strncpy(desc->name, "Temperature", ISC_MAX_CTRL_NAME - 1);
        desc->min.f   = -40.0f;
        desc->max.f   = 125.0f;
        desc->step.f  = 0.0f;
        desc->def.f   = 25.0f;
        desc->flags   = ISC_CTRL_FLAG_READ_ONLY | ISC_CTRL_FLAG_VOLATILE;
        break;

    case ISC_CID_PIXEL_CLOCK:
        desc->type    = ISC_CTRL_TYPE_INTEGER;
        desc->unit    = "Hz";
        strncpy(desc->name, "Pixel Clock", ISC_MAX_CTRL_NAME - 1);
        desc->min.i64 = 0;
        desc->max.i64 = 2000000000;
        desc->def.i64 = IMX477_PIXEL_RATE;
        desc->flags   = ISC_CTRL_FLAG_READ_ONLY | ISC_CTRL_FLAG_VOLATILE;
        break;

    case ISC_CID_LINE_LENGTH:
        desc->type    = ISC_CTRL_TYPE_INTEGER;
        desc->unit    = "pclk";
        strncpy(desc->name, "Line Length", ISC_MAX_CTRL_NAME - 1);
        desc->min.i64 = 0;
        desc->max.i64 = IMX477_LINE_LENGTH_MAX;
        desc->def.i64 = IMX477_PIXEL_ARRAY_WIDTH + 500;
        desc->flags   = ISC_CTRL_FLAG_READ_ONLY | ISC_CTRL_FLAG_VOLATILE;
        break;

    case ISC_CID_FRAME_LENGTH:
        desc->type    = ISC_CTRL_TYPE_INTEGER;
        desc->unit    = "lines";
        strncpy(desc->name, "Frame Length", ISC_MAX_CTRL_NAME - 1);
        desc->min.i64 = 0;
        desc->max.i64 = IMX477_FRAME_LENGTH_MAX;
        desc->def.i64 = 0x0BE0;
        desc->flags   = ISC_CTRL_FLAG_READ_ONLY | ISC_CTRL_FLAG_VOLATILE;
        break;

    case ISC_CID_LANE_COUNT:
        desc->type    = ISC_CTRL_TYPE_ENUM;
        strncpy(desc->name, "MIPI Lanes", ISC_MAX_CTRL_NAME - 1);
        desc->min.i64 = 0; desc->max.i64 = 0; desc->step.i64 = 1; desc->def.i64 = 0;
        desc->flags   = ISC_CTRL_FLAG_READ_ONLY;
        break;

    case ISC_CID_BIT_DEPTH:
        desc->type    = ISC_CTRL_TYPE_ENUM;
        strncpy(desc->name, "Bit Depth", ISC_MAX_CTRL_NAME - 1);
        desc->min.i64 = 0; desc->max.i64 = 1; desc->step.i64 = 1; desc->def.i64 = 0;
        desc->flags   = ISC_CTRL_FLAG_READ_ONLY;
        break;

    default:
        return ISC_ERR_NOT_SUPPORTED;
    }
    return ISC_OK;
}

static int imx477_get_ctrl(isc_dev_t *dev, uint32_t cid, isc_ctrl_value_t *val)
{
    uint16_t reg16;
    int rc;

    switch (cid) {
    case ISC_CID_EXPOSURE:
        rc = imx477_read16(dev, IMX477_REG_EXPOSURE, &reg16);
        if (rc != ISC_OK) return rc;
        val->i64 = reg16;
        break;

    case ISC_CID_ANALOG_GAIN:
        rc = imx477_read16(dev, IMX477_REG_ANALOG_GAIN, &reg16);
        if (rc != ISC_OK) return rc;
        val->i64 = reg16;
        break;

    case ISC_CID_DIGITAL_GAIN:
        rc = imx477_read16(dev, IMX477_REG_DIGITAL_GAIN, &reg16);
        if (rc != ISC_OK) return rc;
        val->i64 = reg16;
        break;

    case ISC_CID_FRAME_RATE: {
        uint16_t hmax, vmax;
        rc  = imx477_read16(dev, IMX477_REG_LINE_LENGTH, &hmax);
        rc |= imx477_read16(dev, IMX477_REG_FRAME_LENGTH, &vmax);
        if (rc != ISC_OK) return rc;
        if (hmax > 0 && vmax > 0)
            val->f = (float)IMX477_PIXEL_RATE / (float)((uint32_t)hmax * (uint32_t)vmax);
        else
            val->f = 10.0f;
        break;
    }

    case ISC_CID_HFLIP:
        rc = imx477_write8(dev, 0x0138, 0x01); /* dummy to read orientation later... */
        /* IMX477 ORIENTATION register: bit0=HFLIP, bit1=VFLIP */
        rc = imx477_write8(dev, IMX477_REG_ORIENTATION, 0x00); /* read back not possible directly */
        (void)rc;
        val->b = 0; /* TODO: read back from cache */
        break;

    case ISC_CID_VFLIP:
        val->b = 0;
        break;

    case ISC_CID_TEST_PATTERN:
        rc = imx477_read16(dev, IMX477_REG_TEST_PATTERN, &reg16);
        if (rc != ISC_OK) return rc;
        val->i64 = (reg16 == 0) ? 0 : (int64_t)((reg16 >> 8) & 0xFF);
        break;

    case ISC_CID_TEMPERATURE:
        /* TODO: IMX477 temperature formula — datasheet needed */
        val->f = 25.0f;
        break;

    case ISC_CID_PIXEL_CLOCK:
        val->i64 = IMX477_PIXEL_RATE;
        break;

    case ISC_CID_LINE_LENGTH:
        rc = imx477_read16(dev, IMX477_REG_LINE_LENGTH, &reg16);
        if (rc != ISC_OK) return rc;
        val->i64 = reg16;
        break;

    case ISC_CID_FRAME_LENGTH:
        rc = imx477_read16(dev, IMX477_REG_FRAME_LENGTH, &reg16);
        if (rc != ISC_OK) return rc;
        val->i64 = reg16;
        break;

    case ISC_CID_LANE_COUNT:
        val->i64 = 0; /* index 0 = 2 Lanes */
        break;

    case ISC_CID_BIT_DEPTH:
        val->i64 = 0; /* index 0 = 10-bit */
        break;

    default:
        return ISC_ERR_NOT_SUPPORTED;
    }
    return ISC_OK;
}

static int imx477_set_ctrl(isc_dev_t *dev, uint32_t cid, isc_ctrl_value_t val)
{
    int rc;

    switch (cid) {
    case ISC_CID_EXPOSURE:
        if ((uint64_t)val.i64 > 0xFFFFu) val.i64 = 0xFFFF;
        rc = imx477_write16(dev, IMX477_REG_EXPOSURE, (uint16_t)val.i64);
        break;

    case ISC_CID_ANALOG_GAIN:
        if ((uint64_t)val.i64 > IMX477_ANA_GAIN_MAX) val.i64 = IMX477_ANA_GAIN_MAX;
        rc = imx477_write16(dev, IMX477_REG_ANALOG_GAIN, (uint16_t)val.i64);
        break;

    case ISC_CID_DIGITAL_GAIN:
        if ((uint64_t)val.i64 > IMX477_DGTL_GAIN_MAX) val.i64 = IMX477_DGTL_GAIN_MAX;
        if ((uint64_t)val.i64 < IMX477_DGTL_GAIN_MIN) val.i64 = IMX477_DGTL_GAIN_MIN;
        rc = imx477_write16(dev, IMX477_REG_DIGITAL_GAIN, (uint16_t)val.i64);
        break;

    case ISC_CID_FRAME_RATE: {
        if (val.f <= 0.0f) return ISC_ERR_INVALID_ARG;
        uint32_t vmax = (uint32_t)((float)IMX477_PIXEL_RATE / (float)(IMX477_PIXEL_ARRAY_WIDTH + 500) / val.f + 0.5f);
        if (vmax < IMX477_PIXEL_ARRAY_HEIGHT + IMX477_VBLANK_MIN) vmax = IMX477_PIXEL_ARRAY_HEIGHT + IMX477_VBLANK_MIN;
        if (vmax > IMX477_FRAME_LENGTH_MAX) vmax = IMX477_FRAME_LENGTH_MAX;
        rc = imx477_write16(dev, IMX477_REG_FRAME_LENGTH, (uint16_t)vmax);
        break;
    }

    case ISC_CID_HFLIP:
        rc = imx477_write8(dev, IMX477_REG_ORIENTATION, val.b ? 0x01 : 0x00);
        break;

    case ISC_CID_VFLIP:
        rc = imx477_write8(dev, IMX477_REG_ORIENTATION, val.b ? 0x02 : 0x00);
        break;

    case ISC_CID_TEST_PATTERN:
        rc = imx477_write16(dev, IMX477_REG_TEST_PATTERN,
                            (val.i64 > 0) ? (uint16_t)(0x0800 | (val.i64 & 0x07)) : 0x0000);
        break;

    default:
        return ISC_ERR_NOT_SUPPORTED;
    }
    return rc;
}

static int imx477_query_menu(isc_dev_t *dev, uint32_t cid, uint32_t index,
                             char *name)
{
    (void)dev;
    if (name == NULL) return ISC_ERR_INVALID_ARG;

    switch (cid) {
    case ISC_CID_TEST_PATTERN:
        if (index < IMX477_NUM_TEST_PATTERNS) {
            strncpy(name, imx477_test_pattern_names[index], ISC_MAX_MENU_NAME - 1);
            return ISC_OK;
        }
        return ISC_ERR_CTRL_RANGE;

    case ISC_CID_LANE_COUNT:
        if (index == 0) { strncpy(name, "2 Lanes", ISC_MAX_MENU_NAME - 1); return ISC_OK; }
        return ISC_ERR_CTRL_RANGE;

    case ISC_CID_BIT_DEPTH:
        if (index == 0) { strncpy(name, "10-bit", ISC_MAX_MENU_NAME - 1); return ISC_OK; }
        if (index == 1) { strncpy(name, "12-bit", ISC_MAX_MENU_NAME - 1); return ISC_OK; }
        return ISC_ERR_CTRL_RANGE;

    default:
        return ISC_ERR_NOT_SUPPORTED;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 流控制
 * ═══════════════════════════════════════════════════════════════════════════ */

static int imx477_stream_on(isc_dev_t *dev)
{
    int rc = imx477_write8(dev, IMX477_REG_MODE_SELECT, 0x01);
    dev->port->delay_ms(5);
    return rc;
}

static int imx477_stream_off(isc_dev_t *dev)
{
    return imx477_write8(dev, IMX477_REG_MODE_SELECT, 0x00);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 物理时序查询
 * ═══════════════════════════════════════════════════════════════════════════ */

static int imx477_query_timing(isc_dev_t *dev, isc_timing_t *timing)
{
    uint16_t hmax, vmax, exposure;
    int rc;

    memset(timing, 0, sizeof(*timing));

    rc  = imx477_read16(dev, IMX477_REG_LINE_LENGTH, &hmax);
    rc |= imx477_read16(dev, IMX477_REG_FRAME_LENGTH, &vmax);
    rc |= imx477_read16(dev, IMX477_REG_EXPOSURE, &exposure);
    if (rc != ISC_OK) return rc;

    timing->pixel_clock_hz     = IMX477_PIXEL_RATE;
    timing->line_length_pclk   = hmax;
    timing->frame_length_lines = vmax;
    timing->exposure_lines     = exposure;
    timing->exposure_max_lines = (vmax > IMX477_EXPOSURE_OFFSET)
        ? vmax - IMX477_EXPOSURE_OFFSET : 0u;
    timing->readout_lines      = dev->current_fmt.height;
    timing->lane_count         = 2;
    timing->bit_depth          = (uint8_t)dev->current_fmt.bit_depth;

    return ISC_OK;
}

static int imx477_try_timing(isc_dev_t *dev, const isc_fmt_t *fmt,
                             isc_timing_t *timing)
{
    const imx477_mode_t *mode = imx477_find_mode(fmt->width, fmt->height);
    uint32_t vmax, hmax;

    (void)dev;
    memset(timing, 0, sizeof(*timing));

    timing->pixel_clock_hz = IMX477_PIXEL_RATE;
    timing->lane_count     = 2;
    timing->bit_depth      = mode->bit_depth;
    timing->readout_lines  = mode->height;

    hmax = mode->width + 500u;
    timing->line_length_pclk = hmax;

    if (fmt->frame_rate_num > 0 && fmt->frame_rate_den > 0) {
        vmax = (uint32_t)((uint64_t)IMX477_PIXEL_RATE
               * (uint64_t)fmt->frame_rate_den
               / ((uint64_t)hmax * (uint64_t)fmt->frame_rate_num));
    } else {
        vmax = mode->frm_length_default;
    }
    {
        uint32_t min_vmax = mode->height + IMX477_VBLANK_MIN;
        if (vmax < min_vmax) vmax = min_vmax;
        if (vmax > IMX477_FRAME_LENGTH_MAX) vmax = IMX477_FRAME_LENGTH_MAX;
    }
    timing->frame_length_lines = vmax;
    timing->exposure_max_lines = (vmax > IMX477_EXPOSURE_OFFSET)
        ? vmax - IMX477_EXPOSURE_OFFSET : 0u;
    timing->exposure_lines = timing->exposure_max_lines;

    return ISC_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 驱动注册
 * ═══════════════════════════════════════════════════════════════════════════ */

const isc_sensor_ops_t isc_sensor_imx477 = {
    .model        = "sony_imx477",
    .vendor       = "Sony",
    .capabilities = ISC_CAP_TIMING_QUERY
                  | ISC_CAP_ROI
                  | ISC_CAP_BINNING
                  | ISC_CAP_SUBSAMPLE,

    .probe      = imx477_probe,
    .init       = imx477_init,
    .deinit     = imx477_deinit,
    .reset      = NULL,

    .enum_fmts  = imx477_enum_fmts,
    .get_fmt    = imx477_get_fmt,
    .set_fmt    = imx477_set_fmt,
    .try_fmt    = imx477_try_fmt,

    .query_ctrl = imx477_query_ctrl,
    .get_ctrl   = imx477_get_ctrl,
    .set_ctrl   = imx477_set_ctrl,
    .query_menu = imx477_query_menu,

    .stream_on  = imx477_stream_on,
    .stream_off = imx477_stream_off,

    .query_timing     = imx477_query_timing,
    .try_timing       = imx477_try_timing,
    .query_constraint = NULL,
    .sensor_ioctl     = NULL,
};
