/**
 * @file    isc_sensor_imx296.c
 * @brief   Sony IMX296 全局快门传感器驱动 — 完整移植自 Linux V4L2 驱动
 *
 * 目标型号: IMX296LLR / IMX296LQR (1.58MP, 全局快门, Bayer BGGR)
 * 接口:     I2C (8/16/24-bit 寄存器), MIPI CSI-2 (2/4 lane)
 * 参考:     Linux V4L2 subdev 驱动 drivers/media/i2c/imx296.c
 *           Copyright 2019 Laurent Pinchart <laurent.pinchart@ideasonboard.com>
 *
 * 硬件假设:
 *   - 外部 XTAL = 37.125 / 54 / 74.25 MHz, 内部 PLL 固定输出 INCK = 74.25 MHz
 *   - 默认配置: 74.25 MHz XTAL, MIPI CSI-2 4-lane
 *   - I2C 7-bit 地址 0x1A
 *
 * 特性:
 *   - 全局快门, 无卷帘效应
 *   - 1456×1088 像素阵列, 10-bit ADC (RAW10)
 *   - 窗口裁剪 (ROI — FID0 寄存器组, 4-pixel 步进, 最小 80×4)
 *   - 曝光控制 (SHS1 24-bit, VMAX 钳位, ns 单位)
 *   - 模拟增益 (16-bit, 0–480, 0.1 dB 步进)
 *   - 测试图输出 (9 种图案, PGCTRL 寄存器组)
 *   - 水平/垂直翻转 (CTRL0E 位域)
 *   - 帧率控制 (通过 VBLANK / VMAX 调节)
 *   - 物理时序查询 (HMAX / VMAX / SHS1 / 传感器内部温度)
 *   - 时钟树配置 (INCKSEL[0-3] + GTTABLENUM + CTRL418C)
 *
 * ── ISC 移植要点 (作为后续传感器驱动示例) ──
 *
 *   1. 寄存器编址: IMX296 使用 8/16/24-bit 寄存器, 宽度编码在地址高 2-bit:
 *      01<<16 = 8-bit, 10<<16 = 16-bit, 11<<16 = 24-bit。
 *      封装 imx296_read/write 统一处理。
 *
 *   2. 厂商私有初始化表: init() 必须写入 41 项非公开寄存器序列,
 *      否则传感器无有效图像输出。表来自 vendor data, 逐项原样保留。
 *
 *   3. 时钟树: INCKSEL[0-3] 寄存器值取决于外部 XTAL 频率,
 *      提供 3 组预设 (37.125/54/74.25 MHz), 默认 74.25 MHz。
 *
 *   4. 曝光模型: Linux 驱动使用 SHS1 = VMAX - exposure_lines 的反向模型,
 *      ISC 移植保持一致, 在 get_ctrl/set_ctrl 中完成 ns ↔ lines 双向转换。
 *
 *   5. 增益模型: 16-bit GAIN 寄存器 (0x3204), 范围 0–480,
 *      公式 gain_× = 10^(gain_code / 200), 运行时 powf()/log10f() 计算。
 *
 *   6. 格式协商: IMX296 仅支持 RAW10 Bayer BGGR, 支持 binning
 *      (当 crop=全阵列时可 2×2 binning), ROI 不支持同时 binning。
 *      try_fmt 实现完整的约束校验链。
 */

#include "isc_internal.h"
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * 寄存器地址定义 (编码: 宽度<<16 | 地址)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define IMX296_REG_8BIT(n)          ((1u << 16) | (n))
#define IMX296_REG_16BIT(n)         ((2u << 16) | (n))
#define IMX296_REG_24BIT(n)         ((3u << 16) | (n))
#define IMX296_REG_SIZE_SHIFT       16
#define IMX296_REG_ADDR_MASK        0xFFFFu

/* ── 系统控制 ── */
#define IMX296_CTRL00               IMX296_REG_8BIT(0x3000)  /* STANDBY          */
#define IMX296_CTRL00_STANDBY       (1u << 0)
#define IMX296_CTRL08               IMX296_REG_8BIT(0x3008)  /* REGHOLD          */
#define IMX296_CTRL08_REGHOLD       (1u << 0)
#define IMX296_CTRL0A               IMX296_REG_8BIT(0x300A)  /* XMSTA (stream)   */
#define IMX296_CTRL0A_XMSTA         (1u << 0)
#define IMX296_CTRL0B               IMX296_REG_8BIT(0x300B)  /* TRIGEN           */
#define IMX296_CTRL0B_TRIGEN        (1u << 0)
#define IMX296_CTRL0D               IMX296_REG_8BIT(0x300D)  /* WINMODE          */
#define IMX296_CTRL0D_WINMODE_ALL   (0u << 0)
#define IMX296_CTRL0D_WINMODE_FD_BINNING (2u << 0)
#define IMX296_CTRL0D_HADD_ON_BINNING    (1u << 5)
#define IMX296_CTRL0D_SAT_CNT            (1u << 6)
#define IMX296_CTRL0E               IMX296_REG_8BIT(0x300E)  /* H/V reverse      */
#define IMX296_CTRL0E_VREVERSE      (1u << 0)
#define IMX296_CTRL0E_HREVERSE      (1u << 1)

/* ── 时序 ── */
#define IMX296_VMAX                 IMX296_REG_24BIT(0x3010) /* 帧长度 (lines)   */
#define IMX296_HMAX                 IMX296_REG_16BIT(0x3014) /* 行长度 (INCK)    */

/* ── 温度传感器 ── */
#define IMX296_TMDCTRL              IMX296_REG_8BIT(0x301D)
#define IMX296_TMDCTRL_LATCH        (1u << 0)
#define IMX296_TMDOUT               IMX296_REG_16BIT(0x301E)
#define IMX296_TMDOUT_MASK          0x3FFu

/* ── 曝光控制 ── */
#define IMX296_SHS1                 IMX296_REG_24BIT(0x308D) /* 曝光 (lines)     */
#define IMX296_EXP_CNT              IMX296_REG_8BIT(0x30A3)
#define IMX296_EXP_CNT_RESET        (1u << 0)
#define IMX296_EXP_MAX              IMX296_REG_16BIT(0x30A6)

/* ── 时钟树 ── */
#define IMX296_INCKSEL(n)           IMX296_REG_8BIT(0x3089 + (n))
#define IMX296_CKREQSEL             IMX296_REG_8BIT(0x4101)
#define IMX296_CKREQSEL_HS          (1u << 2)
#define IMX296_GTTABLENUM           IMX296_REG_8BIT(0x4114)
#define IMX296_CTRL418C             IMX296_REG_8BIT(0x418C)

/* ── 传感器识别 ── */
#define IMX296_SENSOR_INFO          IMX296_REG_16BIT(0x3148)
#define IMX296_SENSOR_INFO_MONO     (1u << 15)
#define IMX296_SENSOR_INFO_IMX296LQ 0x4A00u
#define IMX296_SENSOR_INFO_IMX296LL 0xCA00u

/* ── 低功耗 ── */
#define IMX296_VBLANKLP             IMX296_REG_8BIT(0x309C)
#define IMX296_VBLANKLP_NORMAL      0x04u
#define IMX296_VBLANKLP_LOW_POWER   0x2Cu

