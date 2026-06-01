/**
 * @file    example_app.c
 * @brief   ISC 应用层使用示例
 *
 * 演示完整生命周期: isc_init → isc_open → 配置格式/控制 → 流启停 → isc_close → isc_deinit.
 * 假设平台: FPGA 软核, I2C 总线控制 Sony IMX477, FPGA 通过 AXI-Lite 寄存器同步.
 *
 * 编译: 需链接 isc_core.c 和 isc_sensor_sony.c.
 */

#include <stdio.h>
#include <string.h>
#include "isc/isc.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * 1. 平台移植 — 实现 isc_port_t 和 isc_fpga_ops_t
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ── I2C 私有多路复用上下文 ── */
typedef struct {
    void    *i2c_base;     /* I2C 控制器寄存器基址 (平台相关) */
    uint8_t  dev_addr;     /* 传感器 I2C 7-bit 地址 */
} my_i2c_ctx_t;

static int my_i2c_read(void *user_data, uint32_t reg_addr,
                       uint8_t *data, uint16_t len)
{
    my_i2c_ctx_t *ctx = (my_i2c_ctx_t *)user_data;
    /** TODO: 平台 I2C 读实现:
     *   i2c_start(); i2c_write_byte(ctx->dev_addr << 1);
     *   i2c_write_byte((reg_addr >> 8) & 0xFF); i2c_write_byte(reg_addr & 0xFF);
     *   i2c_start(); i2c_write_byte((ctx->dev_addr << 1) | 1);
     *   for (i=0; i<len; i++) data[i] = i2c_read_byte(i==len-1 ? NAK : ACK);
     *   i2c_stop();
     */
    (void)ctx; (void)reg_addr; (void)data; (void)len;
    return 0;
}

static int my_i2c_write(void *user_data, uint32_t reg_addr,
                        const uint8_t *data, uint16_t len)
{
    my_i2c_ctx_t *ctx = (my_i2c_ctx_t *)user_data;
    /** TODO: 平台 I2C 写实现 */
    (void)ctx; (void)reg_addr; (void)data; (void)len;
    return 0;
}

static void my_delay_ms(uint32_t ms)   { /** TODO: 平台毫秒延时 */ (void)ms; }
static void my_delay_us(uint32_t us)   { /** TODO: 平台微秒延时 */ (void)us; }
static int  my_gpio_write(uint8_t pin, uint8_t level) {
    /** TODO: 平台 GPIO 写 (PWDN/RESET/XCLR) */
    (void)pin; (void)level; return 0;
}

/* ── FPGA 同步上下文 ── */
typedef struct {
    void *axi_base;     /* AXI-Lite 寄存器基址 */
} my_fpga_ctx_t;

