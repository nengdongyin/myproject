/**
 * @file    test_isc_common.c
 * @brief   ISC 单元测试 — 公共夹具实现
 */

#include "test_isc_common.h"
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════════
 *  Mock Register File
 * ═══════════════════════════════════════════════════════════════════════════ */

mock_regfile_t g_mock_regs;

void mock_regfile_reset(void)
{
    memset(&g_mock_regs, 0, sizeof(g_mock_regs));
}

void mock_regfile_set_error(int err)
{
    g_mock_regs.inject_error = err;
}

/* ── Mock I2C read ── */
static int mock_read(void *user_data,
                     const uint8_t *reg_addr, uint16_t reg_len,
                     uint8_t *data, uint16_t data_len)
{
    (void)user_data;
    g_mock_regs.read_count++;

    if (g_mock_regs.inject_error)
        return g_mock_regs.inject_error;

    /* 寄存器地址: 大端序 uint16 → idx */
    uint32_t idx = 0;
    for (uint16_t i = 0; i < reg_len; i++)
        idx = (idx << 8) | reg_addr[i];
    g_mock_regs.last_reg = idx;

    if (idx < MOCK_REG_COUNT) {
        for (uint16_t i = 0; i < data_len && (idx + i) < MOCK_REG_COUNT; i++)
            data[i] = g_mock_regs.regs[idx + i];
    }
    return 0;
}

/* ── Mock I2C write ── */
static int mock_write(void *user_data,
                      const uint8_t *reg_addr, uint16_t reg_len,
                      const uint8_t *data, uint16_t data_len)
{
    (void)user_data;
    g_mock_regs.write_count++;

    if (g_mock_regs.inject_error)
        return g_mock_regs.inject_error;

    uint32_t idx = 0;
    for (uint16_t i = 0; i < reg_len; i++)
        idx = (idx << 8) | reg_addr[i];
    g_mock_regs.last_reg = idx;

    if (idx < MOCK_REG_COUNT) {
        for (uint16_t i = 0; i < data_len && (idx + i) < MOCK_REG_COUNT; i++)
            g_mock_regs.regs[idx + i] = data[i];
    }
    return 0;
}

static void mock_delay_ms(uint32_t ms)  { (void)ms; }
static int  mock_gpio_write(uint8_t pin, uint8_t level)
    { (void)pin; (void)level; return 0; }

isc_port_t g_mock_port = {
    .bus_type   = ISC_BUS_I2C,
    .read       = mock_read,
    .write      = mock_write,
    .cs_assert  = NULL,
    .cs_deassert= NULL,
    .delay_ms   = mock_delay_ms,
    .gpio_write = mock_gpio_write,
    .user_data  = NULL,
};

/* ═══════════════════════════════════════════════════════════════════════════
 *  Mock FPGA Ops
 * ═══════════════════════════════════════════════════════════════════════════ */

mock_fpga_t g_mock_fpga;

void mock_fpga_reset(void)
{
    memset(&g_mock_fpga, 0, sizeof(g_mock_fpga));
}

static int mock_fpga_ioctl(uint32_t cmd, void *arg, void *user_data)
{
    (void)user_data;
    if (g_mock_fpga.inject_error)
        return g_mock_fpga.inject_error;

    switch (cmd) {
    case ISC_FPGA_FORMAT_CHANGED:
        g_mock_fpga.format_changed_count++;
        if (arg) g_mock_fpga.last_fmt = *(const isc_fmt_t *)arg;
        break;
    case ISC_FPGA_STREAM_STATE:
        g_mock_fpga.stream_state_count++;
        if (arg) g_mock_fpga.last_stream_state = *(uint8_t *)arg;
        break;
    default:
        return -1;
    }
    return 0;
}

isc_fpga_ops_t g_mock_fpga_ops = {
    .ioctl     = mock_fpga_ioctl,
    .user_data = NULL,
};

/* ═══════════════════════════════════════════════════════════════════════════
 *  Mock Sensor Driver
 * ═══════════════════════════════════════════════════════════════════════════ */

mock_sensor_ctx_t g_mock_sensor;

