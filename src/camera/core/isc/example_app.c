/**
 * @file    example_app.c
 * @brief   ISC 应用层使用示例 — Sony IMX296 全局快门传感器
 *
 * 演示完整生命周期: isc_init → isc_open → 配置格式/控制 → 流启停 → isc_close → isc_deinit.
 * 目标传感器: Sony IMX296 (1456×1088, 全局快门, Bayer BGGR, 10-bit)
 * 假设平台: FPGA 软核, I2C 总线控制, FPGA 通过 AXI-Lite 寄存器同步.
 *
 * 编译: 需链接 isc_core.c 和 isc_sensor_imx296.c.
 */

#include <stdio.h>
#include <string.h>
#include "isc.h"


/* ── FPGA 同步 (用户实现) ── */
typedef struct { void *axi_base; } my_fpga_ctx_t;
static my_fpga_ctx_t fpga_ctx = { .axi_base = (void *)0x43C00000 };

static int my_fpga_ioctl(uint32_t cmd, void *arg, void *user_data)
{
    (void)user_data;
    switch (cmd) {
    case ISC_FPGA_FORMAT_CHANGED: {
        const isc_fmt_t *fmt = (const isc_fmt_t *)arg;
        (void)fmt; break;
    }
    case ISC_FPGA_STREAM_STATE: {
        uint8_t streaming = *(uint8_t *)arg;
        (void)streaming; break;
    }
    case ISC_FPGA_TRIGGER_SET: {
        uint8_t enable = *(uint8_t *)arg;
        (void)enable; break;
    }
    default: return -1;
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 2. 全局设备和接口实例
 * ═══════════════════════════════════════════════════════════════════════════ */

static isc_fpga_ops_t fpga_ops = {
    .ioctl     = my_fpga_ioctl,
    .user_data = &fpga_ctx,
};

/* ═══════════════════════════════════════════════════════════════════════════
 * 3. 传感器驱动注册表
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 声明外部传感器驱动实例 (定义在 isc_sensor_imx296.c 中) */
extern const isc_sensor_ops_t isc_sensor_imx296;

static const isc_sensor_ops_t *sensors[] = {
    &isc_sensor_imx296,
};

/* ═══════════════════════════════════════════════════════════════════════════
 * 4. 主流程 — IMX296 完整演示
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
    rc = isc_init(NULL, &fpga_ops, sensors,
                  sizeof(sensors) / sizeof(sensors[0]));
    if (rc != ISC_OK) {
        printf("isc_init 失败: %d\n", rc);
        return;
    }

    /* ── 4.2 打开传感器 (IMX296 显式指定型号) ── */
    rc = isc_open("sony_imx296", &dev);
    if (rc != ISC_OK) {
        printf("isc_open 失败: %d\n", rc);
        goto cleanup;
    }

    /* ── 4.3 查询传感器能力 ── */
    rc = isc_query_cap(dev, &cap);
    if (rc != ISC_OK) goto cleanup;

    printf("传感器: %s %s\n", cap.vendor, cap.model);
    printf("支持功能: 0x%08X", cap.capabilities);
    if (cap.capabilities & ISC_CAP_TIMING_QUERY)     printf(" TIMING");
    if (cap.capabilities & ISC_CAP_ROI)              printf(" ROI");
    if (cap.capabilities & ISC_CAP_BINNING)          printf(" BINNING");
    printf("\n支持格式数: %u\n", cap.num_formats);

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
    /* IMX296: 像素阵列 1456×1088, 仅 Bayer BGGR 10-bit, binning 仅全阵列可用 */
    memset(&fmt, 0, sizeof(fmt));
    fmt.pixel_format   = ISC_PIX_FMT_SBGGR10;
    fmt.width          = 1456;
    fmt.height         = 1088;
    fmt.crop_width     = 1456;
    fmt.crop_height    = 1088;
    fmt.crop_left      = 0;
    fmt.crop_top       = 0;
    fmt.reduction_x    = 1; fmt.reduction_y = 1;
    fmt.reduction_mode = ISC_REDUCE_NONE;  /* IMX296: ROI 时 binning 自动禁用 */
    fmt.frame_rate_num = 60;
    fmt.frame_rate_den = 1;
    fmt.bit_depth      = 10;

    rc = isc_try_fmt(dev, &fmt);
    if (rc != ISC_OK) goto cleanup;

    printf("\ntry_fmt 结果: %u×%u, crop=(%u,%u,%u×%u), "
           "reduction=(%u,%u,%u), fps=%u/%u\n",
           fmt.width, fmt.height,
           fmt.crop_left, fmt.crop_top, fmt.crop_width, fmt.crop_height,
           fmt.reduction_x, fmt.reduction_y, fmt.reduction_mode,
           fmt.frame_rate_num, fmt.frame_rate_den);

    /* ── 4.6 提交格式 (SET_FMT — 写传感器寄存器 + 通知 FPGA) ── */
    rc = isc_set_fmt(dev, &fmt);
    if (rc != ISC_OK) goto cleanup;

    /* ── 4.7 配置控制项 ── */
    isc_ctrl_value_t val;

    /* 曝光: 15ms = 15,000,000 ns (IMX296 全局快门, SHS1 反向模型) */
    val.i64 = 15000000;
    rc = isc_set_ctrl(dev, ISC_CID_EXPOSURE, val);
    if (rc != ISC_OK) { printf("设置曝光失败: %d\n", rc); goto cleanup; }

    /* 模拟增益: 码值 120 (≈ 1.5× @ 0.1dB/step, 公式 10^(code/200)) */
    val.i64 = 120;
    rc = isc_set_ctrl(dev, ISC_CID_ANALOG_GAIN, val);
    if (rc != ISC_OK) { printf("设置增益失败: %d\n", rc); goto cleanup; }

    /* 帧率: 30 fps */
    val.f = 30.0f;
    rc = isc_set_ctrl(dev, ISC_CID_FRAME_RATE, val);
    if (rc != ISC_OK) { printf("设置帧率失败: %d\n", rc); goto cleanup; }

    /* 水平翻转 */
    val.b = 1;
    rc = isc_set_ctrl(dev, ISC_CID_HFLIP, val);
    if (rc != ISC_OK) goto cleanup;

    /* 黑电平微调 */
    val.i64 = 60;
    rc = isc_set_ctrl(dev, ISC_CID_BLACK_LEVEL, val);
    if (rc != ISC_OK) { printf("设置黑电平失败: %d\n", rc); goto cleanup; }

    /* ── 4.8 查询并打印全部控制项 ── */
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

    /* ── 4.10 查询传感器温度 ── */
    rc = isc_get_ctrl(dev, ISC_CID_TEMPERATURE, &val);
    if (rc == ISC_OK) {
        printf("\n传感器结温: %.1f °C\n", (double)val.f);
    }

    /* ── 4.11 启动/停止数据流 ── */
    rc = isc_stream_on(dev);
    if (rc != ISC_OK) goto cleanup;

    printf("\n传感器正在输出 MIPI 数据...\n");

    /* 这里 FPGA 逻辑在进行高速 MIPI→ISP→输出处理, CPU 完全空闲 */
    /* 全局快门传感器: 流中仍可调节曝光和增益                          */

    /* 流中动态调曝光: 10ms */
    val.i64 = 10000000;
    rc = isc_set_ctrl(dev, ISC_CID_EXPOSURE, val);
    if (rc != ISC_OK) { printf("流中调曝光失败: %d\n", rc); }

    /* 流中动态调增益: 码值 240 (≈ 2.0× @ 0.1dB/step) */
    val.i64 = 240;
    rc = isc_set_ctrl(dev, ISC_CID_ANALOG_GAIN, val);
    if (rc != ISC_OK) { printf("流中调增益失败: %d\n", rc); }

    rc = isc_stream_off(dev);
    if (rc != ISC_OK) goto cleanup;

    /* ── 4.12 批量控制示例 ── */
    isc_ext_ctrls_t batch = { .count = 3 };
    batch.items[0].cid       = ISC_CID_EXPOSURE;
    batch.items[0].value.i64 = 10000000;
    batch.items[1].cid       = ISC_CID_ANALOG_GAIN;
    batch.items[1].value.i64 = 240;
    batch.items[2].cid       = ISC_CID_FRAME_RATE;
    batch.items[2].value.f   = 30.0f;
    rc = isc_set_ext_ctrls(dev, &batch);
    if (rc != ISC_OK) {
        printf("批量设置失败: error_idx=%u\n", batch.error_idx);
    }

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

    isc_init(NULL, &fpga_ops, sensors,
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
