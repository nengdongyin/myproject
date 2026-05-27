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

    /**
     * @brief 编译器段自动注册
     *
     * 启用 PARAM_MODULE_AUTO_REGISTER 后，所有模块的 init 函数指针收集到
     * .rodata.param_modules 段。param_modules_register_all() 遍历该段并逐个调用。
     */

    /** 编译器段自动注册的条目 */
    typedef struct
    {
        void (*init)(void);
    } param_module_reg_t;

/**
 * @brief 将一个模块的 init 函数放入指定链接器段 (.rodata.param_modules)
 *
 * 启用 PARAM_MODULE_AUTO_REGISTER 后，param_modules_register_all() 遍历该段并逐个调用 init。
 * @param _name 模块名 (用于段符号名唯一性)
 * @param _init 模块 register 函数
 */
#if PARAM_MODULE_AUTO_REGISTER

/** 将模块 init 函数放入 .rodata.param_modules 段 */
#define PARAM_MODULE_REGISTER(_name, _init)                                                                          \
    __attribute__((used, section(".rodata.param_modules"))) static const param_module_reg_t _pm_auto_reg_##_name = { \
        .init = (_init)}
    int param_modules_register_all(void);

#else

#define PARAM_MODULE_REGISTER(_name, _init)
    /* auto-register disabled, call xxx_module_init() manually */

#endif

/**
 * @brief 定义一个 App 模块实例 (静态分配)
 *
 * 使用示例:
 * @code
 * PARAM_MODULE_DEFINE(auto_exp, MODULE_AUTO_EXP, "AutoExposure",
 *                     &g_ae_instance, ae_init, NULL, ae_write, NULL, ae_flush);
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
 * @param _ctx        模块私有上下文
 * @param _init_fn    初始化回调 (可 NULL)
 * @param _read_fn    read 回调 (可 NULL = 直接返缓存)
 * @param _write_fn   write 回调 (校验/转换)
 * @param _exec_fn    exec 命令回调 (可 NULL)
 * @param _flush_fn   flush 回调函数
 */
#define PARAM_MODULE_DEFINE(_mod_name, _mod_id, _label, _ctx, _init_fn, _read_fn, _write_fn, _exec_fn, _flush_fn) \
    static param_module_t _mod_name##_module = {                              \
        .node = {                                                             \
            .module_id = (_mod_id),                                           \
            .name = (_label),                                                 \
            .vtable = &app_module_vtable,                                     \
        },                                                                    \
        .init  = (_init_fn),                                                  \
        .read  = (_read_fn),                                                  \
        .write = (_write_fn),                                                 \
        .exec  = (_exec_fn),                                                  \
        .flush = (_flush_fn),                                                 \
        .ctx   = (_ctx),                                                      \
    }

/**
 * @brief 定义模块注册函数 (一键式)
 *
 * 同时生成 _mod_name##_module_init() 函数和自动注册条目。
 * 调用此函数即完成 param_module_register。
 *
 * @param _mod_name 模块名
 * @param _params   参数表数组 (PARAM_TABLE 定义)
 */
