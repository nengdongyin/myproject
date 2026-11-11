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
 * @par 单一清单 (X-macro)
 * 新增模块只需在 PARAM_MODULE_LIST 中**登记一行** (追加在末尾, 不要在中间
 * 插入): 模块 ID 枚举、MODULE_INIT_ORDER 均由此清单生成 —— 漏登记即
 * 编译期缺席, 不存在"定义了 ID 却忘了进 ORDER"的静默落序。
 * @{
 */

/**
 * @brief 模块单一清单 (X-macro) — ID 与顺序表的唯一来源
 *
 * @param X(name, id, init_fn, required) 每行: 模块标识符名、ID 数值、
 *        注册函数名、注册标志。
 *   - init_fn:  完整注册函数名 (无参 void f(void)), 由 register_modules
 *               清单驱动直接调用; 与模块 .c 里 PARAM_MODULE_INIT 生成的
 *               xxx_module_init 一致, 或手动定义的注册函数名。
 *   - required: 1 = 由 register_modules 清单驱动自动注册;
 *               0 = 不进入 register_modules (框架内置模块 MODULE_META/
 *               MODULE_SYS 由 param_service_init 按特殊时序注册, 或预留
 *               未实现模块)。
 *
 * @attention 行序**即** MODULE_INIT_ORDER 的初始化/flush 顺序:
 *   IP 硬件在前、业务模块在后。ID 数值不必
 *   按行序递增 (枚举成员显式赋值), 调整行序前确认加载/刷硬件顺序契约。
 */
#define PARAM_MODULE_LIST(X) \
    X(MODULE_META,      0x00u, param_meta_module_init,        0) \
    X(IP_SENSOR,        0x80u, sensor_module_init,            1) \
    X(IP_ACQUISITION,   0x81u, ip_acquisition_module_init,    0) \
    X(IP_LVDS_RX,       0x82u, ip_lvds_rx_module_init,        0) \
    X(IP_PWM,           0x83u, ip_pwm_module_init,            0) \
    X(MODULE_VSC_LITE,  0x03u, vsc_lite_control_module_init,  0) \
    X(MODULE_AUTO_EXP,  0x01u, auto_exp_module_init,          1) \
    X(MODULE_SYS,       0x02u, param_service_module_init,     0)

/**
 * @brief 清单行数计数器 — 每个 X(...) 展开为 "+1"
 *
 * 与 PARAM_MODULE_ID_ENUM 配套使用: `0 +1 +1 ...` 得到真实模块总数。
 * 计数只依赖清单**行数**, 与各模块 ID 数值无关 (ID 是稀疏的)。
 */
#define PARAM_MODULE_COUNT_X(name, id, cname, required) +1

/**
 * @brief 模块 ID 枚举 (由清单生成)
 *
 * App 业务模块: 0x01~0x7F; IP 寄存器模块: 0x80~0xFF;
 * MODULE_META (0x00) 为框架元数据。
 * 枚举常量可用于 MAKE_PARAM_ID / switch-case / 数组初始化。
 */
typedef enum
{
#define PARAM_MODULE_ID_ENUM(name, id, cname, required) name = id,
    PARAM_MODULE_LIST(PARAM_MODULE_ID_ENUM)
#undef PARAM_MODULE_ID_ENUM
    /** @brief 清单登记的真实模块总数 (PARAM_MODULE_LIST 行数, 增删行自动同步)
     *
     *  @note 这是"稠密行数", **不是** module_id 上界: module_id 是稀疏的
     *        (0x00 / 0x01~0x03 / 0x80~0x83), 不得拿本值做以 module_id
     *        为下标的数组容量。
     *  @note 不要用"前一个枚举值 + 1"推导总数 (旧实现如此): 清单末行为
     *        MODULE_SYS=0x02 时会算成 3, 与 MODULE_VSC_LITE 撞值。
     */
    PARAM_MODULE_COUNT = 0 PARAM_MODULE_LIST(PARAM_MODULE_COUNT_X)
} param_module_id_t;

#undef PARAM_MODULE_COUNT_X

#define PID_SCHEMA_VER MAKE_PARAM_ID(MODULE_META, 0x00) /**< 存储格式版本号 */

/**
 * @brief 模块初始化和 flush 的顺序表 (由同一清单生成)
 *
 * 决定 param_load_all() 的 init 阶段以及 param_flush() 的遍历顺序。
 * 越靠前越先执行。IP 硬件在前，业务模块在后。
 * 未注册的模块自动跳过，不报错。
 *
 * MODULE_META 必须列在首位且**不得缺省**：
 *   1. 它是框架元数据（存储格式版本号），没有硬件可刷，先执行无副作用；
 *   2. param_check_module_order() 把"已注册但不在本表"的模块计入
 *      flush_order_miss_count 并返回 PARAM_ERR_NOT_FOUND，遗漏会让自检失败。
 */
#define PARAM_MODULE_ORDER_X(name, id, cname, required) name,
/** 顺序表展开 = 清单成员名的逗号列表 (由 PARAM_MODULE_LIST 生成) */
#define MODULE_INIT_ORDER PARAM_MODULE_LIST(PARAM_MODULE_ORDER_X)

/** @} */ /* module_ids */
#ifdef __cplusplus
}
#endif

#endif /* MODULE_IDS_H */