void mock_sensor_reset(void)
{
    memset(&g_mock_sensor, 0, sizeof(g_mock_sensor));
    g_mock_sensor.current_fmt.width         = 1920;
    g_mock_sensor.current_fmt.height        = 1080;
    g_mock_sensor.current_fmt.pixel_format  = ISC_PIX_FMT_SRGGB8;
    g_mock_sensor.current_fmt.frame_rate_num = 30;
    g_mock_sensor.current_fmt.frame_rate_den = 1;
    g_mock_sensor.current_fmt.bit_depth      = 8;
    g_mock_sensor.current_fmt.crop_width     = 1920;
    g_mock_sensor.current_fmt.crop_height    = 1080;
    g_mock_sensor.current_timing.pixel_clock_hz     = 74250000;
    g_mock_sensor.current_timing.line_length_pclk   = 2200;
    g_mock_sensor.current_timing.frame_length_lines = 1125;
    g_mock_sensor.current_timing.lane_count  = 2;
    g_mock_sensor.current_timing.bit_depth   = 8;
    g_mock_sensor.current_timing.exposure_lines     = 1000;
    g_mock_sensor.current_timing.exposure_max_lines = 1100;
    g_mock_sensor.current_timing.readout_lines      = 1080;
    g_mock_sensor.exposure    = 15000000;
    g_mock_sensor.analog_gain = 0;
    g_mock_sensor.frame_rate  = 30.0f;
}

void mock_sensor_set_probe_ret(int ret)
{
    g_mock_sensor.probe_return = ret;
}

void mock_sensor_set_init_ret(int ret)
{
    g_mock_sensor.init_return = ret;
}

/* ── 格式描述: 2 种格式 ── */
static const isc_fmt_desc_t mock_fmts[] = {
    {
        .pixel_format    = ISC_PIX_FMT_SRGGB8,
        .description     = "Bayer RGGB 8-bit",
        .bit_depth       = 8,
        .sensor_width    = 1920,
        .sensor_height   = 1080,
        .crop_step_x     = 2,
        .crop_step_y     = 2,
        .min_crop_width  = 64,
        .min_crop_height = 4,
        .min_width       = 64,
        .max_width       = 1920,
        .min_height      = 4,
        .max_height      = 1080,
        .max_frame_rate_num = 60,
        .max_frame_rate_den = 1,
    },
    {
        .pixel_format    = ISC_PIX_FMT_SBGGR10,
        .description     = "Bayer BGGR 10-bit",
        .bit_depth       = 10,
        .sensor_width    = 1920,
        .sensor_height   = 1080,
        .crop_step_x     = 4,
        .crop_step_y     = 4,
        .min_crop_width  = 80,
        .min_crop_height = 4,
        .min_width       = 80,
        .max_width       = 1920,
        .min_height      = 4,
        .max_height      = 1080,
        .max_frame_rate_num = 120,
        .max_frame_rate_den = 1,
    },
};

/* ── 测试图菜单 ── */
static const char * const mock_test_patterns[] = {
    "Disabled", "Color Bars", "Gradient",
};

/* ═══════════════════════════════════════════════════════════════════════════
 *  Ops 实现
 * ═══════════════════════════════════════════════════════════════════════════ */

static int mock_probe(isc_dev_t *dev)
{
    (void)dev;
    g_mock_sensor.probe_called++;
    return g_mock_sensor.probe_return;
}

static int mock_init(isc_dev_t *dev)
{
    (void)dev;
    g_mock_sensor.init_called++;
    return g_mock_sensor.init_return;
}

static int mock_deinit(isc_dev_t *dev)
{
    (void)dev;
    g_mock_sensor.deinit_called++;
    return ISC_OK;
}

static int mock_enum_fmts(isc_dev_t *dev, uint8_t index, isc_fmt_desc_t *desc)
{
    (void)dev;
    if (index >= 2) return ISC_ENUM_END;
    *desc = mock_fmts[index];
    return ISC_OK;
}

static int mock_get_fmt(isc_dev_t *dev, isc_fmt_t *fmt)
{
    (void)dev;
    *fmt = g_mock_sensor.current_fmt;
    return ISC_OK;
}

static int mock_set_fmt(isc_dev_t *dev, const isc_fmt_t *fmt)
{
    (void)dev;
    g_mock_sensor.current_fmt = *fmt;
    return ISC_OK;
}

static int mock_try_fmt(isc_dev_t *dev, isc_fmt_t *fmt)
{
    (void)dev;
    /* 简单约束: 格式化+对齐 */
    if (fmt->pixel_format == 0)
        fmt->pixel_format = g_mock_sensor.current_fmt.pixel_format;
    fmt->crop_left   &= ~1u;
    fmt->crop_top    &= ~1u;
    fmt->crop_width  &= ~1u;
    fmt->crop_height &= ~1u;
    if (fmt->width  == 0) fmt->width  = fmt->crop_width;
    if (fmt->height == 0) fmt->height = fmt->crop_height;
    if (fmt->crop_width == 0) fmt->crop_width = 1920;
    if (fmt->crop_height == 0) fmt->crop_height = 1080;
    return ISC_OK;
}

