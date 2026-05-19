#ifndef MODULE_IDS_H
#define MODULE_IDS_H

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * @file module_ids.h
 * @brief 模块 ID 集中定义与初始化顺序表
 * @defgroup module_ids 模块标识符定义
 *
 * 所有模块的 ID 在此统一分配，确保 ID 永不漂移。
 * ID 编码: [31:16] = module_id, [15:0] = param_local_id。
 *
 * ID 分配规则:
 *   - 业务参数模块 (App): 0x01 ~ 0x7F
 *   - IP 寄存器模块:         0x80 ~ 0xFF
 *
 * 每新增模块请追加在末尾，不要在中间插入。
 * @{
 */

/** @name 业务逻辑模块 (App) */
/** @{ */
#define MODULE_AUTO_EXP 0x01u /**< 自动曝光控制模块 */
#define MODULE_SYS 0x02u      /**< 系统管理模块 */
/** @} */

/** @name IP 寄存器模块 (0x80 起) */
/** @{ */
#define IP_SENSOR 0x80u      /**< 传感器 IP */
#define IP_ACQUISITION 0x81u /**< 采集 IP */
#define IP_LVDS_RX 0x82u     /**< LVDS 接收 IP */
#define IP_PWM 0x83u         /**< PWM 输出 IP */
/** @} */

/**
 * @brief 模块初始化和 flush 的顺序表
 *
 * 决定 param_load_all() 的 init 阶段以及 param_flush() 的遍历顺序。
 * 越靠前越先执行。IP 硬件在前，业务模块在后。
 * 未注册的模块自动跳过，不报错。
 */
#define MODULE_INIT_ORDER \
    IP_SENSOR,            \
        IP_ACQUISITION,   \
        IP_LVDS_RX,       \
        IP_PWM,           \
        MODULE_AUTO_EXP,  \
        MODULE_SYS

/** @} */ /* module_ids */
#ifdef __cplusplus
}
#endif

#endif /* MODULE_IDS_H */
