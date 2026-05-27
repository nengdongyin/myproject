#ifndef IP_PARAM_MANAGER_H
#define IP_PARAM_MANAGER_H

#include "param_manager.h"

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
 *   - 最多支持 64 个参数 (IP_DIRTY_MAP_BITS = 64)
 *   - dirty 追踪使用 64-bit 位图 (ip_instance_t::dirty_map)
 */

    typedef struct ip_instance ip_instance_t;

    /**
     * @brief IP Driver 读回调
     * @param driver   driver 私有数据指针
     * @param param_id 全局唯一参数 ID (MAKE_PARAM_ID 风格)
     * @param value    [out] 读出的值
     * @return PARAM_OK 成功
     */
    typedef int (*ip_read_fn)(void *driver, uint32_t param_id, param_value_t *value);

    /**
     * @brief IP Driver 写回调
     * @param driver   driver 私有数据指针
     * @param param_id 全局唯一参数 ID
     * @param value    待写入的值
     * @return PARAM_OK 成功
     */
    typedef int (*ip_write_fn)(void *driver, uint32_t param_id, param_value_t value);

    /**
 * @brief IP Driver 批量 flush 回调 (可选)
 *
 * 在逐参数 write 全部调用之后触发。
 * 驱动可在此执行批量操作 (如 I2C burst write)。
 * 置 NULL 时仅走 write 逐参数路径。
 *
 * @param driver    driver 私有数据指针
 * @param dirty_map 脏参数位图
 * @return PARAM_OK 成功
 */
    typedef int (*ip_flush_fn)(void *driver, uint64_t dirty_map);

    /**
     * @brief IP Driver 初始化回调
     * @param driver driver 私有数据指针
     * @return PARAM_OK 成功
     */
    typedef int (*ip_init_fn)(void *driver);

    /**
     * @brief IP Driver 命令执行回调
     * @param driver   driver 私有数据指针
     * @param param_id 命令 ID
     * @param arg      命令参数
     * @return PARAM_OK 成功
     */
    typedef int (*ip_exec_fn)(void *driver, uint32_t param_id, param_value_t arg);

    /**
     * @brief IP 实例 (嵌入 param_module_node 作为统一链表节点)
     *
     * 与 App 模块共用单条全局链表，通过 ip_module_vtable 区分行为。
     */
    struct ip_instance
    {
        param_module_node_t node; /**< 基类链表节点 */
        void *driver;             /**< driver 私有数据 */
        ip_read_fn read;       /**< 读硬件回调 */
        ip_write_fn write;     /**< 写硬件回调 */
        ip_flush_fn flush;     /**< 批量 flush 回调 (NULL 回退 write) */
        ip_init_fn init;       /**< 初始化回调 */
        ip_exec_fn exec;       /**< 命令执行回调 */
        uint64_t dirty_map;       /**< 64-bit 脏位图: bit[i]=1 表示第 i 个参数 dirty */
    };

    /**
     * @brief 注册 IP Driver 及其参数表
     *
     * 参数条目和 driver 实例均静态分配，零 malloc。
     * 注册过程: 哈希插入 + 统一链表插入 (按 MODULE_INIT_ORDER 排序)。
     *
     * @param inst    IP 实例指针 (静态分配)
     * @param entries 参数条目指针数组
     * @param count   参数数量 (<= 64)
     * @return PARAM_OK 成功，PARAM_ERR_ALREADY_REG 重复注册，
     *         PARAM_ERR_OUT_OF_RANGE count > 64，PARAM_ERR_NO_MEMORY 哈希表满
     */
    int ip_driver_register(ip_instance_t *inst,
                           param_entry_t **entries,
                           uint16_t count);

    /**
     * @brief IP 读写动作类型
     */
    typedef enum
    {
        IP_READ = 0,
        IP_WRITE = 1
    } ip_action_t;

    /**
     * @brief IP 控制接口 (调试用) — 直接向 driver 发起读/写
     *
     * 绕过参数缓存，直接与 driver 通信。仅支持 1/2/4 字节传输。
     *
     * @param ip_id    IP 模块 ID
     * @param local_id 局部参数 ID
     * @param data     数据缓冲区
     * @param len      数据长度 (1/2/4)
     * @param action   IP_READ 或 IP_WRITE
     * @return PARAM_OK 成功，或错误码
     */
    int ip_control(uint16_t ip_id, uint16_t local_id, uint8_t *data,
                   uint8_t len, ip_action_t action);

/**
 * @brief IP 读快捷宏
 * @sa ip_control(ip_id, local_id, data, len, IP_READ)
 */
