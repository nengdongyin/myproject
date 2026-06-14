/**
 * @file    isc_sensor_ops.h
 * @brief   传感器驱动操作表 (Sensor Ops — 多态虚表)
 * @details 采用策略模式 (Strategy Pattern) + 编译期多态。
 *
 *          每个传感器型号提供一个该结构体的静态常量实例，通过 isc_init() 注册。
 *          未实现的可选操作填 NULL, ISC 核心返回 ISC_ERR_NOT_SUPPORTED。
 *          所有函数指针指向静态函数, 无动态分配 (S0 内存策略)。
 *
 * @section lock_model 锁与回调模型
 *
 *          ISC 使用 system_lock() / system_unlock() 保护全局状态的互斥访问,
 *          保护对象为 g_isc 和 dev 内部字段。I2C/SPI 外设的并发保护
 *          应由 isc_port_t 的实现层自行负责, 不在 ISC 框架职责范围内。
 *
 *          <b>所有驱动回调均在持锁状态下被调用</b>, 包括:
 *          probe, init, deinit, enum_fmts, get_fmt, set_fmt, try_fmt,
 *          query_ctrl, get_ctrl, set_ctrl, stream_on, stream_off,
 *          query_timing, try_timing, query_constraint。
 *
 *          驱动回调的约束:
 *          - 不得调用任何 ISC 公共 API (会二次加锁导致死锁)
 *          - 不得长时间阻塞 (会阻塞其他 ISC 操作)
 *          - I2C/SPI 通信通常 < 1ms, 是安全的; 禁止在回调中做 ms 级轮询
 *
 *          on_ctrl_change 回调在锁外执行, 可以安全地调用 ISC API。
 *
 * @see isc_dev_t, isc_init(), isc_register_ctrl_callback()
 */

#ifndef ISC_SENSOR_OPS_H
#define ISC_SENSOR_OPS_H

#include <stdint.h>
#include "isc_port.h"

/* 前置声明 (完整类型定义在 isc.h 中, struct tag 须与 isc.h 一致) */
typedef struct isc_dev_t       isc_dev_t;
typedef struct isc_fmt         isc_fmt_t;
typedef struct isc_fmt_desc    isc_fmt_desc_t;
typedef struct isc_timing      isc_timing_t;
typedef struct isc_ctrl_desc   isc_ctrl_desc_t;
typedef union  isc_ctrl_value  isc_ctrl_value_t;

/**
 * @brief 传感器驱动操作表
 */