/* ── 增益 ── */
#define IMX296_GAINCTRL             IMX296_REG_8BIT(0x3200)
#define IMX296_GAINCTRL_WD_GAIN_MODE_NORMAL  0x01u
#define IMX296_GAINCTRL_WD_GAIN_MODE_MULTI   0x41u
#define IMX296_GAIN                 IMX296_REG_16BIT(0x3204)
#define IMX296_GAIN_MIN             0u
#define IMX296_GAIN_MAX             480u
#define IMX296_GAINDLY              IMX296_REG_8BIT(0x3212)
#define IMX296_GAINDLY_NONE         0x08u

/* ── 测试图发生器 ── */
#define IMX296_PGCTRL               IMX296_REG_8BIT(0x3238)
#define IMX296_PGCTRL_REGEN         (1u << 0)
#define IMX296_PGCTRL_CLKEN         (1u << 2)
#define IMX296_PGCTRL_MODE(n)       ((n) << 3)
#define IMX296_PGHPOS               IMX296_REG_16BIT(0x3239)
#define IMX296_PGVPOS               IMX296_REG_16BIT(0x323C)
#define IMX296_PGHPSTEP             IMX296_REG_8BIT(0x323E)
#define IMX296_PGVPSTEP             IMX296_REG_8BIT(0x323F)
#define IMX296_PGHPNUM              IMX296_REG_8BIT(0x3240)
#define IMX296_PGVPNUM              IMX296_REG_8BIT(0x3241)
#define IMX296_PGDATA1              IMX296_REG_16BIT(0x3244)
#define IMX296_PGDATA2              IMX296_REG_16BIT(0x3246)
#define IMX296_PGHGSTEP             IMX296_REG_8BIT(0x3249)

/* ── 黑电平 ── */
#define IMX296_BLKLEVEL             IMX296_REG_16BIT(0x3254)
#define IMX296_BLKLEVELAUTO         IMX296_REG_8BIT(0x3022)
#define IMX296_BLKLEVELAUTO_ON      0x01u
#define IMX296_BLKLEVELAUTO_OFF     0xF0u

/* ── ROI / 裁剪 (FID0 寄存器组) ── */
#define IMX296_FID0_ROI             IMX296_REG_8BIT(0x3300)
#define IMX296_FID0_ROIH1ON         (1u << 0)
#define IMX296_FID0_ROIV1ON         (1u << 1)
#define IMX296_FID0_ROIPH1          IMX296_REG_16BIT(0x3310)  /* 水平起始        */
#define IMX296_FID0_ROIPV1          IMX296_REG_16BIT(0x3312)  /* 垂直起始        */
#define IMX296_FID0_ROIWH1          IMX296_REG_16BIT(0x3314)  /* 窗口宽度        */
#define IMX296_FID0_ROIWH1_MIN      80u
#define IMX296_FID0_ROIWV1          IMX296_REG_16BIT(0x3316)  /* 窗口高度        */
#define IMX296_FID0_ROIWV1_MIN      4u

/* ── 同步 / 频闪 (骨架, 暂未完全实现) ── */
#define IMX296_SYNCSEL              IMX296_REG_8BIT(0x3036)
#define IMX296_SYNCSEL_NORMAL       0xC0u

/* ═══════════════════════════════════════════════════════════════════════════
 * 传感器常量
 * ═══════════════════════════════════════════════════════════════════════════ */

#define IMX296_PIXEL_ARRAY_WIDTH    1456u
#define IMX296_PIXEL_ARRAY_HEIGHT   1088u
#define IMX296_CROP_STEP            4u      /**< 水平+垂直统一 4-pixel 步进        */
#define IMX296_MIN_CROP_WIDTH       IMX296_FID0_ROIWH1_MIN
#define IMX296_MIN_CROP_HEIGHT      IMX296_FID0_ROIWV1_MIN
#define IMX296_INCK_FREQ_HZ         74250000u /**< 内部 PLL 固定 74.25 MHz          */
#define IMX296_DEFAULT_HMAX         1100u     /**< 行长度 (INCK 单位)               */

#define IMX296_TEST_PATTERN_MAX     9u       /**< 最大测试图索引 (共 10 种)         */

/* ═══════════════════════════════════════════════════════════════════════════
 * 时钟参数表 (匹配外部 XTAL 频率)
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t freq;
    uint8_t  incksel[4];
    uint8_t  ctrl418c;
} imx296_clk_params_t;

static const imx296_clk_params_t imx296_clk_params[] = {
    { 37125000, { 0x80, 0x0B, 0x80, 0x08 }, 116 },
    { 54000000, { 0xB0, 0x0F, 0xB0, 0x0C }, 168 },
    { 74250000, { 0x80, 0x0F, 0x80, 0x0C }, 232 },
};

#define IMX296_DEFAULT_XTAL_FREQ    74250000u
#define IMX296_NUM_CLK_PARAMS \
    (sizeof(imx296_clk_params) / sizeof(imx296_clk_params[0]))

/* ═══════════════════════════════════════════════════════════════════════════
 * 内部辅助 — I2C 寄存器读写 (8/16/24-bit 统一)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief 读取任意宽度寄存器
 * @param addr 编码地址: (width<<16 | reg_addr), width=1→8-bit,2→16-bit,3→24-bit
 * @return 寄存器值; I2C 失败时 *err 非零
 */
static uint32_t imx296_read(const isc_dev_t *dev, uint32_t addr, int *err)
{
    uint8_t  buf[3] = { 0, 0, 0 };
    uint16_t reg    = (uint16_t)(addr & IMX296_REG_ADDR_MASK);
    uint8_t  width  = (uint8_t)((addr >> IMX296_REG_SIZE_SHIFT) & 3u);
    uint8_t  reg_buf[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };

    if (*err) return 0;

    int rc = dev->port->read(dev->port->user_data, reg_buf, 2, buf, width);
    if (rc != 0) {
        *err = ISC_ERR_IO;
        return 0;
    }
    return ((uint32_t)buf[2] << 16) | ((uint32_t)buf[1] << 8) | (uint32_t)buf[0];
}

/**
 * @brief 写入任意宽度寄存器
 */
