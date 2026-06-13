/**
 * @file    isc_port_spi.c
 * @brief   ISC 移植层 — SPI 参考实现
 *
 * 本文件不是框架代码。它是一份可在项目中直接使用或修改的参考模板。
 * 放在 src/camera/core/isc/port/ 下，编译时由用户决定是否链接。
 *
 * ── 使用方式 ──
 *   1. 实现 isc_spi_read() 和 isc_spi_write() — 调用你的 SPI 驱动
 *   2. 填充 g_spi 上下文 (spi_base, cs_pin 等)
 *   3. 将 isc_port_spi.c 加入你的编译列表
 *   4. 直接使用 g_spi_port 作为 isc_init() 的参数:
 *
 *        isc_init(&g_spi_port, &fpga_ops, sensors, count);
 *
 *   or 将其赋值给传感器的 ops->port:
 *
 *        .port = &g_spi_port,
 *
 * ═══════════════════════════════════════════════════════════════════════════
 *  用户需要实现的函数 (在另一个 .c 文件中)
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *   #include "isc_port.h"
 *
 *   // 传感器 SPI 寄存器读 — 调用你的 SPI 驱动
 *   int isc_spi_read(void *user_data,
 *                    const uint8_t *reg_addr, uint16_t reg_len,
 *                    uint8_t *data, uint16_t data_len);
 *
 *   // 传感器 SPI 寄存器写 — 调用你的 SPI 驱动
 *   int isc_spi_write(void *user_data,
 *                     const uint8_t *reg_addr, uint16_t reg_len,
 *                     const uint8_t *data, uint16_t data_len);
 *
 *   // SPI 片选控制
 *   int isc_spi_cs_assert(void *user_data);
 *   int isc_spi_cs_deassert(void *user_data);
 *
 *   // 平台延时
 *   void isc_delay_ms(uint32_t ms);
 *   void isc_delay_us(uint32_t us);
 *
 *   // GPIO 写 (PWDN/RESET/XCLR)
 *   int isc_gpio_write(uint8_t pin, uint8_t level);
 *
 *   // SPI 上下文 — 包含你的 SPI 控制器句柄 + 片选引脚
 *   extern void *isc_spi_user_data;
 *
 * ═══════════════════════════════════════════════════════════════════════════
 */

#include <stddef.h>
#include "isc_port.h"
#include "isc_port_common.h"


/* ── SPI 上下文 (用户定义, 平台相关) ── */
typedef struct {
    void    *spi_base;    /* SPI 控制器基址 */
    uint8_t  cs_pin;      /* 片选引脚编号 */
} my_spi_t;

static my_spi_t g_spi = { .spi_base = (void *)0x40002000, .cs_pin = 0 };

/** @brief SPI 片选拉低 — 用户实现 */
static int isc_spi_cs_assert(void *user_data)
{
    my_spi_t *spi = (my_spi_t *)user_data;
    /** TODO: 调用你的 GPIO 驱动拉低片选, 例如:
     *   gpio_set(spi->cs_pin, 0);
     */
    (void)spi;
    return 0;
}

/** @brief SPI 片选拉高 — 用户实现 */
static int isc_spi_cs_deassert(void *user_data)
{
    my_spi_t *spi = (my_spi_t *)user_data;
    /** TODO: 调用你的 GPIO 驱动拉高片选, 例如:
     *   gpio_set(spi->cs_pin, 1);
     */
    (void)spi;
    return 0;
}

/** @brief 传感器寄存器读 — 用户实现, 直接调用自己的 SPI 驱动 */
static int isc_spi_read(void *user_data,
                        const uint8_t *reg_addr, uint16_t reg_len,
                        uint8_t *data, uint16_t data_len)
{
    my_spi_t *spi = (my_spi_t *)user_data;
    /** TODO: 调用你的 SPI 驱动, 例如:
     *   spi_cs_assert();
     *   spi_write(reg_addr, reg_len);
     *   spi_read(data, data_len);
     *   spi_cs_deassert();
     */
    (void)spi; (void)reg_addr; (void)reg_len; (void)data; (void)data_len;
    return 0;
}

/** @brief 传感器寄存器写 — 用户实现 */
static int isc_spi_write(void *user_data,
                         const uint8_t *reg_addr, uint16_t reg_len,
                         const uint8_t *data, uint16_t data_len)
{
    my_spi_t *spi = (my_spi_t *)user_data;
    /** TODO: 调用你的 SPI 驱动, 例如:
     *   spi_cs_assert();
     *   spi_write(reg_addr, reg_len);
     *   spi_write(data, data_len);
     *   spi_cs_deassert();
     */
    (void)spi; (void)reg_addr; (void)reg_len; (void)data; (void)data_len;
    return 0;
}

/* ── 全局 port 实例 ── */
isc_port_t g_spi_port = {
    .bus_type    = ISC_BUS_SPI,
    .read        = isc_spi_read,
    .write       = isc_spi_write,
    .cs_assert   = isc_spi_cs_assert,
    .cs_deassert = isc_spi_cs_deassert,
    .delay_ms    = isc_delay_ms,
    .gpio_write  = isc_gpio_write,
    .user_data   = &g_spi,
};

/** @brief 可选: 在 main 中调用以注入 SPI 上下文 */
void isc_port_spi_init(void *user_data)
{
    g_spi_port.user_data = user_data;
}
