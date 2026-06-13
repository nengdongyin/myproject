/**
 * @file    isc_port_axi.c
 * @brief   ISC 移植层 — AXI 直写参考实现
 *
 * 本文件不是框架代码。它是一份可在项目中直接使用或修改的参考模板。
 * 放在 src/camera/core/isc/port/ 下，编译时由用户决定是否链接。
 *
 * AXI 模式下 reg_addr 即完整内存映射地址, 无总线协议开销。
 * CPU 直接读写 FPGA 内部寄存器或传感器外挂寄存器。
 *
 * ── 使用方式 ──
 *   1. 实现 isc_axi_read() 和 isc_axi_write() — 直接内存映射访问
 *   2. 填充 g_axi 上下文 (axi_base 等)
 *   3. 将 isc_port_axi.c 加入你的编译列表
 *   4. 直接使用 g_axi_port 作为 isc_init() 的参数:
 *
 *        isc_init(&g_axi_port, &fpga_ops, sensors, count);
 *
 *   or 将其赋值给传感器的 ops->port:
 *
 *        .port = &g_axi_port,
 *
 * ═══════════════════════════════════════════════════════════════════════════
 *  用户需要实现的函数 (在另一个 .c 文件中)
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *   #include "isc_port.h"
 *
 *   // 传感器 AXI 寄存器读 — 直接内存映射访问
 *   int isc_axi_read(void *user_data,
 *                    const uint8_t *reg_addr, uint16_t reg_len,
 *                    uint8_t *data, uint16_t data_len);
 *
 *   // 传感器 AXI 寄存器写 — 直接内存映射访问
 *   int isc_axi_write(void *user_data,
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
 *   // AXI 上下文 — 包含你的 AXI 基址
 *   extern void *isc_axi_user_data;
 *
 * ═══════════════════════════════════════════════════════════════════════════
 */

#include <stddef.h>
#include "isc_port.h"
#include "isc_port_common.h"


/* ── AXI 上下文 (用户定义, 平台相关) ── */
typedef struct {
    void    *axi_base;    /* AXI-Lite 寄存器基址 */
} my_axi_t;

static my_axi_t g_axi = { .axi_base = (void *)0x40003000 };

/** @brief 传感器寄存器读 — 用户实现, 直接内存映射访问 */
static int isc_axi_read(void *user_data,
                        const uint8_t *reg_addr, uint16_t reg_len,
                        uint8_t *data, uint16_t data_len)
{
    my_axi_t *axi = (my_axi_t *)user_data;
    /** TODO: 直接内存映射读, 例如:
     *   uint32_t addr = (uint32_t)reg_addr;
     *   for (int i = 0; i < data_len; i++) {
     *       data[i] = *(volatile uint8_t *)(axi->axi_base + addr + i);
     *   }
     */
    (void)axi; (void)reg_addr; (void)reg_len; (void)data; (void)data_len;
    return 0;
}

/** @brief 传感器寄存器写 — 用户实现 */
static int isc_axi_write(void *user_data,
                         const uint8_t *reg_addr, uint16_t reg_len,
                         const uint8_t *data, uint16_t data_len)
{
    my_axi_t *axi = (my_axi_t *)user_data;
    /** TODO: 直接内存映射写, 例如:
     *   uint32_t addr = (uint32_t)reg_addr;
     *   for (int i = 0; i < data_len; i++) {
     *       *(volatile uint8_t *)(axi->axi_base + addr + i) = data[i];
     *   }
     */
    (void)axi; (void)reg_addr; (void)reg_len; (void)data; (void)data_len;
    return 0;
}

/* ── 全局 port 实例 ── */
isc_port_t g_axi_port = {
    .bus_type    = ISC_BUS_AXI,
    .read        = isc_axi_read,
    .write       = isc_axi_write,
    .cs_assert   = NULL,
    .cs_deassert = NULL,
    .delay_ms    = isc_delay_ms,
    .gpio_write  = isc_gpio_write,
    .user_data   = &g_axi,
};

/** @brief 可选: 在 main 中调用以注入 AXI 上下文 */
void isc_port_axi_init(void *user_data)
{
    g_axi_port.user_data = user_data;
}