static int mock_query_ctrl(isc_dev_t *dev, isc_ctrl_desc_t *desc)
{
    (void)dev;
    uint32_t cid = desc->cid;
    memset(desc, 0, sizeof(*desc));
    desc->cid = cid;

    switch (cid) {
    case ISC_CID_EXPOSURE:
        desc->type     = ISC_CTRL_TYPE_INTEGER;
        desc->unit     = "ns";
        strncpy(desc->name, "Exposure", ISC_MAX_CTRL_NAME - 1);
        desc->min.i64  = 1000;
        desc->max.i64  = 1000000000;
        desc->step.i64 = 1000;
        desc->def.i64  = 15000000;
        desc->flags    = ISC_CTRL_FLAG_STREAMABLE;
        break;
    case ISC_CID_ANALOG_GAIN:
        desc->type     = ISC_CTRL_TYPE_INTEGER;
        desc->unit     = "code";
        strncpy(desc->name, "Analog Gain", ISC_MAX_CTRL_NAME - 1);
        desc->min.i64  = 0;
        desc->max.i64  = 480;
        desc->step.i64 = 1;
        desc->def.i64  = 0;
        desc->flags    = ISC_CTRL_FLAG_STREAMABLE;
        break;
    case ISC_CID_FRAME_RATE:
        desc->type     = ISC_CTRL_TYPE_FLOAT;
        desc->unit     = "fps";
        strncpy(desc->name, "Frame Rate", ISC_MAX_CTRL_NAME - 1);
        desc->min.f    = 1.0f;
        desc->max.f    = 120.0f;
        desc->def.f    = 30.0f;
        desc->flags    = 0;
        break;
    case ISC_CID_HFLIP:
        desc->type     = ISC_CTRL_TYPE_BOOLEAN;
        strncpy(desc->name, "H Flip", ISC_MAX_CTRL_NAME - 1);
        desc->min.b    = 0; desc->max.b = 1; desc->step.b = 1;
        desc->flags    = 0;
        break;
    case ISC_CID_VFLIP:
        desc->type     = ISC_CTRL_TYPE_BOOLEAN;
        strncpy(desc->name, "V Flip", ISC_MAX_CTRL_NAME - 1);
        desc->min.b    = 0; desc->max.b = 1; desc->step.b = 1;
        desc->flags    = 0;
        break;
    case ISC_CID_TEST_PATTERN:
        desc->type     = ISC_CTRL_TYPE_ENUM;
        strncpy(desc->name, "Test Pattern", ISC_MAX_CTRL_NAME - 1);
        desc->min.i64  = 0;
        desc->max.i64  = 2;
        desc->step.i64 = 1;
        desc->flags    = 0;
        break;
    /* 只读控制 */
    case ISC_CID_TEMPERATURE:
        desc->type     = ISC_CTRL_TYPE_FLOAT;
        desc->unit     = "°C";
        strncpy(desc->name, "Temperature", ISC_MAX_CTRL_NAME - 1);
        desc->min.f    = -40.0f;
        desc->max.f    = 125.0f;
        desc->def.f    = 25.0f;
        desc->flags    = ISC_CTRL_FLAG_READ_ONLY | ISC_CTRL_FLAG_VOLATILE;
        break;
    case ISC_CID_PIXEL_CLOCK:
        desc->type     = ISC_CTRL_TYPE_INTEGER;
        desc->unit     = "Hz";
        strncpy(desc->name, "Pixel Clock", ISC_MAX_CTRL_NAME - 1);
        desc->min.i64  = 0;
        desc->max.i64  = 200000000;
        desc->flags    = ISC_CTRL_FLAG_READ_ONLY;
        break;
    default:
        return ISC_ERR_NOT_SUPPORTED;
    }
    return ISC_OK;
}

static int mock_get_ctrl(isc_dev_t *dev, uint32_t cid, isc_ctrl_value_t *val)
{
    (void)dev;
    switch (cid) {
    case ISC_CID_EXPOSURE:      val->i64 = g_mock_sensor.exposure;     break;
    case ISC_CID_ANALOG_GAIN:   val->i64 = g_mock_sensor.analog_gain;  break;
    case ISC_CID_FRAME_RATE:    val->f   = g_mock_sensor.frame_rate;   break;
    case ISC_CID_HFLIP:         val->b   = (uint8_t)g_mock_sensor.hflip; break;
    case ISC_CID_VFLIP:         val->b   = (uint8_t)g_mock_sensor.vflip; break;
    case ISC_CID_TEST_PATTERN:  val->i64 = g_mock_sensor.test_pattern; break;
    case ISC_CID_TEMPERATURE:   val->f   = 42.5f;                     break;
    case ISC_CID_PIXEL_CLOCK:   val->i64 = 74250000;                  break;
    default: return ISC_ERR_NOT_SUPPORTED;
    }
    return ISC_OK;
}