#define ip_read(ip_id, local_id, data, len) \
    ip_control(ip_id, local_id, data, len, IP_READ)

/**
 * @brief IP 写快捷宏
 * @sa ip_control(ip_id, local_id, data, len, IP_WRITE)
 */
#define ip_write(ip_id, local_id, data, len) \
    ip_control(ip_id, local_id, data, len, IP_WRITE)

/**
 * @name IP 参数条目定义宏
 *
 * 与 App 宏使用完全相同的结构体类型，唯一区别是 vtable 指向 &ip_vtable。
 * 行为由 vtable 决定，不由结构体决定。
 * @{
 */

/** @brief 定义 IP UINT 类型参数 */
#define PARAM_IP_UINT(_name, _id, _flags, _def, _min, _max) \
    static param_range_entry_t _name = {                      \
        .base = {(_id), &ip_vtable},                          \
        .type = PARAM_TYPE_UINT,                              \
        .flags = (_flags),                                    \
        .dirty = 0,                                           \
        .has_range = ((_min) < (_max)),                       \
        .cache       = {.u32 = (_def)},                       \
        .default_val = {.u32 = (_def)},                       \
        .min         = {.u32 = (_min)},                       \
        .max         = {.u32 = (_max)},                       \
        PARAM_DEBUG_NAME_INIT(_name)}

/** @brief 定义 IP INT 类型参数 */
#define PARAM_IP_INT(_name, _id, _flags, _def, _min, _max) \
    static param_range_entry_t _name = {                     \
        .base = {(_id), &ip_vtable},                         \
        .type = PARAM_TYPE_INT,                              \
        .flags = (_flags),                                   \
        .dirty = 0,                                          \
        .has_range = ((_min) < (_max)),                      \
        .cache       = {.i32 = (_def)},                      \
        .default_val = {.i32 = (_def)},                      \
        .min         = {.i32 = (_min)},                      \
        .max         = {.i32 = (_max)},                      \
        PARAM_DEBUG_NAME_INIT(_name)}

/** @brief 定义 IP FLOAT 类型参数 */
#define PARAM_IP_FLOAT(_name, _id, _flags, _def, _min, _max) \
    static param_range_entry_t _name = {                       \
        .base = {(_id), &ip_vtable},                           \
        .type = PARAM_TYPE_FLOAT,                              \
        .flags = (_flags),                                     \
        .dirty = 0,                                            \
        .has_range = ((_min) < (_max)),                        \
        .cache       = {.f32 = (_def)},                        \
        .default_val = {.f32 = (_def)},                        \
        .min         = {.f32 = (_min)},                        \
        .max         = {.f32 = (_max)},                        \
        PARAM_DEBUG_NAME_INIT(_name)}

/** @brief 定义 IP BOOL 类型参数 */
#define PARAM_IP_BOOL(_name, _id, _flags, _def) \
    static param_bool_entry_t _name = {           \
        .base = {(_id), &ip_vtable},              \
        .type = PARAM_TYPE_BOOL,                  \
        .flags = (_flags),                        \
        .dirty = 0,                               \
        .cache       = {.b = (_def)},             \
        .default_val = {.b = (_def)},             \
        PARAM_DEBUG_NAME_INIT(_name)}

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
    static param_enum_entry_t _name = {                        \
        .base = {(_id), &ip_vtable},                           \
        .type = PARAM_TYPE_ENUM,                               \
        .flags = (_flags),                                     \
        .dirty = 0,                                            \
        .cache       = {.i32 = (_def)},                        \
        .default_val = {.i32 = (_def)},                        \
        .enum_values = (_vals), .enum_count = (_cnt),          \
        PARAM_DEBUG_NAME_INIT(_name)}

/** @brief 定义 IP BLOB 类型参数
 *
 * BLOB 数据存放在 cache.ptr 指向的外部静态缓冲区。框架不管理内存。
 * 调用者必须确保缓冲区的生命周期覆盖参数使用期间。
 *
 * @param _name    变量名
 * @param _id      参数 ID
 * @param _flags   属性标志
 * @param _def_ptr 默认值缓冲区 (静态分配)
 * @param _size    blob 大小
 */
#define PARAM_IP_BLOB(_name, _id, _flags, _def_ptr, _size)            \
    static param_blob_entry_t _name = {                                 \
        .base        = { (_id), &ip_vtable },                           \
        .type        = PARAM_TYPE_BLOB,                                 \
        .flags       = (_flags),                                        \
        .dirty       = 0,                                               \
        .cache       = { .ptr = (_def_ptr) },                           \
        .default_val = { .ptr = (_def_ptr) },                           \
        .blob_size   = (_size),                                         \
        PARAM_DEBUG_NAME_INIT(_name)                                    \
    }