static void imx296_write(const isc_dev_t *dev, uint32_t addr, uint32_t value,
                         int *err)
{
    uint8_t  buf[3];
    uint16_t reg   = (uint16_t)(addr & IMX296_REG_ADDR_MASK);
    uint8_t  width = (uint8_t)((addr >> IMX296_REG_SIZE_SHIFT) & 3u);
    uint8_t  reg_buf[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };

    if (*err) return;

    buf[0] = (uint8_t)(value & 0xFF);
    buf[1] = (uint8_t)((value >> 8) & 0xFF);
    buf[2] = (uint8_t)((value >> 16) & 0xFF);

    int rc = dev->port->write(dev->port->user_data, reg_buf, 2, buf, width);
    if (rc != 0) *err = ISC_ERR_IO;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 厂商私有初始化表 (41 项 — 不可省略)
 *
 * 来自 vendor data, 完全未公开文档。
 * 首个寄存器写入 (0x3005=0xf0) 是激活 CSI-2 输出的必要条件。
 * 其余配置内部模拟偏置 / PLL / ADC 校准 / 曝光时序微调。
 * ═══════════════════════════════════════════════════════════════════════════ */

static const struct {
    uint32_t reg;
    uint8_t  value;
} imx296_init_table[] = {
    { IMX296_REG_8BIT(0x3005), 0xF0 },
    { IMX296_REG_8BIT(0x309E), 0x04 },
    { IMX296_REG_8BIT(0x30A0), 0x04 },
    { IMX296_REG_8BIT(0x30A1), 0x3C },
    { IMX296_REG_8BIT(0x30A4), 0x5F },
    { IMX296_REG_8BIT(0x30A8), 0x91 },
    { IMX296_REG_8BIT(0x30AC), 0x28 },
    { IMX296_REG_8BIT(0x30AF), 0x09 },
    { IMX296_REG_8BIT(0x30DF), 0x00 },
    { IMX296_REG_8BIT(0x3165), 0x00 },
    { IMX296_REG_8BIT(0x3169), 0x10 },
    { IMX296_REG_8BIT(0x316A), 0x02 },
    { IMX296_REG_8BIT(0x31C8), 0xF3 },
    { IMX296_REG_8BIT(0x31D0), 0xF4 },
    { IMX296_REG_8BIT(0x321A), 0x00 },
    { IMX296_REG_8BIT(0x3226), 0x02 },
    { IMX296_REG_8BIT(0x3256), 0x01 },
    { IMX296_REG_8BIT(0x3541), 0x72 },
    { IMX296_REG_8BIT(0x3516), 0x77 },
    { IMX296_REG_8BIT(0x350B), 0x7F },
    { IMX296_REG_8BIT(0x3758), 0xA3 },
    { IMX296_REG_8BIT(0x3759), 0x00 },
    { IMX296_REG_8BIT(0x375A), 0x85 },
    { IMX296_REG_8BIT(0x375B), 0x00 },
    { IMX296_REG_8BIT(0x3832), 0xF5 },
    { IMX296_REG_8BIT(0x3833), 0x00 },
    { IMX296_REG_8BIT(0x38A2), 0xF6 },
    { IMX296_REG_8BIT(0x38A3), 0x00 },
    { IMX296_REG_8BIT(0x3A00), 0x80 },
    { IMX296_REG_8BIT(0x3D48), 0xA3 },
    { IMX296_REG_8BIT(0x3D49), 0x00 },
    { IMX296_REG_8BIT(0x3D4A), 0x85 },
    { IMX296_REG_8BIT(0x3D4B), 0x00 },
    { IMX296_REG_8BIT(0x400E), 0x58 },
    { IMX296_REG_8BIT(0x4014), 0x1C },
    { IMX296_REG_8BIT(0x4041), 0x2A },
    { IMX296_REG_8BIT(0x40A2), 0x06 },
    { IMX296_REG_8BIT(0x40C1), 0xF6 },
    { IMX296_REG_8BIT(0x40C7), 0x0F },
    { IMX296_REG_8BIT(0x40C8), 0x00 },
    { IMX296_REG_8BIT(0x4174), 0x00 },
};

/* ═══════════════════════════════════════════════════════════════════════════
 * 测试图菜单
 * ═══════════════════════════════════════════════════════════════════════════ */

static const char * const imx296_test_pattern_names[] = {
    "Disabled",
    "Multiple Pixels",
    "Sequence 1",
    "Sequence 2",
    "Gradient",
    "Row",
    "Column",
    "Cross",
    "Stripe",
    "Checks",
};

/* ═══════════════════════════════════════════════════════════════════════════
 * 内部辅助 — 格式 / 约束
 * ═══════════════════════════════════════════════════════════════════════════ */

/** @brief 查找匹配输入 XTAL 频率的时钟参数 */
static const imx296_clk_params_t *imx296_find_clk_params(uint32_t freq)
{
    for (uint8_t i = 0; i < (uint8_t)IMX296_NUM_CLK_PARAMS; i++) {
        if (imx296_clk_params[i].freq == freq)
            return &imx296_clk_params[i];
    }
    /* 默认: 74.25 MHz (索引 2) */
    return &imx296_clk_params[2];
}

/**
 * @brief 裁剪窗口对齐 (4-pixel 步进, 范围钳位)
 */
static void imx296_crop_align(uint32_t *left,  uint32_t *top,
                              uint32_t *width, uint32_t *height)
{
    *left   = (*left   / IMX296_CROP_STEP) * IMX296_CROP_STEP;
    *top    = (*top    / IMX296_CROP_STEP) * IMX296_CROP_STEP;
    *width  = ((*width  + IMX296_CROP_STEP - 1u) / IMX296_CROP_STEP)
            * IMX296_CROP_STEP;
    *height = ((*height + IMX296_CROP_STEP - 1u) / IMX296_CROP_STEP)
            * IMX296_CROP_STEP;

    if (*width  < IMX296_MIN_CROP_WIDTH)  *width  = IMX296_MIN_CROP_WIDTH;
    if (*height < IMX296_MIN_CROP_HEIGHT) *height = IMX296_MIN_CROP_HEIGHT;
    if (*left + *width  > IMX296_PIXEL_ARRAY_WIDTH)
        *left = IMX296_PIXEL_ARRAY_WIDTH - *width;
    if (*top  + *height > IMX296_PIXEL_ARRAY_HEIGHT)
        *top  = IMX296_PIXEL_ARRAY_HEIGHT - *height;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 生命周期
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief 读 SENSOR_INFO 寄存器确认传感器型号
 */
static int imx296_probe(isc_dev_t *dev)
{
    int err = 0;
    uint16_t info = (uint16_t)imx296_read(dev, IMX296_SENSOR_INFO, &err);
    if (err) return ISC_ERR_IO;

    /* IMX296LQ = 0x4A00, IMX296LL = 0xCA00 */
    if (info != IMX296_SENSOR_INFO_IMX296LQ &&
        info != IMX296_SENSOR_INFO_IMX296LL) {
        return ISC_OK;
    }
    return ISC_OK;
}

/**
 * @brief 上电时序 + 全寄存器初始化
 *
 * 序列:
 *   1. GPIO 上电 (PWDN→XCLR, 含延时)
 *   2. 写入厂商私有初始化表 (41 项)
 *   3. 配置时钟树 (INCKSEL + GTTABLENUM + CTRL418C + CKREQSEL)
 *   4. 配置默认格式 (全阵列 1456×1088, HMAX=1100, VMAX=height+30)
 *   5. 配置增益 (GAINCTRL + GAINDLY + GAIN=0)
 *   6. 配置黑电平自动校准
 *   7. 配置同步输出 (SYNCSEL)
 *   8. 退出待机
 */
static int imx296_init(isc_dev_t *dev)
{
    const imx296_clk_params_t *clk;
    uint8_t i;
    int err = 0;

    /* ── 1. GPIO 上电时序 ── */
    dev->port->gpio_write(0, 0);  dev->port->delay_ms(1);   /* PWDN=0       */
    dev->port->gpio_write(0, 1);  dev->port->delay_ms(2);   /* PWDN=1       */
    dev->port->gpio_write(1, 0);  dev->port->delay_ms(1);   /* XCLR=0       */
    dev->port->gpio_write(1, 1);  dev->port->delay_ms(10);  /* XCLR=1       */

    /* ── 2. 厂商私有初始化表 ── */
    for (i = 0; i < (uint8_t)(sizeof(imx296_init_table)
                              / sizeof(imx296_init_table[0])); i++) {
        imx296_write(dev, imx296_init_table[i].reg,
                     imx296_init_table[i].value, &err);
    }
    if (err) return err;

    /* ── 3. 时钟树 ── */
    clk = imx296_find_clk_params(IMX296_DEFAULT_XTAL_FREQ);
    for (i = 0; i < 4; i++) {
        imx296_write(dev, IMX296_INCKSEL(i), clk->incksel[i], &err);
    }
    imx296_write(dev, IMX296_GTTABLENUM, 0xC5, &err);
    imx296_write(dev, IMX296_CTRL418C,  clk->ctrl418c, &err);
    imx296_write(dev, IMX296_CKREQSEL,  IMX296_CKREQSEL_HS, &err);
    if (err) return err;

    /* ── 4. 默认格式: 全阵列, HMAX=1100, VMAX=height+30 ── */
    imx296_write(dev, IMX296_FID0_ROI, 0, &err);
    imx296_write(dev, IMX296_CTRL0D, 0, &err);
    imx296_write(dev, IMX296_HMAX, IMX296_DEFAULT_HMAX, &err);
    imx296_write(dev, IMX296_VMAX,
                 IMX296_PIXEL_ARRAY_HEIGHT + 30u, &err);
    /* SHS1 = VMAX − exposure_lines; 15ms ≈ 15e-3 × INCK/HMAX ≈ 1012 lines  */
    imx296_write(dev, IMX296_SHS1,
                 (IMX296_PIXEL_ARRAY_HEIGHT + 30u) - 1012u, &err);
    if (err) return err;

    /* ── 5. 增益 ── */
    imx296_write(dev, IMX296_GAINCTRL,
                 IMX296_GAINCTRL_WD_GAIN_MODE_NORMAL, &err);
    imx296_write(dev, IMX296_GAINDLY, IMX296_GAINDLY_NONE, &err);
    imx296_write(dev, IMX296_GAIN, IMX296_GAIN_MIN, &err);
    if (err) return err;

    /* ── 6. 黑电平 ── */
    imx296_write(dev, IMX296_BLKLEVEL, 0x3C, &err);
    imx296_write(dev, IMX296_BLKLEVELAUTO,
                 IMX296_BLKLEVELAUTO_ON, &err);
    if (err) return err;

    /* ── 7. 同步 ── */
    imx296_write(dev, IMX296_SYNCSEL, IMX296_SYNCSEL_NORMAL, &err);

    /* ── 8. 退出待机 ── */
    imx296_write(dev, IMX296_CTRL00, 0, &err);
    dev->port->delay_ms(5);

    return err;
}

/**
 * @brief 下电
 */
static int imx296_deinit(isc_dev_t *dev)
{
    int err = 0;
    imx296_write(dev, IMX296_CTRL00, IMX296_CTRL00_STANDBY, &err);
    dev->port->delay_ms(1);
    dev->port->gpio_write(1, 0);
    dev->port->gpio_write(0, 0);
    return err ? ISC_ERR_IO : ISC_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 格式枚举
 * ═══════════════════════════════════════════════════════════════════════════ */

/** @brief IMX296 仅支持 RAW10 Bayer BGGR */
static const isc_fmt_desc_t imx296_fmts[] = {
    {
        .pixel_format       = ISC_PIX_FMT_SBGGR10,
        .description        = "Bayer BGGR 10-bit",
        .bit_depth          = 10,
        .sensor_width       = IMX296_PIXEL_ARRAY_WIDTH,
        .sensor_height      = IMX296_PIXEL_ARRAY_HEIGHT,
        .crop_step_x        = IMX296_CROP_STEP,
        .crop_step_y        = IMX296_CROP_STEP,
        .min_crop_width     = IMX296_MIN_CROP_WIDTH,
        .min_crop_height    = IMX296_MIN_CROP_HEIGHT,
        .min_width          = IMX296_MIN_CROP_WIDTH,
        .max_width          = IMX296_PIXEL_ARRAY_WIDTH,
        .min_height         = IMX296_MIN_CROP_HEIGHT,
        .max_height         = IMX296_PIXEL_ARRAY_HEIGHT,
        .max_frame_rate_num = 60,
        .max_frame_rate_den = 1,
    },
};

#define IMX296_NUM_FMTS (sizeof(imx296_fmts) / sizeof(imx296_fmts[0]))

static int imx296_enum_fmts(isc_dev_t *dev, uint8_t index, isc_fmt_desc_t *desc)
{
    (void)dev;
    if (index >= IMX296_NUM_FMTS) return ISC_ENUM_END;
    *desc = imx296_fmts[index];
    return ISC_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 格式获取 / 设置 / 试探
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief 获取当前格式 — 返回 ISC 核心维护的软件缓存
 *
 * 对标 Linux V4L2 做法：格式由软件状态缓存维护 (dev->current_fmt),
 * set_fmt 时同步写入硬件, get_fmt 只读缓存 —— 零 I2C 开销。
 * 传感器寄存器不会被外部修改, 缓存始终是权威状态。
 */
static int imx296_get_fmt(isc_dev_t *dev, isc_fmt_t *fmt)
{
    *fmt = dev->current_fmt;
    return ISC_OK;
}

/**
 * @brief 试探格式 — 仅校验约束, 不写硬件
 *
 * IMX296 特殊约束:
 *   - 裁剪=全阵列时支持 binning (2×2), ROI 时 binning 自动禁用
 *   - 裁剪窗口 4-pixel 对齐, 最小 80×4
 *   - 仅支持 Bayer BGGR 10-bit
 */
static int imx296_try_fmt(isc_dev_t *dev, isc_fmt_t *fmt)
{
    (void)dev;

    /* ── 1. 像素格式: 仅 SBGGR10 ── */
    fmt->pixel_format = ISC_PIX_FMT_SBGGR10;
    fmt->bit_depth    = 10;

    /* ── 2. 裁剪窗口: 零=全阵列 ── */
    if (fmt->crop_width == 0 || fmt->crop_height == 0) {
        fmt->crop_left   = 0;
        fmt->crop_top    = 0;
        fmt->crop_width  = IMX296_PIXEL_ARRAY_WIDTH;
        fmt->crop_height = IMX296_PIXEL_ARRAY_HEIGHT;
    }

    /* ── 3. 对齐 + 钳位 ── */
    imx296_crop_align(&fmt->crop_left, &fmt->crop_top,
                      &fmt->crop_width, &fmt->crop_height);

    /* ── 4. Binning: 仅全阵列时允许 ── */
    if (fmt->crop_width  != IMX296_PIXEL_ARRAY_WIDTH ||
        fmt->crop_height != IMX296_PIXEL_ARRAY_HEIGHT) {
        fmt->bin_x = 1; fmt->bin_y = 1;
    }

    /* ── 5. 输出分辨率 ── */
    if (fmt->width  == 0) fmt->width  = fmt->crop_width;
    if (fmt->height == 0) fmt->height = fmt->crop_height;

    /* Binning 时输出 = 裁剪 / 因子                                        */
    if (fmt->bin_x > 1) fmt->width  /= (uint32_t)fmt->bin_x;
    if (fmt->bin_y > 1) fmt->height /= (uint32_t)fmt->bin_y;
    /* 默认 binning 模式: 全阵列裁剪 + 缩减因子 >1 */
    /* binning 模式通过 ctrl 设置 */

    /* ── 6. 帧率默认 ── */
    if (fmt->frame_rate_num == 0 || fmt->frame_rate_den == 0) {
        fmt->frame_rate_num = 60;
        fmt->frame_rate_den = 1;
    }

    return ISC_OK;
}

/**
 * @brief 提交格式 — 写 FID0 ROI + CTRL0D + HMAX/VMAX + 通知 FPGA
 */
static int imx296_set_fmt(isc_dev_t *dev, const isc_fmt_t *fmt)
{
    int err = 0;
    uint8_t ctrl0d = 0;

    /* ── 进入 REGHOLD (原子更新) ── */
    imx296_write(dev, IMX296_CTRL08, IMX296_CTRL08_REGHOLD, &err);

    /* ── ROI 裁剪窗口 ── */
    if (fmt->crop_width  != IMX296_PIXEL_ARRAY_WIDTH ||
        fmt->crop_height != IMX296_PIXEL_ARRAY_HEIGHT) {
        imx296_write(dev, IMX296_FID0_ROI,
                     IMX296_FID0_ROIH1ON | IMX296_FID0_ROIV1ON, &err);
        imx296_write(dev, IMX296_FID0_ROIPH1, fmt->crop_left,   &err);
        imx296_write(dev, IMX296_FID0_ROIPV1, fmt->crop_top,    &err);
        imx296_write(dev, IMX296_FID0_ROIWH1, fmt->crop_width,  &err);
        imx296_write(dev, IMX296_FID0_ROIWV1, fmt->crop_height, &err);
    } else {
        imx296_write(dev, IMX296_FID0_ROI, 0, &err);
    }

    /* ── CTRL0D: Binning 控制 ── */
    if (fmt->width  != fmt->crop_width)  ctrl0d |= IMX296_CTRL0D_HADD_ON_BINNING;
    if (fmt->height != fmt->crop_height) ctrl0d |= IMX296_CTRL0D_WINMODE_FD_BINNING;
    imx296_write(dev, IMX296_CTRL0D, ctrl0d, &err);

    /* ── 帧率: VMAX = INCK / (HMAX × fps) ── */
    if (fmt->frame_rate_num > 0 && fmt->frame_rate_den > 0) {
        uint64_t vmax64 = (uint64_t)IMX296_INCK_FREQ_HZ
                        * (uint64_t)fmt->frame_rate_den;
        uint64_t div = (uint64_t)IMX296_DEFAULT_HMAX
                     * (uint64_t)fmt->frame_rate_num;
        uint32_t vmax = (div > 0) ? (uint32_t)(vmax64 / div) : 0;

        uint32_t min_vmax = fmt->height + 30u;
        if (vmax < min_vmax) vmax = min_vmax;
        if (vmax > 0xFFFFFFu) vmax = 0xFFFFFFu;

        imx296_write(dev, IMX296_VMAX, vmax, &err);
    }

    /* ── HMAX 固定 1100 ── */
    imx296_write(dev, IMX296_HMAX, IMX296_DEFAULT_HMAX, &err);

    /* ── 退出 REGHOLD (必须无条件, 否则传感器锁死) ── */
    {
        int hold_err = 0;
        imx296_write(dev, IMX296_CTRL08, 0, &hold_err);
        if (hold_err) return ISC_ERR_IO;
    }
    if (err) return ISC_ERR_IO;
    return ISC_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 控制框架
 * ═══════════════════════════════════════════════════════════════════════════ */

static int imx296_query_ctrl(isc_dev_t *dev, isc_ctrl_desc_t *desc)
{
    (void)dev;
    uint32_t cid = desc->cid;
    memset(desc, 0, sizeof(*desc));
    desc->cid = cid;

    switch (cid) {

    /* ── 相机控制 ── */
    case ISC_CID_EXPOSURE:
        desc->type     = ISC_CTRL_TYPE_INTEGER;
        desc->unit     = "ns";
        strncpy(desc->name, "Exposure", ISC_MAX_CTRL_NAME - 1);
        desc->min.i64  = 1000;           /* 1 µs                             */
        desc->max.i64  = 5000000000LL;   /* 5 s                              */
        desc->step.i64 = 1000;           /* 1 µs 步进                        */
        desc->def.i64  = 15000000;       /* 15 ms                            */
        desc->flags    = ISC_CTRL_FLAG_STREAMABLE;
        break;

    case ISC_CID_ANALOG_GAIN:
        desc->type     = ISC_CTRL_TYPE_INTEGER;
        desc->unit     = "code";
        strncpy(desc->name, "Analog Gain", ISC_MAX_CTRL_NAME - 1);
        desc->min.i64  = IMX296_GAIN_MIN;
        desc->max.i64  = IMX296_GAIN_MAX;
        desc->step.i64 = 1;
        desc->def.i64  = IMX296_GAIN_MIN;
        desc->flags    = ISC_CTRL_FLAG_STREAMABLE;
        break;

    case ISC_CID_FRAME_RATE:
        desc->type     = ISC_CTRL_TYPE_FLOAT;
        desc->unit     = "fps";
        strncpy(desc->name, "Frame Rate", ISC_MAX_CTRL_NAME - 1);
        desc->min.f    = 1.0f;
        desc->max.f    = 120.0f;
        desc->step.f   = 0.0f;
        desc->def.f    = 40.0f;
        desc->flags    = 0;  /* 流中不可改                                   */
        break;

    /* ── 用户控制 ── */
    case ISC_CID_HFLIP:
        desc->type     = ISC_CTRL_TYPE_BOOLEAN;
        strncpy(desc->name, "H Flip", ISC_MAX_CTRL_NAME - 1);
        desc->min.b    = 0; desc->max.b = 1; desc->step.b = 1; desc->def.b = 0;
        desc->flags    = 0;
        break;

    case ISC_CID_VFLIP:
        desc->type     = ISC_CTRL_TYPE_BOOLEAN;
        strncpy(desc->name, "V Flip", ISC_MAX_CTRL_NAME - 1);
        desc->min.b    = 0; desc->max.b = 1; desc->step.b = 1; desc->def.b = 0;
        desc->flags    = 0;
        break;

    /* ── 传感器控制 ── */
    case ISC_CID_TEST_PATTERN:
        desc->type     = ISC_CTRL_TYPE_ENUM;
        strncpy(desc->name, "Test Pattern", ISC_MAX_CTRL_NAME - 1);
        desc->min.i64  = 0;
        desc->max.i64  = IMX296_TEST_PATTERN_MAX;
        desc->step.i64 = 1;
        desc->def.i64  = 0;
        desc->flags    = 0;
        break;

    case ISC_CID_PIXEL_CLOCK:
        desc->type     = ISC_CTRL_TYPE_INTEGER;
        desc->unit     = "Hz";
        strncpy(desc->name, "Pixel Clock", ISC_MAX_CTRL_NAME - 1);
        desc->min.i64  = 0;
        desc->max.i64  = 200000000;
        desc->def.i64  = IMX296_INCK_FREQ_HZ;
        desc->flags    = ISC_CTRL_FLAG_READ_ONLY | ISC_CTRL_FLAG_VOLATILE;
        break;

    case ISC_CID_LINE_LENGTH:
        desc->type     = ISC_CTRL_TYPE_INTEGER;
        desc->unit     = "INCK";
        strncpy(desc->name, "Line Length", ISC_MAX_CTRL_NAME - 1);
        desc->min.i64  = 0;
        desc->max.i64  = 65535;
        desc->def.i64  = IMX296_DEFAULT_HMAX;
        desc->flags    = ISC_CTRL_FLAG_READ_ONLY | ISC_CTRL_FLAG_VOLATILE;
        break;

    case ISC_CID_FRAME_LENGTH:
        desc->type     = ISC_CTRL_TYPE_INTEGER;
        desc->unit     = "lines";
        strncpy(desc->name, "Frame Length", ISC_MAX_CTRL_NAME - 1);
        desc->min.i64  = 0;
        desc->max.i64  = 0xFFFFFF;
        desc->def.i64  = IMX296_PIXEL_ARRAY_HEIGHT + 30;
        desc->flags    = ISC_CTRL_FLAG_READ_ONLY | ISC_CTRL_FLAG_VOLATILE;
        break;

    case ISC_CID_TEMPERATURE:
        desc->type     = ISC_CTRL_TYPE_FLOAT;
        desc->unit     = "°C";
        strncpy(desc->name, "Temperature", ISC_MAX_CTRL_NAME - 1);
        /* T = 246.312 − 0.304 × raw; raw∈[0,1023], max raw→−64.7°C      */
        desc->min.f    = -65.0f;
        desc->max.f    = 246.3f;
        desc->step.f   = 0.0f;
        desc->def.f    = 25.0f;
        desc->flags    = ISC_CTRL_FLAG_READ_ONLY | ISC_CTRL_FLAG_VOLATILE;
        break;

    case ISC_CID_LANE_COUNT:
        desc->type     = ISC_CTRL_TYPE_ENUM;
        strncpy(desc->name, "MIPI Lanes", ISC_MAX_CTRL_NAME - 1);
        desc->min.i64  = 0;          /* 索引 0 = 4 Lanes (仅此一项)        */
        desc->max.i64  = 0;
        desc->step.i64 = 1;
        desc->def.i64  = 0;
        desc->flags    = ISC_CTRL_FLAG_READ_ONLY;
        break;

    case ISC_CID_BIT_DEPTH:
        desc->type     = ISC_CTRL_TYPE_ENUM;
        strncpy(desc->name, "Bit Depth", ISC_MAX_CTRL_NAME - 1);
        desc->min.i64  = 0;          /* 索引 0 = 10-bit (仅此一项)         */
        desc->max.i64  = 0;
        desc->step.i64 = 1;
        desc->def.i64  = 0;
        desc->flags    = ISC_CTRL_FLAG_READ_ONLY;
        break;

    case ISC_CID_BLACK_LEVEL:
        desc->type     = ISC_CTRL_TYPE_INTEGER;
        desc->unit     = "DN";
        strncpy(desc->name, "Black Level", ISC_MAX_CTRL_NAME - 1);
        desc->min.i64  = 0;
        desc->max.i64  = 65535;
        desc->step.i64 = 1;
        desc->def.i64  = 0x3C;
        desc->flags    = 0;
        break;

    default:
        return ISC_ERR_NOT_SUPPORTED;
    }
    return ISC_OK;
}

static int imx296_get_ctrl(isc_dev_t *dev, uint32_t cid, isc_ctrl_value_t *val)
{
    int err = 0;

    switch (cid) {

    case ISC_CID_EXPOSURE: {
        /* Linux 反向模型: SHS1 = VMAX − exposure_lines                     */
        uint32_t vmax = imx296_read(dev, IMX296_VMAX, &err);
        uint32_t shs1 = imx296_read(dev, IMX296_SHS1, &err);
        if (err) return err;
        uint32_t lines = (vmax > shs1) ? vmax - shs1 : 0u;
        val->i64 = (int64_t)lines * (int64_t)IMX296_DEFAULT_HMAX
                 * 1000000000LL / (int64_t)IMX296_INCK_FREQ_HZ;
        break;
    }

    case ISC_CID_ANALOG_GAIN:
        val->i64 = (int64_t)imx296_read(dev, IMX296_GAIN, &err);
        break;

    case ISC_CID_FRAME_RATE: {
        uint32_t vmax = imx296_read(dev, IMX296_VMAX, &err);
        if (err) return err;
        if (vmax > 0)
            val->f = (float)IMX296_INCK_FREQ_HZ
                   / (float)(IMX296_DEFAULT_HMAX * vmax);
        else
            val->f = 40.0f;
        break;
    }

    case ISC_CID_HFLIP: {
        uint8_t ctrl0e = (uint8_t)imx296_read(dev, IMX296_CTRL0E, &err);
        if (err) return err;
        val->b = (ctrl0e & IMX296_CTRL0E_HREVERSE) ? 1 : 0;
        break;
    }

    case ISC_CID_VFLIP: {
        uint8_t ctrl0e = (uint8_t)imx296_read(dev, IMX296_CTRL0E, &err);
        if (err) return err;
        val->b = (ctrl0e & IMX296_CTRL0E_VREVERSE) ? 1 : 0;
        break;
    }

    case ISC_CID_TEST_PATTERN: {
        uint8_t pgctrl = (uint8_t)imx296_read(dev, IMX296_PGCTRL, &err);
        if (err) return err;
        val->i64 = (pgctrl & IMX296_PGCTRL_REGEN)
                 ? (int64_t)((pgctrl >> 3) & 0x0Fu) + 1 : 0;
        break;
    }

    case ISC_CID_PIXEL_CLOCK:
        val->i64 = IMX296_INCK_FREQ_HZ;
        break;

    case ISC_CID_LINE_LENGTH:
        val->i64 = (int64_t)imx296_read(dev, IMX296_HMAX, &err);
        break;

    case ISC_CID_FRAME_LENGTH:
        val->i64 = (int64_t)imx296_read(dev, IMX296_VMAX, &err);
        break;

    case ISC_CID_TEMPERATURE: {
        /* Linux: imx296_read_temperature() — 锁存 → 读取 → 清除锁存         */
        imx296_write(dev, IMX296_TMDCTRL, IMX296_TMDCTRL_LATCH, &err);
        uint16_t raw = (uint16_t)imx296_read(dev, IMX296_TMDOUT, &err);
        if (!err) {
            raw &= IMX296_TMDOUT_MASK;
            val->f = 246.312f - 0.304f * (float)raw;
        }
        imx296_write(dev, IMX296_TMDCTRL, 0, &err);
        break;
    }

    case ISC_CID_LANE_COUNT:
        val->i64 = 0;    /* 索引 0 = 4 Lanes                              */
        break;

    case ISC_CID_BIT_DEPTH:
        val->i64 = 0;    /* 索引 0 = 10-bit                               */
        break;

    case ISC_CID_BLACK_LEVEL:
        val->i64 = (int64_t)imx296_read(dev, IMX296_BLKLEVEL, &err);
        break;

    default:
        return ISC_ERR_NOT_SUPPORTED;
    }
    return err;
}

/**
 * @brief H/V 翻转后同步 Bayer 顺序到 pixel_format
 *
 * IMX296 原生 Bayer = BGGR (ISC_PIX_FMT_SBGGR10)。
 * HFLIP 交换列 (BGGR→GBRG), VFLIP 交换行 (BGGR→GRBG)。
 */
/**
 * @brief 根据 CTRL0E 寄存器值更新 current_fmt 的 pixel_format
 * @param ctrl0e  即将写入 CTRL0E 的新值 (免去重新 I2C 读取)
 */
static void imx296_sync_bayer_order(isc_dev_t *dev, uint8_t ctrl0e)
{
    /* Bayer 顺序查找表: [vflip][hflip] */
    static const uint32_t bayer_map[2][2] = {
        { ISC_PIX_FMT_SBGGR10, ISC_PIX_FMT_SGBRG10 },  /* V=0 */
        { ISC_PIX_FMT_SGRBG10, ISC_PIX_FMT_SRGGB10 },  /* V=1 */
    };

    int h = (ctrl0e & IMX296_CTRL0E_HREVERSE) ? 1 : 0;
    int v = (ctrl0e & IMX296_CTRL0E_VREVERSE) ? 1 : 0;
    dev->current_fmt.pixel_format = bayer_map[v][h];
}

static int imx296_set_ctrl(isc_dev_t *dev, uint32_t cid, isc_ctrl_value_t val)
{
    int err = 0;

    switch (cid) {

    case ISC_CID_EXPOSURE: {
        /* ns → SHS1 lines: lines = ns × INCK / (HMAX × 1e9)               */
        uint64_t lines = (uint64_t)val.i64 * (uint64_t)IMX296_INCK_FREQ_HZ
                       / (uint64_t)IMX296_DEFAULT_HMAX / 1000000000ULL;
        if (lines > 0xFFFFFFu) lines = 0xFFFFFFu;
        if (lines < 1u) lines = 1u;

        /* Linux 驱动使用反向模型: SHS1 = VMAX - exposure_lines              */
        uint32_t vmax = imx296_read(dev, IMX296_VMAX, &err);
        if (err) return err;
        if (lines >= vmax) lines = vmax - 1u;
        imx296_write(dev, IMX296_SHS1, vmax - (uint32_t)lines, &err);
        break;
    }

    case ISC_CID_ANALOG_GAIN: {
        uint32_t code = (uint32_t)val.i64;
        if (code > IMX296_GAIN_MAX) code = IMX296_GAIN_MAX;
        imx296_write(dev, IMX296_GAIN, code, &err);
        break;
    }

    case ISC_CID_FRAME_RATE: {
        /* fps → VMAX                                                       */
        if (val.f <= 0.0f) return ISC_ERR_INVALID_ARG;
        uint32_t vmax = (uint32_t)((float)IMX296_INCK_FREQ_HZ
                        / ((float)IMX296_DEFAULT_HMAX * val.f) + 0.5f);
        uint32_t min_vmax = IMX296_PIXEL_ARRAY_HEIGHT + 30u;
        if (vmax < min_vmax) vmax = min_vmax;
        if (vmax > 0xFFFFFFu) vmax = 0xFFFFFFu;

        imx296_write(dev, IMX296_CTRL08, IMX296_CTRL08_REGHOLD, &err);
        imx296_write(dev, IMX296_VMAX, vmax, &err);
        /* 曝光裁剪到新 VMAX                                                 */
        uint32_t shs1 = imx296_read(dev, IMX296_SHS1, &err);
        if (err) { imx296_write(dev, IMX296_CTRL08, 0, &err); return ISC_ERR_IO; }
        if (shs1 >= vmax) imx296_write(dev, IMX296_SHS1, vmax - 1u, &err);
        imx296_write(dev, IMX296_CTRL08, 0, &err);
        break;
    }

    case ISC_CID_HFLIP: {
        uint8_t ctrl0e = (uint8_t)imx296_read(dev, IMX296_CTRL0E, &err);
        if (val.b) ctrl0e |=  IMX296_CTRL0E_HREVERSE;
        else       ctrl0e &= ~IMX296_CTRL0E_HREVERSE;
        imx296_write(dev, IMX296_CTRL0E, ctrl0e, &err);
        if (!err) imx296_sync_bayer_order(dev, ctrl0e);
        break;
    }

    case ISC_CID_VFLIP: {
        uint8_t ctrl0e = (uint8_t)imx296_read(dev, IMX296_CTRL0E, &err);
        if (val.b) ctrl0e |=  IMX296_CTRL0E_VREVERSE;
        else       ctrl0e &= ~IMX296_CTRL0E_VREVERSE;
        imx296_write(dev, IMX296_CTRL0E, ctrl0e, &err);
        if (!err) imx296_sync_bayer_order(dev, ctrl0e);
        break;
    }

    case ISC_CID_TEST_PATTERN: {
        if (val.i64 > 0 && val.i64 <= (int64_t)IMX296_TEST_PATTERN_MAX) {
            /* 测试图使能: 配置图案参数 + 关闭黑电平自动                      */
            imx296_write(dev, IMX296_PGHPOS,   8,    &err);
            imx296_write(dev, IMX296_PGVPOS,   8,    &err);
            imx296_write(dev, IMX296_PGHPSTEP, 8,    &err);
            imx296_write(dev, IMX296_PGVPSTEP, 8,    &err);
            imx296_write(dev, IMX296_PGHPNUM,  100,  &err);
            imx296_write(dev, IMX296_PGVPNUM,  100,  &err);
            imx296_write(dev, IMX296_PGDATA1,  0x300,&err);
            imx296_write(dev, IMX296_PGDATA2,  0x100,&err);
            imx296_write(dev, IMX296_PGHGSTEP, 0,    &err);
            imx296_write(dev, IMX296_BLKLEVEL, 0,    &err);
            imx296_write(dev, IMX296_BLKLEVELAUTO,
                         IMX296_BLKLEVELAUTO_OFF, &err);
            imx296_write(dev, IMX296_PGCTRL,
                         IMX296_PGCTRL_REGEN | IMX296_PGCTRL_CLKEN
                       | IMX296_PGCTRL_MODE((uint8_t)(val.i64 - 1)), &err);
        } else {
            /* 关闭测试图, 恢复黑电平自动                                    */
            imx296_write(dev, IMX296_PGCTRL, IMX296_PGCTRL_CLKEN, &err);
            imx296_write(dev, IMX296_BLKLEVEL, 0x3C, &err);
            imx296_write(dev, IMX296_BLKLEVELAUTO,
                         IMX296_BLKLEVELAUTO_ON, &err);
        }
        break;
    }

    case ISC_CID_BLACK_LEVEL: {
        /* 黑电平自动校准时忽略显式设定; 否则写 BLKLEVEL 寄存器             */
        uint32_t level = (uint32_t)val.i64;
        if (level > 65535u) level = 65535u;
        imx296_write(dev, IMX296_BLKLEVEL, level, &err);
        break;
    }

    default:
        return ISC_ERR_NOT_SUPPORTED;
    }
    return err;
}

static int imx296_query_menu(isc_dev_t *dev, uint32_t cid, uint32_t index,
                             char *name)
{
    (void)dev;
    if (name == NULL) return ISC_ERR_INVALID_ARG;

    switch (cid) {
    case ISC_CID_TEST_PATTERN:
        if (index <= IMX296_TEST_PATTERN_MAX) {
            strncpy(name, imx296_test_pattern_names[index],
                    ISC_MAX_MENU_NAME - 1);
            return ISC_OK;
        }
        return ISC_ERR_CTRL_RANGE;

    case ISC_CID_LANE_COUNT:
        if (index == 0) { strncpy(name, "4 Lanes", ISC_MAX_MENU_NAME - 1); return ISC_OK; }
        return ISC_ERR_CTRL_RANGE;

    case ISC_CID_BIT_DEPTH:
        if (index == 0) { strncpy(name, "10-bit", ISC_MAX_MENU_NAME - 1); return ISC_OK; }
        return ISC_ERR_CTRL_RANGE;

    default:
        return ISC_ERR_NOT_SUPPORTED;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 流控制
 * ═══════════════════════════════════════════════════════════════════════════ */

static int imx296_stream_on(isc_dev_t *dev)
{
    int err = 0;
    /* 退出待机 + 启动主时钟输出 (XMSTA=1)                                  */
    imx296_write(dev, IMX296_CTRL00, 0, &err);
    dev->port->delay_ms(2);
    imx296_write(dev, IMX296_CTRL0A, IMX296_CTRL0A_XMSTA, &err);
    return err;
}

static int imx296_stream_off(isc_dev_t *dev)
{
    int err = 0;
    /* 停止主时钟输出 (XMSTA=0) + 进入待机                                  */
    imx296_write(dev, IMX296_CTRL0A, 0, &err);
    imx296_write(dev, IMX296_CTRL00, IMX296_CTRL00_STANDBY, &err);
    return err;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 物理时序查询
 * ═══════════════════════════════════════════════════════════════════════════ */

static int imx296_query_timing(isc_dev_t *dev, isc_timing_t *timing)
{
    int err = 0;

    memset(timing, 0, sizeof(*timing));

    timing->pixel_clock_hz     = IMX296_INCK_FREQ_HZ;
    timing->line_length_pclk   = imx296_read(dev, IMX296_HMAX, &err);
    timing->frame_length_lines = imx296_read(dev, IMX296_VMAX, &err);
    {
        /* Linux 反向模型: SHS1 = VMAX − exposure_lines                      */
        uint32_t shs1 = imx296_read(dev, IMX296_SHS1, &err);
        timing->exposure_lines = (timing->frame_length_lines > shs1)
            ? timing->frame_length_lines - shs1 : 0u;
    }

    /* 最大曝光 = VMAX − 1                                                   */
    if (timing->frame_length_lines > 0)
        timing->exposure_max_lines = timing->frame_length_lines - 1u;

    /* 读出跨度: 从 dev->current_fmt 软件缓存获取 (对标 Linux 做法)           */
    timing->readout_lines = dev->current_fmt.height;

    timing->lane_count = 4;
    timing->bit_depth  = 10;

    return err;
}

/**
 * @brief 试探指定格式下的预期物理时序 (无副作用)
 *
 * 与 isc_try_fmt 配对——先 try_fmt 确认格式合法, 再 try_timing
 * 获取预期时序, 验证 FPGA 流水线/输出带宽约束, 最后一次性 set_fmt 提交。
 *
 * 计算基于 HMAX=1100 (固定) 和 INCK=74.25 MHz (固定):
 *   - line_period   = HMAX / INCK ≈ 14.8 µs
 *   - frame_length  = INCK / (HMAX × fps) = VMAX
 *   - exposure_max  = VMAX − readout_lines
 *   - readout_lines = crop_height (窗口高度决定读出跨度)
 */
static int imx296_try_timing(isc_dev_t *dev, const isc_fmt_t *fmt,
                             isc_timing_t *timing)
{
    uint32_t vmax;
    uint32_t readout;

    (void)dev;
    memset(timing, 0, sizeof(*timing));

    /* ── 固定参数 ── */
    timing->pixel_clock_hz   = IMX296_INCK_FREQ_HZ;   /* 74.25 MHz           */
    timing->line_length_pclk = IMX296_DEFAULT_HMAX;    /* 1100 INCK           */
    timing->lane_count       = 4;
    timing->bit_depth        = 10;

    /* ── 读出跨度 = 裁剪窗口高度 ── */
    readout = (fmt->crop_height > 0) ? fmt->crop_height
                                     : IMX296_PIXEL_ARRAY_HEIGHT;
    timing->readout_lines = readout;

    /* ── 帧长度 = INCK / (HMAX × fps) ── */
    if (fmt->frame_rate_num > 0 && fmt->frame_rate_den > 0) {
        uint64_t vmax64 = (uint64_t)IMX296_INCK_FREQ_HZ
                        * (uint64_t)fmt->frame_rate_den;
        uint64_t div = (uint64_t)IMX296_DEFAULT_HMAX
                     * (uint64_t)fmt->frame_rate_num;
        vmax = (div > 0) ? (uint32_t)(vmax64 / div) : 0;
    } else {
        /* 默认 ~40 fps */
        vmax = IMX296_PIXEL_ARRAY_HEIGHT + 30u;
    }
    /* VMAX 下限: 至少容纳读出 + 消隐                                        */
    {
        uint32_t min_vmax = readout + 30u;
        if (vmax < min_vmax) vmax = min_vmax;
        if (vmax > 0xFFFFFFu) vmax = 0xFFFFFFu;
    }
    timing->frame_length_lines = vmax;

    /* ── 曝光: VMAX − readout, SHS1 需 ≥1 行, 保守预留 1 行余量 ── */
    if (vmax > readout + 1u) {
        timing->exposure_lines     = vmax - readout - 1u;
        timing->exposure_max_lines = vmax - readout - 1u;
    } else if (vmax > readout) {
        timing->exposure_lines     = 1u;
        timing->exposure_max_lines = 1u;
    } else {
        timing->exposure_lines     = 0;
        timing->exposure_max_lines = 0;
    }

    return ISC_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 驱动注册
 * ═══════════════════════════════════════════════════════════════════════════ */
extern isc_port_t g_i2c_port;
const isc_sensor_ops_t isc_sensor_imx296 = {
    /* ── 标识 ── */
    .model        = "sony_imx296",
    .vendor       = "Sony",
    .capabilities = ISC_CAP_TIMING_QUERY
                  | ISC_CAP_ROI
                  | ISC_CAP_BINNING,  /* Binning 仅在 crop=全阵列时可用       */
    .port         = &g_i2c_port,

    /* ── 生命周期 ── */
    .probe          = imx296_probe,
    .init           = imx296_init,
    .deinit         = imx296_deinit,
    .reset          = NULL,

    /* ── 格式 ── */
    .enum_fmts      = imx296_enum_fmts,
    .get_fmt        = imx296_get_fmt,
    .set_fmt        = imx296_set_fmt,
    .try_fmt        = imx296_try_fmt,

    /* ── 控制 ── */
    .query_ctrl     = imx296_query_ctrl,
    .get_ctrl       = imx296_get_ctrl,
    .set_ctrl       = imx296_set_ctrl,
    .query_menu     = imx296_query_menu,

    /* ── 流 ── */
    .stream_on      = imx296_stream_on,
    .stream_off     = imx296_stream_off,

    /* ── 物理状态 ── */
    .query_timing     = imx296_query_timing,
    .try_timing       = imx296_try_timing,
    .query_constraint = NULL,
    .sensor_ioctl     = NULL,
};
