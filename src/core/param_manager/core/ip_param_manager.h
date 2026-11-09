#ifndef IP_PARAM_MANAGER_H
#define IP_PARAM_MANAGER_H

#include "param_manager.h"
#include "param_manager_internal.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @file ip_param_manager.h
     * @brief IP (FPGA IP 可配置属性) 参数子系统 — 基于 driver 回调的参数管理
     *
     * IP 参数不直接操作硬件地址。硬件细节 (基址、偏移、位宽、FIFO 协议) 全部由
     * driver 封装。框架通过 read / write / init 回调与 driver 通信。
     *
     * 支持类型: UINT / INT / FLOAT / BOOL / BLOB / STRING / EXEC
     *
     * 参数条目结构体与 App 完全一致 (param_range_entry_t / param_bool_entry_t /
     * param_blob_entry_t / param_string_entry_t / param_exec_entry_t)，
     * 唯一区别是 vtable 指向 &ip_vtable。行为由 vtable 决定，不由结构体决定。
     *
     * 关键约束:
     *   - 参数条目前 sizeof(param_entry_head_t) 字节与 param_entry_head_t 布局一致，可使用统一访问器
     *   - 无独立参数数上限: 受 local_id 16-bit 空间与全局哈希表 PARAM_HASH_SIZE 约束
     *   - dirty 追踪使用条目级 dirty 标志 (param_entry_head_t::dirty)，与 App 模块同构
     *
     * IP Driver 回调契约 (回调类型 param_read_fn / param_write_fn /
     * param_flush_fn / param_init_fn / param_exec_fn 定义于 param_manager.h,
     * 与 App 共用; 以下契约语义 IP 独有):
     *   - read:  仅当确实从硬件寄存器取得有效值时才允许返回 PARAM_OK
     *            (框架无条件回写缓存, 返回 OK 但 value 无效将污染缓存);
     *            未取得硬件值必须返回错误, 框架回退缓存值。
     *   - write: 执行期间缓存已先行更新为新值 (immediate 与 flush 路径一致),
     *            回调内 param_read / param_save_one 反映新值; 硬件写失败
     *            缓存保持新值不回滚。
     *            可 NULL: 缺失时参数值只停留在缓存层, 由 flush 的批量回调
     *            或 exec 命令下发, 框架不视为错误 (与 param_write 一致)。
     *            回调在所有路径均锁外执行, 可安全调用加锁 API;
     *            但仍不得在回调内调用 param_flush(), 也不建议回调内做
     *            param_save_one 持久化 (每次下发写一次 Flash, 寿命消耗大)。
     *   - flush: 在逐参数 write 全部调用之后触发, 可执行批量操作 (如
     *            I2C burst write); 无脏位图参数, 需要批量信息时自行
     *            查询参数缓存。
     */

    /** IP 实例类型 — App 与 IP 共用的模块实例结构体别名 (定义见 param_manager.h) */
    typedef struct param_module ip_instance_t;

    /* ================================================================
     *  回调类型 (与 app_param_manager.h 对应位置对称)
     *
     *  IP 侧无独立回调类型定义: 与 App 共用 param_read_fn /
     *  param_write_fn / param_flush_fn / param_init_fn / param_exec_fn
     *  (定义于 param_manager.h)。IP 独有的回调契约见本文件头部
     *  "IP Driver 回调契约" 小节。
     * ================================================================ */

    /* ================================================================
     *  实例/模块结构体 (与 app_param_manager.h 对应位置对称)
     *
     *  IP 实例无独立结构体定义: ip_instance_t 为 struct param_module
     *  的别名 (定义于 param_manager.h)，两子系统共用同一份布局:
     *   - node    基类链表节点
     *   - read / write / flush / init / exec   回调组
     *   - ctx     私有上下文 (IP 侧语义为 driver 私有数据指针)
     * ================================================================ */

    /* ================================================================
     *  注册 (与 App 共用同一入口)
     *
     *  IP 无独立注册函数: App 与 IP 共用 param_module_register
     *  (定义于 param_manager_internal.h / param_manager.c)。vtable 已由
     *  IP_DRIVER_DEFINE 写入 node, 注册时无需也不能再指定。
     * ================================================================ */

    /** @brief IP 参数级 vtable 实现 (定义于 ip_param_manager.c) */
    extern const param_vtable_t ip_vtable;
    /** @brief IP 模块级 vtable 实现 (定义于 ip_param_manager.c) */
    extern const param_module_vtable_t ip_module_vtable;

/**
 * @name IP 参数条目定义宏
 *
 * 条目体由 param_manager.h 中的 PARAM_ENTRY_* 核心宏生成 (与 App 共享),
 * 唯一区别是 vtable 指向 &ip_vtable。行为由 vtable 决定，不由结构体决定。
 * @{
 */

/** @brief 定义 IP UINT 类型参数 */
#define PARAM_IP_UINT(_name, _id, _flags, _def, _min, _max) \
    PARAM_ENTRY_RANGE_UINT(&ip_vtable, _name, _id, _flags, _def, _min, _max)

/** @brief 定义 IP INT 类型参数 */
#define PARAM_IP_INT(_name, _id, _flags, _def, _min, _max) \
    PARAM_ENTRY_RANGE_INT(&ip_vtable, _name, _id, _flags, _def, _min, _max)

/** @brief 定义 IP FLOAT 类型参数 */
#define PARAM_IP_FLOAT(_name, _id, _flags, _def, _min, _max) \
    PARAM_ENTRY_RANGE_FLOAT(&ip_vtable, _name, _id, _flags, _def, _min, _max)