static int my_fpga_ioctl(uint32_t cmd, void *arg, void *user_data)
{
    my_fpga_ctx_t *ctx = (my_fpga_ctx_t *)user_data;
    switch (cmd) {
    case ISC_FPGA_FORMAT_CHANGED: {
        const isc_fmt_t *fmt = (const isc_fmt_t *)arg;
        /** TODO: 写 FPGA 格式寄存器组:
         *   axi_write(ctx->axi_base + FPGA_REG_WIDTH,       fmt->width);
         *   axi_write(ctx->axi_base + FPGA_REG_HEIGHT,      fmt->height);
         *   axi_write(ctx->axi_base + FPGA_REG_PIX_FMT,     fmt->pixel_format);
         *   axi_write(ctx->axi_base + FPGA_REG_CROP_LEFT,   fmt->crop_left);
         *   axi_write(ctx->axi_base + FPGA_REG_CROP_TOP,    fmt->crop_top);
         *   axi_write(ctx->axi_base + FPGA_REG_CROP_WIDTH,  fmt->crop_width);
         *   axi_write(ctx->axi_base + FPGA_REG_CROP_HEIGHT, fmt->crop_height);
         *   axi_write(ctx->axi_base + FPGA_REG_REDUCTION,   fmt->reduction);
         */
        (void)fmt;
        break;
    }
    case ISC_FPGA_STREAM_STATE: {
        uint8_t streaming = *(uint8_t *)arg;
        /** TODO: 写 FPGA 流控制寄存器:
         *   axi_write(ctx->axi_base + FPGA_REG_STREAM_EN, streaming);
         */
        (void)streaming;
        break;
    }
    case ISC_FPGA_TRIGGER_SET: {
        uint8_t enable = *(uint8_t *)arg;
        /** TODO: 写 FPGA 触发控制寄存器 */
        (void)enable;
        break;
    }
    default:
        return -1;
    }
    (void)ctx;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 2. 全局设备和接口实例
 * ═══════════════════════════════════════════════════════════════════════════ */

static my_i2c_ctx_t  i2c_ctx  = { .i2c_base = (void *)0x40001000, .dev_addr = 0x1A };
static my_fpga_ctx_t fpga_ctx = { .axi_base = (void *)0x43C00000 };

static isc_port_t port = {
    .bus_type    = ISC_BUS_I2C,
    .read        = my_i2c_read,
    .write       = my_i2c_write,
    .delay_ms    = my_delay_ms,
    .delay_us    = my_delay_us,
    .gpio_write  = my_gpio_write,
    .cs_assert   = NULL,
    .cs_deassert = NULL,
    .user_data   = &i2c_ctx,
};

static isc_fpga_ops_t fpga_ops = {
    .ioctl     = my_fpga_ioctl,
    .user_data = &fpga_ctx,
};

/* ═══════════════════════════════════════════════════════════════════════════
 * 3. 传感器驱动注册表
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 声明外部传感器驱动实例 (定义在 isc_sensor_xxx.c 中) */
extern const isc_sensor_ops_t isc_sensor_sony;
extern const isc_sensor_ops_t isc_sensor_gsense;
extern const isc_sensor_ops_t isc_sensor_smartsens;

static const isc_sensor_ops_t *sensors[] = {
    &isc_sensor_sony,
    &isc_sensor_gsense,
    &isc_sensor_smartsens,
};

/* ═══════════════════════════════════════════════════════════════════════════
 * 4. 主流程
 * ═══════════════════════════════════════════════════════════════════════════ */

void app_sensor_demo(void)
{
    int            rc;
    isc_dev_t     *dev = NULL;
    isc_cap_t      cap;
    isc_fmt_desc_t desc;
    isc_fmt_t      fmt;
    isc_timing_t   timing;

    /* ── 4.1 初始化 ISC 框架 ── */
    rc = isc_init(&port, &fpga_ops, sensors,
                  sizeof(sensors) / sizeof(sensors[0]));
    if (rc != ISC_OK) {
        printf("isc_init 失败: %d\n", rc);
        return;
    }

    /* ── 4.2 打开传感器 (显式指定型号) ── */
    rc = isc_open("sony_imx477", &dev);
    if (rc != ISC_OK) {
        printf("isc_open 失败: %d\n", rc);
        goto cleanup;
    }

    /* ── 4.3 查询传感器能力 ── */
    rc = isc_query_cap(dev, &cap);
    if (rc != ISC_OK) goto cleanup;

    printf("传感器: %s %s\n", cap.vendor, cap.model);
    printf("支持功能: 0x%08X\n", cap.capabilities);
    printf("支持格式数: %u\n", cap.num_formats);

    /* ── 4.4 枚举并打印所有支持格式 ── */
    printf("\n支持格式:\n");
    for (uint8_t i = 0; ; i++) {
        rc = isc_enum_fmt(dev, i, &desc);
        if (rc == ISC_ENUM_END) break;
        if (rc != ISC_OK) { printf("枚举错误: %d\n", rc); break; }
        printf("  [%u] %s (bayer=%u, sensor=%u×%u, crop_step=%u×%u, "
               "out=%u×%u..%u×%u, max_fps=%u/%u)\n",
               i, desc.description, desc.bit_depth,
               desc.sensor_width, desc.sensor_height,
               desc.crop_step_x, desc.crop_step_y,
               desc.min_width, desc.min_height,
               desc.max_width, desc.max_height,
               desc.max_frame_rate_num, desc.max_frame_rate_den);
    }

    /* ── 4.5 试探格式 (TRY_FMT — 不扰动硬件) ── */
    memset(&fmt, 0, sizeof(fmt));
    fmt.pixel_format   = ISC_PIX_FMT_SRGGB10;
    fmt.width          = 960;
    fmt.height         = 540;
    fmt.crop_width     = 1920;
    fmt.crop_height    = 1080;
    fmt.crop_left      = (4056 - 1920) / 2;
    fmt.crop_top       = (3040 - 1080) / 2;
    fmt.reduction      = ISC_REDUCTION_BIN_2;
    fmt.frame_rate_num = 60;
    fmt.frame_rate_den = 1;
    fmt.bit_depth      = 10;

    rc = isc_try_fmt(dev, &fmt);
    if (rc != ISC_OK) goto cleanup;

    printf("\ntry_fmt 结果: %u×%u, crop=(%u,%u,%u×%u), "
           "reduction=%u, fps=%u/%u\n",
           fmt.width, fmt.height,
           fmt.crop_left, fmt.crop_top, fmt.crop_width, fmt.crop_height,
           fmt.reduction, fmt.frame_rate_num, fmt.frame_rate_den);

    /* ── 4.6 提交格式 (SET_FMT — 写传感器寄存器 + 通知 FPGA) ── */
    rc = isc_set_fmt(dev, &fmt);
    if (rc != ISC_OK) goto cleanup;

    /* ── 4.7 配置控制项 ── */
    isc_ctrl_value_t val;

    /* 曝光: 15ms = 15,000,000 ns */
    val.i64 = 15000000;
    rc = isc_set_ctrl(dev, ISC_CID_EXPOSURE, val);
    if (rc != ISC_OK) goto cleanup;

    /* 模拟增益: 1.5× */
    val.f = 1.5f;
    rc = isc_set_ctrl(dev, ISC_CID_ANALOG_GAIN, val);
    if (rc != ISC_OK) goto cleanup;

    /* 设置自动曝光模式——片内 AEC 引擎 */
    val.i64 = ISC_EXPOSURE_AUTO_CONTINUOUS;
    rc = isc_set_ctrl(dev, ISC_CID_EXPOSURE_AUTO, val);
    if (rc != ISC_OK) goto cleanup;

    /* ── 4.8 查询并打印全部控制项 (使用 isc_query_next_ctrl) ── */
    printf("\n控制项:\n");
    isc_ctrl_desc_t cd;
    while ((rc = isc_query_next_ctrl(dev, &cd)) == ISC_OK) {
        isc_get_ctrl(dev, cd.cid, &val);
        printf("  [%08X] %-24s", cd.cid, cd.name);
        switch (cd.type) {
        case ISC_CTRL_TYPE_INTEGER:
        case ISC_CTRL_TYPE_ENUM:
            printf("= %lld %s\n", (long long)val.i64,
                   cd.unit ? cd.unit : "");
            break;
        case ISC_CTRL_TYPE_BOOLEAN:
            printf("= %s\n", val.b ? "ON" : "OFF");
            break;
        case ISC_CTRL_TYPE_FLOAT:
            printf("= %.3f %s\n", (double)val.f,
                   cd.unit ? cd.unit : "");
            break;
        }
    }

    /* ── 4.9 查询物理时序 ── */
    rc = isc_query_timing(dev, &timing);
    if (rc == ISC_OK) {
        printf("\n物理时序:\n");
        printf("  pixel_clock:    %u Hz\n", timing.pixel_clock_hz);
        printf("  line_length:    %u pclk\n", timing.line_length_pclk);
        printf("  frame_length:   %u lines\n", timing.frame_length_lines);
        printf("  line_period:    %u ns\n", timing.line_period_ns);
        printf("  frame_period:   %u ns\n", timing.frame_period_ns);
        printf("  exposure:       %u lines / %u ns\n",
               timing.exposure_lines, timing.exposure_time_ns);
        printf("  exposition_max: %u lines / %u ns\n",
               timing.exposure_max_lines, timing.exposure_max_ns);
        printf("  lane_count:     %u\n", timing.lane_count);
        printf("  bit_depth:      %u\n", timing.bit_depth);
    }

    /* ── 4.10 启动/停止数据流 ── */
    rc = isc_stream_on(dev);
    if (rc != ISC_OK) goto cleanup;

    printf("\n传感器正在输出 LVDS 数据...\n");

    /* 这里 FPGA 逻辑在进行高速 LVDS→ISP→输出处理, CPU 完全空闲 */

    rc = isc_stream_off(dev);
    if (rc != ISC_OK) goto cleanup;

    /* ── 4.11 批量控制示例 ──
     * (实际工程中常用于"一次性应用曝光+增益+帧率", 减少 I2C 事务) ── */
    isc_ext_ctrls_t batch = { .count = 3 };
    batch.items[0].cid    = ISC_CID_EXPOSURE;
    batch.items[0].value.i64 = 10000000;
    batch.items[1].cid    = ISC_CID_ANALOG_GAIN;
    batch.items[1].value.f = 2.0f;
    batch.items[2].cid    = ISC_CID_FRAME_RATE;
    batch.items[2].value.f = 30.0f;
    rc = isc_set_ext_ctrls(dev, &batch);
    if (rc != ISC_OK) {
        printf("批量设置失败: error_idx=%u\n", batch.error_idx);
    }

    /* ── 4.12 传感器专属操作 ── */
    /* 例: Sony 的 DOL-HDR 配置 (厂商扩展) */
    /* isc_sensor_ioctl(dev, SONY_IOCTL_DOL_HDR_CFG, &hdr_cfg); */

    /* ── 4.13 关闭 ── */
cleanup:
    if (dev != NULL) {
        isc_close(dev);
    }
    isc_deinit();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 5. 自动探测示例 (固件不预知传感器型号)
 * ═══════════════════════════════════════════════════════════════════════════ */

void app_auto_detect_demo(void)
{
    isc_dev_t *dev = NULL;

    isc_init(&port, &fpga_ops, sensors,
             sizeof(sensors) / sizeof(sensors[0]));

    int rc = isc_open(NULL, &dev);  /* NULL = 自动探测 */
    if (rc != ISC_OK) {
        printf("自动探测失败: %d\n", rc);
        isc_deinit();
        return;
    }

    isc_cap_t cap;
    isc_query_cap(dev, &cap);
    printf("探测到传感器: %s %s\n", cap.vendor, cap.model);

    isc_close(dev);
    isc_deinit();
}
