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
 * @param X(name, id) 每行: 模块标识符名与 ID 数值
 *
 * @attention 行序**即** MODULE_INIT_ORDER 的初始化/flush 顺序:
 *   IP 硬件在前、业务模块在后。ID 数值不必
 *   按行序递增 (枚举成员显式赋值), 调整行序前确认加载/刷硬件顺序契约。
 */
#define PARAM_MODULE_LIST(X) \
    X(MODULE_META,      0x00u) \
    X(IP_SENSOR,        0x80u) \
    X(IP_ACQUISITION,   0x81u) \
    X(IP_LVDS_RX,       0x82u) \
    X(IP_PWM,           0x83u) \
    X(MODULE_VSC_LITE,  0x03u) \
    X(MODULE_AUTO_EXP,  0x01u) \
    X(MODULE_SYS,       0x02u)

/**
 * @brief 模块 ID 枚举 (由清单生成)
 *
 * App 业务模块: 0x01~0x7F; IP 寄存器模块: 0x80~0xFF;
 * MODULE_META (0x00) 为框架元数据。
 * 枚举常量可用于 MAKE_PARAM_ID / switch-case / 数组初始化。
 */
typedef enum
{
#define PARAM_MODULE_ID_ENUM(name, id) name = id,
    PARAM_MODULE_LIST(PARAM_MODULE_ID_ENUM)
#undef PARAM_MODULE_ID_ENUM
    PARAM_MODULE_ID_COUNT
} param_module_id_t;

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
#define PARAM_MODULE_ORDER_X(name, id) name,
/** 顺序表展开 = 清单成员名的逗号列表 (由 PARAM_MODULE_LIST 生成) */
#define MODULE_INIT_ORDER PARAM_MODULE_LIST(PARAM_MODULE_ORDER_X)

/** @} */ /* module_ids */
#ifdef __cplusplus
}
#endif

#endif /* MODULE_IDS_H */
