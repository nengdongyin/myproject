/**
 * @file    isc_port_i2c.c
 * @brief   ISC 移植层 — I2C 参考实现
 *
 * 本文件不是框架代码。它是一份可在项目中直接使用或修改的参考模板。
 * 放在 src/camera/core/isc/port/ 下，编译时由用户决定是否链接。
 *
 * ── 使用方式 ──
 *   1. 实现 isc_i2c_read() 和 isc_i2c_write() — 调用你的 I2C 驱动
 *   2. 填充 g_i2c 上下文 (dev_addr 等)
 *   3. 将 isc_port_i2c.c 加入你的编译列表
 *   4. 直接使用 g_i2c_port 作为 isc_init() 的参数:
 *
 *        isc_init(&g_i2c_port, &fpga_ops, sensors, count);
 *
 *   or 将其赋值给传感器的 ops->port:
 *
 *        .port = &g_i2c_port,
 *
 * ═══════════════════════════════════════════════════════════════════════════
 *  用户需要实现的函数 (在另一个 .c 文件中)
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *   #include "isc_port.h"
 *
 *   // 传感器 I2C 寄存器读 — 调用你的 I2C 驱动
 *   int isc_i2c_read(void *user_data,
 *                    const uint8_t *reg_addr, uint16_t reg_len,
 *                    uint8_t *data, uint16_t data_len);
 *
 *   // 传感器 I2C 寄存器写 — 调用你的 I2C 驱动
 *   int isc_i2c_write(void *user_data,
 *                     const uint8_t *reg_addr, uint16_t reg_len,
 *                     const uint8_t *data, uint16_t data_len);
 *
 *   // 平台延时
 *   void isc_delay_ms(uint32_t ms);
 *   void isc_delay_us(uint32_t us);
 *
 *   // GPIO 写 (PWDN/RESET/XCLR)
 *   int isc_gpio_write(uint8_t pin, uint8_t level);
 *
 *   // I2C 上下文 — 包含你的 I2C 控制器句柄 + 器件地址
 *   extern void *isc_i2c_user_data;
 *
 * ═══════════════════════════════════════════════════════════════════════════
 */

#include <stddef.h>
#include "isc_port.h"
#include "isc_port_common.h"


/* ── I2C 上下文 (用户定义, 平台相关) ── */
typedef struct {
    void    *i2c_base;    /* I2C 控制器基址 */
    uint8_t  dev_addr;    /* 7-bit 器件地址 */
} my_i2c_t;

static my_i2c_t g_i2c = { .i2c_base = (void *)0x40001000, .dev_addr = 0x1A };

/** @brief 传感器寄存器读 — 用户实现, 直接调用自己的 I2C 驱动 */
static int isc_i2c_read(void *user_data,
                       const uint8_t *reg_addr, uint16_t reg_len,
                       uint8_t *data, uint16_t data_len)
{
    my_i2c_t *i2c = (my_i2c_t *)user_data;
    /** TODO: 调用你的 I2C 驱动, 例如:
     *   i2c_read_reg(i2c->i2c_base, i2c->dev_addr,
     *                reg_addr, reg_len, data, data_len);
     */
    (void)i2c; (void)reg_addr; (void)reg_len; (void)data; (void)data_len;
    return 0;
}

/** @brief 传感器寄存器写 — 用户实现 */
static int isc_i2c_write(void *user_data,
                        const uint8_t *reg_addr, uint16_t reg_len,
                        const uint8_t *data, uint16_t data_len)
{
    my_i2c_t *i2c = (my_i2c_t *)user_data;
    (void)i2c; (void)reg_addr; (void)reg_len; (void)data; (void)data_len;
    return 0;
}
/* ── 全局 port 实例 ── */
isc_port_t g_i2c_port = {
    .bus_type   = ISC_BUS_I2C,
    .read       = isc_i2c_read,
    .write      = isc_i2c_write,
    .cs_assert  = NULL,
    .cs_deassert= NULL,
    .delay_ms   = isc_delay_ms,
    .gpio_write = isc_gpio_write,
    .user_data  = &g_i2c,  /* 由 _init 填充 */
};
