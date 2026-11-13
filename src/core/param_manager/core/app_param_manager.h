#ifndef APP_PARAM_MANAGER_H
#define APP_PARAM_MANAGER_H

#include "param_manager.h"
#include "param_manager_internal.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @file app_param_manager.h
     * @brief App (业务逻辑) 参数子系统的类型定义、宏与注册接口
     *
     * App 参数按类型分为 5 种派生结构体 (大小为 32-bit 平台典型值):
     *   - param_range_entry_t  (32B) — UINT / INT / FLOAT (带范围校验)
     *   - param_enum_entry_t   (28B) — ENUM (带枚举值校验)
     *   - param_bool_entry_t   (20B) — BOOL (无校验)
     *   - param_blob_entry_t   (24B) — BLOB (字节块，二进制数据)
     *
     * 所有类型的前 sizeof(param_entry_head_t) 字节公共头部 (PARAM_ENTRY_HEAD) 完全一致，
     * 对外统一通过 param_entry_t * 基类指针和 param_write / param_read 操作。
     *
     * 针对每一参数类型的操作函数 (pre_write / cache_update / read / save / load / reset)
     * 通过编译期常量分派表 g_type_handlers[PARAM_TYPE_COUNT] 统一调度，消除 switch-case。
     */

    /* ================================================================
     *  类型前置声明 / 回调类型 / 模块结构体 / 注册接口
     *
     *  App 侧无独立内容 (与 ip_param_manager.h 对应位置对称):
     *   - 前置声明:   param_module_t 定义于 param_manager.h
     *   - 回调类型:   param_read_fn / param_write_fn / param_flush_fn /
     *                 param_init_fn / param_exec_fn 定义于 param_manager.h
     *   - 模块结构体: struct param_module 定义于 param_manager.h
     *   - 注册接口:   param_module_register 声明于 param_manager.h
     * ================================================================ */

/* 本子系统声明 App 派生侧的 vtable 常量: 基类头文件 (param_manager.h)
   只命名基类符号, 派生侧符号由各自的派生头文件声明 (依赖方向
   派生 → 基类, 无反向)。下方的 PARAM_APP_* / PARAM_APP_MODULE_DEFINE 宏
   在建表时引用这两个常量, 因此声明必须出现在宏定义之前。 */

    /** @brief App 参数级 vtable 实现 (定义于 app_param_manager.c) */
    extern const param_vtable_t app_vtable;
    /** @brief App 模块级 vtable 实现 (定义于 app_param_manager.c) */
    extern const param_module_vtable_t app_module_vtable;

/**
 * @name App 参数条目定义宏
 *
 * 每个宏定义一个 static 的 xxx_entry_t 变量 (位于 .data/.bss, 由宏参数
 * 完成编译期初始化, 无运行时构造)。条目由 PARAM_TABLE 收集后随模块注册,
 * 框架不产生任何动态分配。
 * @{
 */

/**
 * @brief 定义 UINT 类型参数
 *
 * @param _name  变量名
 * @param _id    参数 ID (MAKE_PARAM_ID)
 * @param _flags 属性标志 (PARAM_FLAG_PERSIST 等)
 * @param _def   默认值
 * @param _min   最小值
 * @param _max   最大值 (若 _min >= _max 则不启用范围校验)
 */
#define PARAM_APP_UINT(_name, _id, _flags, _def, _min, _max) \
    PARAM_ENTRY_RANGE_UINT(&app_vtable, _name, _id, _flags, _def, _min, _max)

/** @brief 定义 INT 类型参数 */
#define PARAM_APP_INT(_name, _id, _flags, _def, _min, _max) \
    PARAM_ENTRY_RANGE_INT(&app_vtable, _name, _id, _flags, _def, _min, _max)

/** @brief 定义 FLOAT 类型参数 */
#define PARAM_APP_FLOAT(_name, _id, _flags, _def, _min, _max) \
    PARAM_ENTRY_RANGE_FLOAT(&app_vtable, _name, _id, _flags, _def, _min, _max)

/** @brief 定义 BOOL 类型参数 */
#define PARAM_APP_BOOL(_name, _id, _flags, _def) \
    PARAM_ENTRY_BOOL(&app_vtable, _name, _id, _flags, _def)

/** @brief 定义 ENUM 类型参数 */
#define PARAM_APP_ENUM(_name, _id, _flags, _def, _vals, _cnt) \
    PARAM_ENTRY_ENUM(&app_vtable, _name, _id, _flags, _def, _vals, _cnt)

/**
 * @brief 定义 BLOB 类型参数
 *
 * @attention 缓存与默认值共享 _def_ptr 指向的单一缓冲区 (单缓冲配置):
 *   写入后 reset 不会恢复初始默认内容 (自拷贝被跳过)。框架有意不提供
 *   双缓冲默认值方案 — BLOB 通常体积较大, 双备份的存储收益有限。
 *
 * @param _name    变量名
 * @param _id      参数 ID
 * @param _flags   属性标志
 * @param _def_ptr 默认值缓冲区 (静态分配)
 * @param _size    blob 大小
 */
#define PARAM_APP_BLOB(_name, _id, _flags, _def_ptr, _size) \
    PARAM_ENTRY_BLOB(&app_vtable, _name, _id, _flags, _def_ptr, _size)

