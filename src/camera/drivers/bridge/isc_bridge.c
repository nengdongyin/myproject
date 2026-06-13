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
 *
 *  ⚠ 测试桩局限: null_read 在 CHIP_ID 寄存器返回 IMX477 的 ID 以通过 probe。
 *  如需注册其他传感器, 必须在 null_read 中增加对应 CHIP_ID 映射。
 *  生产环境应替换为真实 I2C 驱动, 不需要此测试桩。
 * ═══════════════════════════════════════════════════════════════════════ */

/* IMX477 chip ID (16-bit big-endian: 0x0477 at reg 0x0016) */
#define IMX477_CHIP_ID_REG  0x0016u
#define IMX477_CHIP_ID_VAL  0x0477u

static int null_read(void *u,
                     const uint8_t *reg_addr, uint16_t reg_len,
                     uint8_t *data, uint16_t data_len)
{
    (void)u; (void)reg_len;
    /* IMX477 CHIP_ID at reg 0x0016 (big-endian 2-byte) */
    if (reg_len >= 2 && reg_addr[0] == 0x00 && reg_addr[1] == 0x16
        && data_len >= 2) {
        data[0] = (uint8_t)((IMX477_CHIP_ID_VAL >> 8) & 0xFF);
        data[1] = (uint8_t)(IMX477_CHIP_ID_VAL & 0xFF);
    }
    if (data_len > 2) memset(data + 2, 0, data_len - 2);
    return 0;
}

static int null_write(void *u,
                      const uint8_t *reg_addr, uint16_t reg_len,
                      const uint8_t *data, uint16_t data_len)
{ (void)u; (void)reg_addr; (void)reg_len; (void)data; (void)data_len; return 0; }

static void null_delay_ms(uint32_t ms)  { (void)ms; }
static int  null_gpio(uint8_t pin, uint8_t level) { (void)pin; (void)level; return 0; }

static int null_fpga_ioctl(uint32_t cmd, void *arg, void *user_data)
{ (void)cmd; (void)arg; (void)user_data; return 0; }

static isc_port_t g_port = {
    .bus_type   = ISC_BUS_I2C,
    .read       = null_read,
    .write      = null_write,
    .delay_ms   = null_delay_ms,
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

int isc_bridge_init(void)
{
    if (isc_is_initialized()) return 0;

    int rc = isc_init(&g_port, &g_fpga_ops,
                      g_sensors,
                      sizeof(g_sensors) / sizeof(g_sensors[0]));
    return (rc == ISC_OK) ? 0 : rc;
}

bool isc_bridge_is_ready(void)
{
    return (bool)isc_is_initialized();
}
