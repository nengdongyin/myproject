/**
 * @file    isc_port.h
 * @brief   ISC 移植层接口
 *
 * 定义传感器通信原语 (isc_port_t) 和 FPGA 同步控制接口 (isc_fpga_ops_t)。
 * 用户负责实现其中的函数指针并将其传入 isc_init()。
 */

#ifndef ISC_PORT_H
#define ISC_PORT_H

#include <stdint.h>

/* ──── 总线类型 ──── */
typedef enum {
    ISC_BUS_I2C = 0,    /**< I2C 总线: reg_addr 低 16-bit 为寄存器地址       */
    ISC_BUS_SPI = 1,    /**< SPI 总线: reg_addr 按传感器约定编码              */
    ISC_BUS_AXI = 2,    /**< FPGA AXI 直写: reg_addr 为完整内存映射地址       */
} isc_bus_type_t;

/**
 * @brief 通用传感器通信接口 — 由用户实现
 *
 * user_data 典型用法:
 *   - I2C: user_data = &{i2c_controller, dev_addr}
 *   - SPI: user_data = &{spi_controller, cs_pin}
 *   - AXI: user_data = &{axi_base_address}
 */
typedef struct isc_port {
    isc_bus_type_t bus_type;       /**< 总线类型                              */

    /** @brief 传感器寄存器读
     *  @param[in]  user_data  平台私有数据
     *  @param[in]  reg_addr   寄存器地址字节流 (大端序)
     *  @param[in]  reg_len    寄存器地址字节数
     *  @param[out] data       读取缓冲区
     *  @param[in]  data_len   读取字节数
     *  @return 0=成功, <0=平台错误码
     */
    int (*read)(void *user_data,
                const uint8_t *reg_addr, uint16_t reg_len,
                uint8_t *data, uint16_t data_len);

    /** @brief 传感器寄存器写
     *  @param[in] user_data  平台私有数据
     *  @param[in] reg_addr   寄存器地址字节流 (大端序)
     *  @param[in] reg_len    寄存器地址字节数
     *  @param[in] data       写入数据
     *  @param[in] data_len   写入字节数
     *  @return 0=成功, <0=平台错误码
     */
    int (*write)(void *user_data,
                 const uint8_t *reg_addr, uint16_t reg_len,
                 const uint8_t *data, uint16_t data_len);

    /* ── SPI 专用 (ISC_BUS_SPI 时由框架/驱动调用, 其它填 NULL) ── */
    /** @brief 拉低 SPI 片选 */
    int (*cs_assert)(void *user_data);
    /** @brief 拉高 SPI 片选 */
    int (*cs_deassert)(void *user_data);

    /* ── 时序 ── */
    /** @brief 毫秒级阻塞延时 */
    void (*delay_ms)(uint32_t ms);

    /* ── GPIO ── */
    /** @brief GPIO 电平设置 (PWDN / RESET / XCLR 等)
     *  @param[in] pin   引脚编号 (由用户定义映射)
     *  @param[in] level 0=低电平, 1=高电平
     *  @return 0=成功
     */
    int (*gpio_write)(uint8_t pin, uint8_t level);

    void *user_data;               /**< 用户私有数据 (I2C 句柄/SPI 基址等)     */
} isc_port_t;

/* ──── FPGA 同步/控制接口 ──── */

/* 前置声明 (类型定义在 isc.h 中, 须与 isc_sensor_ops.h 的 struct tag 一致) */
typedef struct isc_fmt isc_fmt_t;

/**
 * @brief FPGA 同步/控制接口 — 由用户实现
 *
 * 所有 CPU→FPGA 通信通过此单一 ioctl 通道。
 * ISC 框架主动调用（格式变更、流启停），传感器驱动也可借用
 * 请求 FPGA 操作（触发控制等）。
 */
typedef struct isc_fpga_ops {
    /** @brief 通用控制
     *  @param[in] cmd       命令码 (ISC_FPGA_*)
     *  @param[in] arg       命令参数 (类型由 cmd 决定)
     *  @param[in] user_data 用户私有数据
     *  @return 0=成功
     */
    int (*ioctl)(uint32_t cmd, void *arg, void *user_data);

    void *user_data;               /**< 用户私有数据 (AXI 寄存器基址等)       */
} isc_fpga_ops_t;

/* ──── 框架调用 (ISC → FPGA) ──── */
#define ISC_FPGA_FORMAT_CHANGED    0x0001  /**< arg: const struct isc_fmt_t *fmt     */
#define ISC_FPGA_STREAM_STATE      0x0002  /**< arg: uint8_t streaming (0=停, 1=启) */

/* ──── 驱动调用 (sensor driver → FPGA) ──── */
#define ISC_FPGA_TRIGGER_SET       0x0003  /**< arg: uint8_t enable (0=关, 1=开)    */

/* ──── 厂商扩展 ──── */
#define ISC_FPGA_PRIVATE_BASE      0x0010  /**< 厂商扩展命令起点 (0x0010–0xFFFF)    */

#endif /* ISC_PORT_H */