/**
 * @brief 定义 STRING 类型参数 — 缓存指向调用方静态缓冲区
 *
 * @attention _def 必须为可写静态字符数组的地址 char[_max_len+1]
 *   (由调用方初始化默认内容)。严禁传入字符串字面量: 字面量位于
 *   .rodata, 首次写入将触发写保护故障。
 *   缓存与默认值共享该缓冲 (单缓冲配置), 写入后 reset 无法恢复
 *   初始默认内容 (自拷贝被跳过, 仅清 dirty)。
 *
 * @param _name    变量名
 * @param _id      参数 ID
 * @param _flags   属性标志
 * @param _def     可写静态字符缓冲区地址 (长 _max_len+1, 调用方初始化)
 * @param _max_len 最大字符数 (不含 '\\0')
 */
#define PARAM_APP_STRING(_name, _id, _flags, _def, _max_len) \
    PARAM_ENTRY_STRING(&app_vtable, _name, _id, _flags, (char *)(_def), _max_len)

/**
 * @brief 定义 App EXEC 命令
 *
 * 注册一个 PARAM_FLAG_EXEC 标记的伪参数条目。不可通过 param_write 写入，
 * 仅由 param_exec 触发。框架保证 param_write_raw 遇到 EXEC 参数时自动
 * 路由到模块的 exec 回调，arg 为 param_value_t 联合体 (.ptr 指向原始 data)。
 * dump 输出为 "EXEC" (不显示值)，flags 列显示 "E"。
 *
 * @param _name 命令变量名
 * @param _id   命令 ID (MAKE_PARAM_ID 风格)
 */
#define PARAM_APP_EXEC(_name, _id)      \
    PARAM_ENTRY_EXEC(&app_vtable, _name, _id)

    /** @} */

    /* ================================================================
     *  传统拼写别名
     *
     *  与 IP 侧 PARAM_IP_* 对称的规范名为 PARAM_APP_*; PARAM_* 是它的
     *  等价别名, 供既有代码继续使用。新代码请直接用 PARAM_APP_*。
     * ================================================================ */
#define PARAM_UINT   PARAM_APP_UINT
#define PARAM_INT    PARAM_APP_INT
#define PARAM_FLOAT  PARAM_APP_FLOAT
#define PARAM_BOOL   PARAM_APP_BOOL
#define PARAM_ENUM   PARAM_APP_ENUM
#define PARAM_BLOB   PARAM_APP_BLOB
#define PARAM_STRING PARAM_APP_STRING
#define PARAM_EXEC   PARAM_APP_EXEC

/**
 * @brief 定义一个 App 模块实例 (静态分配)
 *
 * 使用示例:
 * @code
 * PARAM_APP_MODULE_DEFINE(auto_exp, MODULE_AUTO_EXP, "AutoExposure",
 *                     .ctx   = &g_ae_instance,
 *                     .init  = ae_init,
 *                     .write = ae_write,
 *                     .flush = ae_flush);
 * @endcode
 *
 * App 与 IP 回调语义对称，按生命周期排列:
 *   init  (可 NULL) — 模块初始化
 *   read  (可 NULL) — 自定义读取 (实时计算/硬件查询); NULL=直接返缓存
 *   write           — 校验/转换写入值; 返回 PARAM_OK 放行
 *   exec  (可 NULL) — 命令执行
 *   flush           — 刷入硬件
 *
 * @param _mod_name   模块变量名 (生成 _mod_name##_module)
 * @param _mod_id     模块 ID (来自 module_ids.h)
 * @param _label      模块调试名称
 * @param ...         具名字段初始化器 (.ctx / .init / .read / .write /
 *                    .exec / .flush; 省略字段为 NULL —— 具名可任意顺序,
 *                    杜绝位置参数写反)
 */
#define PARAM_APP_MODULE_DEFINE(_mod_name, _mod_id, _label, ...)      \
    PARAM_MODULE_DEFINE_CORE(&app_module_vtable, _mod_name##_module,  \
                             _mod_id, _label, __VA_ARGS__)

/* 传统拼写别名: 规范名为 PARAM_APP_MODULE_DEFINE (与 IP 侧
   PARAM_IP_MODULE_DEFINE 对称); PARAM_MODULE_DEFINE 供既有代码继续使用 */
#define PARAM_MODULE_DEFINE PARAM_APP_MODULE_DEFINE


/**
 * @brief 需要自定义注册尾巴的模块
 *
 * 手工写注册函数即可 (模式见 sensor_module.c 的 sensor_module_init):
 * @code
 * void xxx_module_init(void)
 * {
 *     param_module_register(&xxx_module, xxx_params, PARAM_COUNT(xxx_params));
 *     // ... 自定义初始化 (事件注册 / 额外配置) ...
 * }
 * PARAM_MODULE_REGISTER(xxx, xxx_module_init); // 自动注册开启时挂入链接器段
 * @endcode
 */

/**
 * @brief 编译器段自动注册
 *
 * 启用 PARAM_MODULE_AUTO_REGISTER_ENABLE 后，所有模块 (App 与 IP) 的
 * init 函数指针收集到 .rodata.param_modules 段,
 * param_modules_register_all() 遍历该段并逐个调用。
 * 机制定义见 param_manager.h (基类头, App/IP 共用)。
 */

#ifdef __cplusplus
}
#endif
#endif /* APP_PARAM_MANAGER_H */