typedef struct isc_sensor_ops {
    /* ── 标识 ── */
    const char *model;              /**< 型号名, 与 isc_open() 的 model 参数匹配  */
    const char *vendor;             /**< 厂商名                                  */
    uint32_t    capabilities;       /**< ISC_CAP_* 位掩码 (传感器主动声明)        */

    /**
     * @brief 本传感器使用的通信接口 (可选)
     *
     * 非 NULL 时覆盖 isc_init() 传入的全局 port。
     * 允许不同传感器使用不同总线 (I2C/SPI) 或不同 I2C 地址。
     * NULL 时回退到全局 port (向后兼容单传感器系统)。
     */
    const isc_port_t *port;

    /**
     * @brief 本传感器使用的 FPGA 同步接口 (可选)
     *
     * 非 NULL 时覆盖 isc_init() 传入的全局 fpga_ops。
     * NULL 时回退到全局 fpga_ops (向后兼容)。
     */
    const isc_fpga_ops_t *fpga_ops;

    /* ── 生命周期 ── */
    /** @brief 读 CHIP_ID 寄存器确认传感器型号 (必须)
     *  @return ISC_OK / ISC_ERR_IO
     */
    int (*probe)(isc_dev_t *dev);

    /** @brief 上电时序 + 写入初始寄存器序列 (必须)
     *  @return ISC_OK / ISC_ERR_*
     */
    int (*init)(isc_dev_t *dev);

    /** @brief 下电、释放资源 (可选, NULL 则仅操作 GPIO) */
    int (*deinit)(isc_dev_t *dev);

    /** @brief 软复位 (可选, NULL 降级为 deinit+init) */
    int (*reset)(isc_dev_t *dev);

    /* ── 格式 ── */
    /** @brief 枚举支持的像素格式 (必须)
     *  @param[in]  index  格式索引 (0-based, 递增至 ISC_ENUM_END)
     *  @param[out] desc   格式描述 (含约束)
     */
    int (*enum_fmts)(isc_dev_t *dev, uint8_t index,
                     isc_fmt_desc_t *desc);

    /** @brief 获取当前生效格式 (必须) */
    int (*get_fmt)(isc_dev_t *dev, isc_fmt_t *fmt);

    /** @brief 设置新格式 (必须) */
    int (*set_fmt)(isc_dev_t *dev, const isc_fmt_t *fmt);

    /** @brief 试探格式 (可选, NULL→核心用 set_fmt 回滚模拟) */
    int (*try_fmt)(isc_dev_t *dev, isc_fmt_t *fmt);

    /* ── 控制 ── */
    /** @brief 查询控制项属性 (必须)
     *  @note 驱动仅处理标准 CID 或私有 CID,
     *        枚举由框架层 isc_query_next_ctrl() 负责。
     */
    int (*query_ctrl)(isc_dev_t *dev, isc_ctrl_desc_t *desc);

    /** @brief 读取控制值 (必须) */
    int (*get_ctrl)(isc_dev_t *dev, uint32_t cid,
                    isc_ctrl_value_t *val);

    /** @brief 设置控制值 (必须) */
    int (*set_ctrl)(isc_dev_t *dev, uint32_t cid,
                    isc_ctrl_value_t val);

    /** @brief 查询 ENUM 型控制项的菜单名称 (可选, NULL 则 isc_query_menu 返回 NOT_SUPPORTED)
     *  @param[in]  cid   控制 ID
     *  @param[in]  index 菜单索引 (0..max-1)
     *  @param[out] name  菜单名称 (调用者提供缓冲区)
     */
    int (*query_menu)(isc_dev_t *dev, uint32_t cid, uint32_t index, char *name);

    /* ── 流 ── */
    /** @brief 启动传感器数据输出 (必须) */
    int (*stream_on)(isc_dev_t *dev);

    /** @brief 停止传感器数据输出 (必须) */
    int (*stream_off)(isc_dev_t *dev);

    /* ── 物理状态与约束 ── */
    /** @brief 查询传感器物理时序 (必须) */
    int (*query_timing)(isc_dev_t *dev, isc_timing_t *timing);

    /** @brief 试探指定格式下的预期物理时序 (可选, NULL→核心回退模拟)
     *  @param[in]  fmt    试探格式
     *  @param[out] timing 预期物理时序
     */
    int (*try_timing)(isc_dev_t *dev, const isc_fmt_t *fmt, isc_timing_t *timing);

    /** @brief 查询传感器约束 (可选, NULL 表示无特殊约束)
     *  @param[in]  type      约束类型 ID (厂商头文件定义)
     *  @param[in]  index     同类型约束索引 (0-based)
     *  @param[out] data      约束数据 (类型由 type 决定)
     *  @param[in]  data_size 缓冲区字节数 (由框架传入, 驱动可校验)
     */
    int (*query_constraint)(isc_dev_t *dev, uint32_t type,
                            uint32_t index, void *data,
                            uint32_t data_size);

    /* ── 扩展 ── */
    /** @brief 传感器专属操作 (可选)
     *  @param[in]  cmd 命令码 (FourCC 风格, 厂商定义)
     *  @param[in,out] arg 命令参数
     */
    int (*sensor_ioctl)(isc_dev_t *dev, uint32_t cmd, void *arg);
} isc_sensor_ops_t;

#endif /* ISC_SENSOR_OPS_H */