/**
 * @brief 定义 IP STRING 类型参数
 *
 * 缓冲区大小应为 (max_len + 1)，以容纳结尾 '\\0'。
 * 写入时自动 strncpy 并截断超长字符串。
 *
 * @param _name    变量名
 * @param _id      参数 ID
 * @param _flags   属性标志
 * @param _def_ptr 默认值缓冲区 (静态分配, 长 (max_len + 1))
 * @param _len     最大字符数 (不含结尾 '\\0')
 */
#define PARAM_IP_STRING(_name, _id, _flags, _def_ptr, _len)           \
    static param_string_entry_t _name = {                               \
        .base        = { (_id), &ip_vtable },                           \
        .type        = PARAM_TYPE_STRING,                               \
        .flags       = (_flags),                                        \
        .dirty       = 0,                                               \
        .cache       = { .ptr = (_def_ptr) },                           \
        .default_val = { .ptr = (_def_ptr) },                           \
        .max_len     = (_len),                                          \
        PARAM_DEBUG_NAME_INIT(_name)                                    \
    }

/**
 * @brief 定义 IP EXEC 命令
 *
 * 注册一个 PARAM_FLAG_EXEC 标记的伪参数条目。不可通过 param_write 写入，
 * 仅由 param_exec 触发。框架保证 param_write_raw / param_write_immediate
 * 遇到 EXEC 参数时自动路由到模块的 exec 回调，arg 为 param_value_t 联合体。
 * dump 输出为 "EXEC" (不显示值)，flags 列显示 "E"。
 *
 * @param _name 命令变量名
 * @param _id   命令 ID (MAKE_PARAM_ID 风格)
 */
#define PARAM_IP_EXEC(_name, _id)       \
    static param_exec_entry_t _name = {  \
        .base = {(_id), &ip_vtable},     \
        .type = PARAM_TYPE_EXEC,         \
        .flags = PARAM_FLAG_EXEC,        \
        .dirty = 0,                      \
        .cache       = {.u32 = 0},       \
        .default_val = {.u32 = 0},       \
        PARAM_DEBUG_NAME_INIT(_name)}

/** @} */

/**
 * @brief 定义 IP Driver 实例 (静态分配)
 *
 * 参数顺序按生命周期排列: driver → init → read → write → exec → flush
 *
 * 使用示例:
 * @code
 * IP_DRIVER_DEFINE(sensor, IP_SENSOR, "OV4689_Sensor_IP",
 *                  &g_sensor_dev, sensor_init_cb, sensor_read,
 *                  sensor_write, sensor_exec, NULL);
 * @endcode
 *
 * @param _ip_name IP 实例名 (生成 _ip_name##_instance)
 * @param _ip_id   模块 ID
 * @param _label   调试名称
 * @param _drv     driver 私有数据指针
 * @param _init    初始化回调 (可 NULL)
 * @param _rd      读回调
 * @param _wr      写回调 (可 NULL)
 * @param _exec    exec 命令回调 (可 NULL)
 * @param _fl      flush 回调 (可 NULL, 回退逐参数 write)
 */
#define IP_DRIVER_DEFINE(_ip_name, _ip_id, _label, _drv, _init, _rd, _wr, _exec, _fl) \
    static ip_instance_t _ip_name##_instance = {                        \
        .node = {                                                       \
            .module_id = (_ip_id),                                      \
            .name = (_label),                                           \
            .vtable = &ip_module_vtable,                                \
        },                                                              \
        .driver = (_drv),                                               \
        .read = (_rd),                                               \
        .write = (_wr),                                              \
        .exec = (_exec),                                             \
        .flush = (_fl),                                              \
        .init = (_init),                                             \
    }

/**
 * @brief 定义 IP Driver 注册函数
 *
 * @param _ip_name IP 实例名 (与 IP_DRIVER_DEFINE 中相同)
 * @param _entries 参数表数组
 */
#define IP_DRIVER_INIT(_ip_name, _entries)                            \
    void _ip_name##_init(void)                                        \
    {                                                                 \
        ip_driver_register(&_ip_name##_instance,                      \
                           (_entries),                                \
                           sizeof(_entries) / sizeof((_entries)[0])); \
    }

#ifdef __cplusplus
}
#endif
#endif /* IP_PARAM_MANAGER_H */
