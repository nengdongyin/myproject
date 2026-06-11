/**
 * @file    isc_bridge.c
 * @brief   ISC 桥接模块 — 初始化 ISC 框架，注册传感器驱动。
 *
 * VSC 通过本模块桥接 ISC。使用前调用 isc_bridge_init()。
 * 测试环境使用空实现 port/fpga_ops（无真实硬件 I2C）。
 */

#include "isc.h"
#include <stdbool.h>
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════
 *  空实现 port / fpga_ops（测试环境 — 替换为真实 I2C 操作）
 * ═══════════════════════════════════════════════════════════════════════ */

/* IMX477 chip ID registers (16-bit big-endian: 0x0477 at reg 0x0016) */
#define IMX477_CHIP_ID_REG  0x0016u
#define IMX477_CHIP_ID_VAL  0x0477u

static int null_read(void *u, uint32_t reg, uint8_t *data, uint16_t len)
{
    (void)u;
    /* return valid chip ID for IMX477 probe at reg 0x0016 */
    if (reg == IMX477_CHIP_ID_REG && len >= 2) {
        data[0] = (IMX477_CHIP_ID_VAL >> 8) & 0xFF;
        data[1] = IMX477_CHIP_ID_VAL & 0xFF;
    }
    /* for all other registers: return 0 (standby/safe values) */
    if (len > 2) memset(data + 2, 0, len - 2);
    if (reg != IMX477_CHIP_ID_REG) memset(data, 0, len);
    (void)reg;
    return 0;
}

static int null_write(void *u, uint32_t reg, const uint8_t *data, uint16_t len)
{ (void)u; (void)reg; (void)data; (void)len; return 0; }

static void null_delay_ms(uint32_t ms)  { (void)ms; }
static void null_delay_us(uint32_t us)  { (void)us; }
static int  null_gpio(uint8_t pin, uint8_t level) { (void)pin; (void)level; return 0; }

static int null_fpga_ioctl(uint32_t cmd, void *arg, void *user_data)
{ (void)cmd; (void)arg; (void)user_data; return 0; }

static isc_port_t g_port = {
    .bus_type   = ISC_BUS_I2C,
    .read       = null_read,
    .write      = null_write,
    .delay_ms   = null_delay_ms,
    .delay_us   = null_delay_us,
    .gpio_write = null_gpio,
};

static isc_fpga_ops_t g_fpga_ops = {
    .ioctl = null_fpga_ioctl,
};

/* ═══════════════════════════════════════════════════════════════════════
 *  传感器驱动注册表
 * ═══════════════════════════════════════════════════════════════════════ */

/* 由 isc_sensor_imx477.c 定义 */
extern const isc_sensor_ops_t isc_sensor_imx477;

static const isc_sensor_ops_t *g_sensors[] = {
    &isc_sensor_imx477,
};

/* ═══════════════════════════════════════════════════════════════════════
 *  公开 API
 * ═══════════════════════════════════════════════════════════════════════ */

static bool g_bridge_ready = false;

int isc_bridge_init(void)
{
    if (g_bridge_ready) return 0;

    int rc = isc_init(&g_port, &g_fpga_ops,
                      g_sensors,
                      sizeof(g_sensors) / sizeof(g_sensors[0]));
    if (rc != ISC_OK) return rc;

    g_bridge_ready = true;
    return 0;
}

bool isc_bridge_is_ready(void)
{
    return g_bridge_ready;
}