/** @brief 定义 IP BOOL 类型参数 */
#define PARAM_IP_BOOL(_name, _id, _flags, _def) \
    PARAM_ENTRY_BOOL(&ip_vtable, _name, _id, _flags, _def)

/**
 * @brief 定义 IP ENUM 类型参数
 *
 * @param _name  变量名
 * @param _id    参数 ID
 * @param _flags 属性标志
 * @param _def   默认值
 * @param _vals  允许的值列表 (static const int32_t[])
 * @param _cnt   列表中值的数量
 */
#define PARAM_IP_ENUM(_name, _id, _flags, _def, _vals, _cnt) \
    PARAM_ENTRY_ENUM(&ip_vtable, _name, _id, _flags, _def, _vals, _cnt)

/** @brief 定义 IP BLOB 类型参数
 *
 * BLOB 数据存放在 cache.ptr 指向的外部静态缓冲区。框架不管理内存。
 * 调用者必须确保缓冲区的生命周期覆盖参数使用期间。
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
#define PARAM_IP_BLOB(_name, _id, _flags, _def_ptr, _size) \
    PARAM_ENTRY_BLOB(&ip_vtable, _name, _id, _flags, _def_ptr, _size)

/**
 * @brief 定义 IP STRING 类型参数 — 缓存指向调用方静态缓冲区
 *
 * @attention _def_ptr 必须为可写静态字符数组的地址 char[_max_len+1]
 *   (由调用方初始化默认内容)。严禁传入字符串字面量: 字面量位于
 *   .rodata, 首次写入将触发写保护故障。
 *   缓存与默认值共享该缓冲 (单缓冲配置), 写入后 reset 无法恢复
 *   初始默认内容 (自拷贝被跳过, 仅清 dirty)。
 *
 * @param _name    变量名
 * @param _id      参数 ID
 * @param _flags   属性标志
 * @param _def_ptr 可写静态字符缓冲区地址 (长 _max_len+1, 调用方初始化)
 * @param _len     最大字符数 (不含结尾 '\\0')
 */
#define PARAM_IP_STRING(_name, _id, _flags, _def_ptr, _len) \
    PARAM_ENTRY_STRING(&ip_vtable, _name, _id, _flags, (_def_ptr), _len)

/**
 * @brief 定义 IP EXEC 命令
 *
 * 注册一个 PARAM_FLAG_EXEC 标记的伪参数条目。不可通过 param_write 写入，
 * 仅由 param_exec 触发。框架保证 param_write_raw / param_write_sync
 * 遇到 EXEC 参数时自动路由到模块的 exec 回调，arg 为 param_value_t 联合体。
 * dump 输出为 "EXEC" (不显示值)，flags 列显示 "E"。
 *
 * @param _name 命令变量名
 * @param _id   命令 ID (MAKE_PARAM_ID 风格)
 */
#define PARAM_IP_EXEC(_name, _id)   \
    PARAM_ENTRY_EXEC(&ip_vtable, _name, _id)

/** @} */

/**
 * @brief 定义 IP Driver 实例 (静态分配)
 *
 * 参数顺序按生命周期排列: driver → init → read → write → exec → flush
 *
 * 使用示例:
 * @code
 * IP_DRIVER_DEFINE(sensor, IP_SENSOR, "OV4689_Sensor_IP",
 *                  .ctx  = &g_sensor_dev,
 *                  .init = sensor_init_cb,
 *                  .read = sensor_read,
 *                  .write = sensor_write,
 *                  .exec = sensor_exec);
 * @endcode
 *
 * @param _ip_name IP 实例名 (生成 _ip_name##_module, 与 App 后缀一致)
 * @param _ip_id   模块 ID
 * @param _label   调试名称
 * @param ...      具名字段初始化器 (.ctx / .init / .read / .write /
 *                 .exec / .flush; 省略字段为 NULL)
 */
#define IP_DRIVER_DEFINE(_ip_name, _ip_id, _label, ...)               \
    PARAM_MODULE_DEFINE_CORE(&ip_module_vtable, _ip_name##_module,    \
                             _ip_id, _label, __VA_ARGS__)

/**
 * @brief 定义 IP Driver 注册函数 (注册入口与 App 共用 param_module_register)
 *
 * 开启 PARAM_MODULE_AUTO_REGISTER_ENABLE 时同时生成 linker-section
 * 自动注册条目 (与 PARAM_MODULE_INIT 同机制); 未开启时由启动代码手动调用。
 *
 * @param _ip_name IP 实例名 (与 IP_DRIVER_DEFINE 中相同)
 * @param _entries 参数表数组
 */
#define IP_DRIVER_INIT(_ip_name, _entries)                            \
    PARAM_MODULE_INIT_CORE(param_module_register,                     \
                           _ip_name##_init,                           \
                           _ip_name##_module,                         \
                           _entries)                                  \
    PARAM_MODULE_REGISTER(_ip_name, _ip_name##_init)

    /* ================================================================
     *  编译器段自动注册
     *
     *  IP 侧与 App 侧共用 param_module_reg_t / PARAM_MODULE_REGISTER /
     *  param_modules_register_all (位于 app_param_manager.h);
     *  开启 PARAM_MODULE_AUTO_REGISTER_ENABLE 后 IP_DRIVER_INIT
     *  同样进入 linker-section 自动注册。
     * ================================================================ */

#ifdef __cplusplus
}
#endif
#endif /* IP_PARAM_MANAGER_H */