#define PARAM_MODULE_INIT(_mod_name, _params)                          \
    void _mod_name##_module_init(void)                                 \
    {                                                                  \
        param_module_register(&_mod_name##_module,                     \
                              (_params),                               \
                              sizeof(_params) / sizeof((_params)[0])); \
    }                                                                  \
    PARAM_MODULE_REGISTER(_mod_name, _mod_name##_module_init)

/**
 * @brief 模块注册函数前缀 (手动指定 init 回调时使用)
 *
 * 配合 PARAM_MODULE_INIT_END 使用，允许在 register 和自动注册之间插入自定义 init 设置。
 *
 * @param _mod_name 模块名
 * @param _params   参数表数组
 */
#define PARAM_MODULE_INIT_BEGIN(_mod_name, _params) \
    void _mod_name##_module_init(void)              \
    {                                               \
        param_module_register(&_mod_name##_module,  \
                              (_params),            \
                              sizeof(_params) / sizeof((_params)[0]))

/**
 * @brief 模块注册函数后缀
 *
 * @param _mod_name 模块名
 */
#define PARAM_MODULE_INIT_END(_mod_name) \
    }                                    \
    PARAM_MODULE_REGISTER(_mod_name, _mod_name##_module_init)

/**
 * @name App 参数条目定义宏
 *
 * 每个宏定义一个 static 的 xxx_entry_t 变量，编译期 .data/.bss 中初始化为零，
 * 其余字段 (cache / default_val / range 等) 由宏参数填入。
 *
 * @note 所有宏定义均声明为 static 变量，不占用可写内存。
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
#define PARAM_UINT(_name, _id, _flags, _def, _min, _max) \
    static param_range_entry_t _name = {                 \
        .base = {(_id), &app_vtable},                    \
        .type = PARAM_TYPE_UINT,                         \
        .flags = (_flags),                               \
        .dirty = 0,                                      \
        .has_range = ((_min) < (_max)),                  \
        .cache = {.u32 = (_def)},                        \
        .default_val = {.u32 = (_def)},                  \
        .min = {.u32 = (_min)},                          \
        .max = {.u32 = (_max)},                          \
        PARAM_DEBUG_NAME_INIT(_name)}

/** @brief 定义 INT 类型参数 */
#define PARAM_INT(_name, _id, _flags, _def, _min, _max) \
    static param_range_entry_t _name = {                \
        .base = {(_id), &app_vtable},                   \
        .type = PARAM_TYPE_INT,                         \
        .flags = (_flags),                              \
        .dirty = 0,                                     \
        .has_range = ((_min) < (_max)),                 \
        .cache = {.i32 = (_def)},                       \
        .default_val = {.i32 = (_def)},                 \
        .min = {.i32 = (_min)},                         \
        .max = {.i32 = (_max)},                         \
        PARAM_DEBUG_NAME_INIT(_name)}

/** @brief 定义 FLOAT 类型参数 */
#define PARAM_FLOAT(_name, _id, _flags, _def, _min, _max) \
    static param_range_entry_t _name = {                  \
        .base = {(_id), &app_vtable},                     \
        .type = PARAM_TYPE_FLOAT,                         \
        .flags = (_flags),                                \
        .dirty = 0,                                       \
        .has_range = ((_min) < (_max)),                   \
        .cache = {.f32 = (_def)},                         \
        .default_val = {.f32 = (_def)},                   \
        .min = {.f32 = (_min)},                           \
        .max = {.f32 = (_max)},                           \
        PARAM_DEBUG_NAME_INIT(_name)}

/** @brief 定义 ENUM 类型参数 */
#define PARAM_ENUM(_name, _id, _flags, _def, _vals, _cnt) \
    static param_enum_entry_t _name = {                   \
        .base = {(_id), &app_vtable},                     \
        .type = PARAM_TYPE_ENUM,                          \
        .flags = (_flags),                                \
        .dirty = 0,                                       \
        .cache = {.i32 = (_def)},                         \
        .default_val = {.i32 = (_def)},                   \
        .enum_values = (_vals),                           \
        .enum_count = (_cnt),                             \
        PARAM_DEBUG_NAME_INIT(_name)}

/** @brief 定义 BOOL 类型参数 */
#define PARAM_BOOL(_name, _id, _flags, _def) \
    static param_bool_entry_t _name = {      \
        .base = {(_id), &app_vtable},        \
        .type = PARAM_TYPE_BOOL,             \
        .flags = (_flags),                   \
        .dirty = 0,                          \
        .cache = {.b = (_def)},              \
        .default_val = {.b = (_def)},        \
        PARAM_DEBUG_NAME_INIT(_name)}

/**
 * @brief 定义 BLOB 类型参数
 *
 * @param _name    变量名
 * @param _id      参数 ID
 * @param _flags   属性标志
 * @param _def_ptr 默认值缓冲区 (静态分配)
 * @param _size    blob 大小
 */
#define PARAM_BLOB(_name, _id, _flags, _def_ptr, _size) \
    static param_blob_entry_t _name = {                 \
        .base = {(_id), &app_vtable},                   \
        .type = PARAM_TYPE_BLOB,                        \
        .flags = (_flags),                              \
        .dirty = 0,                                     \
        .cache = {.ptr = (_def_ptr)},                   \
        .default_val = {.ptr = (_def_ptr)},             \
        .blob_size = (_size),                           \
        PARAM_DEBUG_NAME_INIT(_name)}

/**
 * @brief 定义 STRING 类型参数
 *
 * 缓冲区大小应为 (max_len + 1)，以容纳结尾 '\\0'。
 * 写入时自动 strncpy 并截断超长字符串。
 *
 * @param _name    变量名
 * @param _id      参数 ID
 * @param _flags   属性标志
 * @param _def     默认字符串 (C 字面量)
 * @param _max_len 最大字符数 (不含 '\\0')
 */
#define PARAM_STRING(_name, _id, _flags, _def, _max_len) \
    static param_string_entry_t _name = {                \
        .base = {(_id), &app_vtable},                    \
        .type = PARAM_TYPE_STRING,                       \
        .flags = (_flags),                               \
        .dirty = 0,                                      \
        .cache = {.ptr = (char *)(_def)},                \
        .default_val = {.ptr = (char *)(_def)},          \
        .max_len = (_max_len),                           \
        PARAM_DEBUG_NAME_INIT(_name)}

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
#define PARAM_EXEC(_name, _id)          \
    static param_exec_entry_t _name = { \
        .base = {(_id), &app_vtable},   \
        .type = PARAM_TYPE_EXEC,        \
        .flags = PARAM_FLAG_EXEC,       \
        .dirty = 0,                     \
        .cache = {.u32 = 0},            \
        .default_val = {.u32 = 0},      \
        PARAM_DEBUG_NAME_INIT(_name)}

    /** @} */

#ifdef __cplusplus
}
#endif
#endif /* APP_PARAM_MANAGER_H */