static int mock_set_ctrl(isc_dev_t *dev, uint32_t cid, isc_ctrl_value_t val)
{
    (void)dev;
    switch (cid) {
    case ISC_CID_EXPOSURE:      g_mock_sensor.exposure     = val.i64;   break;
    case ISC_CID_ANALOG_GAIN:   g_mock_sensor.analog_gain  = val.i64;   break;
    case ISC_CID_FRAME_RATE:    g_mock_sensor.frame_rate   = val.f;     break;
    case ISC_CID_HFLIP:         g_mock_sensor.hflip        = val.b;      break;
    case ISC_CID_VFLIP:         g_mock_sensor.vflip        = val.b;      break;
    case ISC_CID_TEST_PATTERN:  g_mock_sensor.test_pattern = (int)val.i64; break;
    default: return ISC_ERR_NOT_SUPPORTED;
    }
    return ISC_OK;
}

static int mock_query_menu(isc_dev_t *dev, uint32_t cid, uint32_t index, char *name)
{
    (void)dev;
    if (cid != ISC_CID_TEST_PATTERN) return ISC_ERR_NOT_SUPPORTED;
    if (index > 2) return ISC_ERR_CTRL_RANGE;
    strncpy(name, mock_test_patterns[index], ISC_MAX_MENU_NAME - 1);
    return ISC_OK;
}

static int mock_stream_on(isc_dev_t *dev)
{
    (void)dev;
    g_mock_sensor.stream_state = 1;
    return ISC_OK;
}

static int mock_stream_off(isc_dev_t *dev)
{
    (void)dev;
    g_mock_sensor.stream_state = 0;
    return ISC_OK;
}

static int mock_query_timing(isc_dev_t *dev, isc_timing_t *timing)
{
    (void)dev;
    *timing = g_mock_sensor.current_timing;
    return ISC_OK;
}

static int mock_try_timing(isc_dev_t *dev, const isc_fmt_t *fmt, isc_timing_t *timing)
{
    (void)dev;
    memset(timing, 0, sizeof(*timing));
    timing->pixel_clock_hz     = 74250000;
    timing->line_length_pclk   = 2200;
    timing->lane_count         = 2;
    timing->bit_depth          = fmt->bit_depth;
    timing->readout_lines      = (uint32_t)(fmt->crop_height > 0 ? fmt->crop_height : 1080);
    timing->frame_length_lines = timing->readout_lines + 45;
    timing->exposure_lines     = timing->readout_lines;
    timing->exposure_max_lines = timing->readout_lines + 20;
    return ISC_OK;
}

/* ── Ops 表 ── */
const isc_sensor_ops_t g_mock_sensor_ops = {
    .model        = "mock_sensor",
    .vendor       = "MockVendor",
    .capabilities = ISC_CAP_TIMING_QUERY | ISC_CAP_ROI | ISC_CAP_BINNING,
    .port         = NULL,  /* 使用全局 port */

    .probe      = mock_probe,
    .init       = mock_init,
    .deinit     = mock_deinit,
    .reset      = NULL,

    .enum_fmts  = mock_enum_fmts,
    .get_fmt    = mock_get_fmt,
    .set_fmt    = mock_set_fmt,
    .try_fmt    = mock_try_fmt,

    .query_ctrl = mock_query_ctrl,
    .get_ctrl   = mock_get_ctrl,
    .set_ctrl   = mock_set_ctrl,
    .query_menu = mock_query_menu,

    .stream_on  = mock_stream_on,
    .stream_off = mock_stream_off,

    .query_timing     = mock_query_timing,
    .try_timing       = mock_try_timing,
    .query_constraint = NULL,
    .sensor_ioctl     = NULL,
};

/* ═══════════════════════════════════════════════════════════════════════════
 *  测试夹具
 * ═══════════════════════════════════════════════════════════════════════════ */

static const isc_sensor_ops_t *g_sensors[] = { &g_mock_sensor_ops };

void isc_setup(void)
{
    mock_regfile_reset();
    mock_fpga_reset();
    mock_sensor_reset();
    isc_init(&g_mock_port, &g_mock_fpga_ops, g_sensors, 1);
}

void isc_teardown(void)
{
    isc_deinit();
}
