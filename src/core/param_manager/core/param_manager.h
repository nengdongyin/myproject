#ifndef PARAM_MANAGER_H
#define PARAM_MANAGER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "param_manager_config.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @file param_manager.h
     * @brief 嵌入式内存参数管理器 — 公共接口定义
     *
     * @par 架构总览
     * 两级 vtable 多态 (OOP-in-C):
     *   - 参数级 param_vtable_t: 每条参数条目携带 vtable 指针, 决定
     *     读/写/持久化/重置等行为 (App 参数与 IP 参数两套实现);
     *   - 模块级 param_module_vtable_t: 每个模块携带 vtable 指针,
     *     决定标脏/flush/init/exec 等行为。
     *
     * @par 分层规则 (单一事实来源)
     *   - 本文件: 共享类型、两级 vtable 结构、公共 API、*_ENTRY / *_CORE
     *     核心宏 (以参数注入 App/IP 差异, 应用层勿直接使用);
     *   - param_manager.c: 两级 vtable 的通用实现 (基类);
     *   - app_param_manager.* / ip_param_manager.*: 各自子系统专属的
     *     类型别名、包装宏、vtable 常量与差异实现 (派生)。
     *   依赖方向为 派生 → 基类, 无反向。
     *
     * @par 关键设计点
     *   - 参数条目静态定义, 零动态分配;
     *   - 类型分派表 (g_param_data_ops / g_param_pre_write) 消除 switch-case;
     *   - App 与 IP 模块共用统一结构体 param_module 与单条注册链表;
     *   - 模块级 flush 顺序由 MODULE_INIT_ORDER (module_ids.h) 决定;
     *   - 持久化经 param_storage_drv_t 后端抽象 (FlashDB / mkv / ini 等),
     *     支持多分区与版本迁移。
     */

/** @brief 构造 32 位参数 ID (高 16 位 module_id, 低 16 位 local_id) */
#define MAKE_PARAM_ID(mod, loc) (uint32_t)((((mod) & 0xFFFFu) << 16) | ((loc) & 0xFFFFu))
/** @brief 从 32 位 ID 提取模块 ID (高 16 位) */
#define PARAM_MODULE_ID(id) (((id) >> 16) & 0xFFFFu)
/** @brief 从 32 位 ID 提取本地 ID (低 16 位) */
#define PARAM_LOCAL_ID(id) ((id) & 0xFFFFu)

    /**
     * @brief 参数值联合体
     *
     * 覆盖所有支持的参数类型。实际使用的成员由 param_type_t 决定。
     *
     * .ptr 成员承担多种语义 (按使用场景区分):
     *   - BLOB  cache / default_val: 指向外部静态字节缓冲区
     *   - STRING cache / default_val: 指向外部静态 char 缓冲区
     *   - 写入传递: param_write_string / param_write_raw 通过 .ptr
     *     向 cache_update_string / cache_update_blob 传递源数据指针
     *   - EXEC 参数: param_exec / write_raw 通过 .ptr 传递 user_arg
     *
     * 值类型 (UINT/INT/FLOAT/BOOL/ENUM) 不使用 .ptr。
     */
    typedef union
    {
        uint32_t u32;
        int32_t i32;
        float f32;
        bool b;
        void *ptr;
    } param_value_t;

    /**
     * @brief 参数类型枚举
     */
    typedef enum
    {
        PARAM_TYPE_UINT = 0,
        PARAM_TYPE_INT = 1,
        PARAM_TYPE_FLOAT = 2,
        PARAM_TYPE_BOOL = 3,
        PARAM_TYPE_ENUM = 4,
        PARAM_TYPE_BLOB = 5,
        PARAM_TYPE_STRING = 6,
        PARAM_TYPE_EXEC = 7,
        PARAM_TYPE_COUNT
    } param_type_t;

    /**
     * @brief 参数属性标志位
     *
     * 可通过位或组合使用。
     */
    typedef enum
    {
        PARAM_FLAG_PERSIST = (1u << 0),    /**< 需要持久化到 Flash */
        PARAM_FLAG_READONLY = (1u << 1),   /**< 只读，禁止写入 */
        PARAM_FLAG_HIDDEN = (1u << 2),     /**< 对用户隐藏 */
        PARAM_FLAG_DEPRECATED = (1u << 3), /**< 已废弃，保留兼容 */
        PARAM_FLAG_EXEC = (1u << 4),       /**< exec 命令 (不可写入, 仅 param_exec 触发) */
    } param_flag_t;

    /**
     * @brief 函数返回状态码
     *
     * 所有公开 API 均返回 param_status_t 或其等价 int。
     */
    typedef enum
    {
        PARAM_OK = 0,                 /**< 操作成功 */
        PARAM_ERR_INVALID_ID = -1,    /**< 无效的参数 ID */
        PARAM_ERR_OUT_OF_RANGE = -2,  /**< 值超出合法范围 */
        PARAM_ERR_READONLY = -3,      /**< 尝试写入只读参数 */
        PARAM_ERR_TYPE_MISMATCH = -4, /**< 参数类型不匹配 */
        PARAM_ERR_NOT_FOUND = -5,     /**< 模块/参数未找到 */
        PARAM_ERR_FLASH_FAIL = -6,    /**< Flash 存储操作失败 */
        PARAM_ERR_ALREADY_REG = -7,   /**< 模块或参数已注册 */
        PARAM_ERR_NO_MEMORY = -8,     /**< 哈希表已满 */
        PARAM_ERR_TIMEOUT = -9,       /**< 操作超时 */
        PARAM_ERR_BUSY = -10,         /**< 系统忙，重复初始化 */
        /** 当前配置/后端不具备该能力: 未编译持久化 (storage == NULL)、
         *  或后端未提供所需回调 (load/save/delete/erase_all/分区)。
         *  与 PARAM_ERR_NOT_FOUND("查不到实体") 区分: 本码表示"本固件不支持",
         *  重试无意义; 通常由构建配置决定, 运行期不会自行恢复。 */
        PARAM_ERR_NOT_SUPPORTED = -11,
    } param_status_t;

    typedef struct param_module param_module_t;
    typedef struct param_entry param_entry_t;

    typedef struct param_module_node param_module_node_t;

    /**
     * @brief 参数变化通知回调
     *
     * 当参数值通过 param_write / param_write_cache / param_write_immediate /
     * param_write_raw / param_write_blob / param_write_string / param_update
     * 成功写入后调用。回调在锁外执行;
     * 回调内再次调用写入类 API 需由实现者自行处理防重入
     * (框架不提供递归保护)。
     *
     * @attention new_value 取自写入后的参数缓存 (**生效值**), 不是调用者
     *   传入的原始值: pre_write 的超范围裁剪与 apply 回调的修正都已体现
     *   在其中。BLOB/STRING 的 .ptr 指向框架缓存缓冲区 (生命周期覆盖整个
     *   运行期), 而非调用者的临时缓冲区。
     *   EXEC 参数经 param_write_raw 触发时缓存不参与, new_value 携带命令
     *   负载 (与 exec 回调收到的 arg 一致)。
     *
     * @attention 多线程并发写入同一参数时，notify 可能收到中间值
     *   (另一个线程在 notify 发出后立即覆盖了参数值)。
     *   new_value 应视为 best-effort 快照，不应假设其与缓存当前值一致。
     *
     * @param param_id  发生变化的参数 ID
     * @param new_value 写入后的生效值 (best-effort 快照)
     */
    typedef void (*param_notify_fn)(uint32_t param_id, param_value_t new_value);

    /**
     * @brief 模块参数读取回调 (App 与 IP 共用)
     *
     * 若提供且返回 PARAM_OK，框架回写缓存后返回。
     * NULL 或返回错误 → 回退到读取缓存值。
     *
     * @attention 仅当确实从权威源 (硬件寄存器/实时数据) 取得有效值
     *   时才允许返回 PARAM_OK: 框架对返回值无条件回写缓存,
     *   返回 PARAM_OK 但 value 未填有效值将污染缓存并可能被持久化。
     *   未取得权威值时必须返回错误, 框架会回退到缓存值。
     *
     * @param ctx      模块私有上下文
     * @param param_id 参数 ID
     * @param value    [out] 读到的值
     * @return PARAM_OK 成功 (触发缓存回写)，其他值回退缓存
     */
    typedef int (*param_read_fn)(void *ctx, uint32_t param_id, param_value_t *value);

    /**
     * @brief 模块参数写入回调 (App 与 IP 共用)
     *
     * 回调语义随模块类型分派:
     *   - App 模块: apply 校验/转换, 返回非 OK 拒绝写入;
     *   - IP 模块:  向硬件下发新值。
     *
     * @attention 回调执行期间缓存已先行更新为新值 (含 BLOB/STRING):
     *   回调内 param_read / param_save_one 均反映新值。
     *   回调返回非 OK 时缓存保持新值不回滚。
     *
     * @param param_id 参数 ID
     * @param value    待写入的新值
     * @return PARAM_OK 表示接受，其他值表示拒绝
     */
    typedef int (*param_write_fn)(void *ctx, uint32_t param_id, param_value_t value);

    /**
     * @brief App 模块 flush 回调 — 将缓存的参数批量写入硬件
     *
     * @note 回调在锁外执行, 回调内可安全调用 param_* API;
     *   回调内对参数的重新写入会保留 dirty, 由下一轮 param_flush 处理。
     * @param ctx 模块注册时提供的上下文指针
     * @return PARAM_OK 成功
     */
    typedef int (*param_flush_fn)(void *ctx);

    /**
     * @brief App 模块初始化回调
     * @param ctx 模块注册时提供的上下文指针
     * @return PARAM_OK 成功
     */
    typedef int (*param_init_fn)(void *ctx);

    /**
     * @brief 模块命令执行回调
     *
     * 当 param_write / param_write_raw / param_write_immediate 遇到 PARAM_FLAG_EXEC
     * 参数时，框架自动路由到此回调。arg 为 param_value_t 联合体，可通过 .u32 / .i32 /
     * .f32 / .b / .ptr 按需取值。
     *
     * @param ctx      模块私有上下文 (App: 业务实例 / IP: driver 数据)
     * @param param_id 全局唯一参数 ID (MAKE_PARAM_ID 风格)
     * @param arg      命令参数 (param_value_t 联合体)
     * @return PARAM_OK 成功，其他值表示失败
     */
    typedef int (*param_exec_fn)(void *ctx, uint32_t param_id, param_value_t arg);

    /**
     * @brief 模块级虚函数表 (vtable)
     *
     * 每个模块类型 (App / IP) 提供一套实现，存于 .rodata。
     * 通过模块级 vtable 统一处理 dirty 标记、flush、init 等操作。
     */
    typedef struct param_module_vtable
    {
        /** 将模块及其指定参数标记为 dirty */
        void (*mark_dirty)(param_module_node_t *m, uint16_t local_id);
        /** 清除模块内指定参数的 dirty 标记 */
        void (*clear_dirty)(param_module_node_t *m, uint16_t local_id);
        /** 将模块的所有 dirty 参数刷入硬件 */
        int (*flush)(param_module_node_t *m);
        /** 模块初始化 */
        int (*init)(param_module_node_t *m);
        /** 模块命令执行 */
        int (*exec)(param_module_node_t *m, uint32_t cmd_id, param_value_t arg);
        /** 模块重置 */
        void (*reset)(param_module_node_t *m);
        /** 模块去初始化 */
        void (*deinit)(param_module_node_t *m);
    } param_module_vtable_t;

    /* 两级 vtable 的具体实例 (app_vtable / ip_vtable / app_module_vtable /
       ip_module_vtable) 由派生侧头文件声明:
         - app_param_manager.h → app_vtable / app_module_vtable
         - ip_param_manager.h  → ip_vtable  / ip_module_vtable
       基类头文件只命名基类符号 (依赖方向: 派生 → 基类, 无反向)。 */

    /**
     * @brief 参数级虚函数表 (vtable)
     *
     * 全系统仅 2 份实例 (app_vtable / ip_vtable)，存于 .rodata。
     * 所有参数操作 (读/写/立即写/原始写/flush/存/取/重置) 均通过此表分派。
     *
     * module_vtable 由各子系统在组装本表时填入 (App→&app_module_vtable,
     * IP→&ip_module_vtable): 基类实现据此校验"参数条目 ↔ 所属模块"的类型
     * 一致性, 从而基类代码无需也不得命名任何派生侧的 vtable 符号
     * (依赖方向: 派生 → 基类, 无反向)。
     */
    typedef struct param_vtable
    {
        /** 读取参数的当前缓存值 */
        int (*read)(param_entry_t *e, param_value_t *value);
        /** 写入参数缓存 (不立即刷硬件) */
        int (*write)(param_entry_t *e, param_value_t value);
        /** 写入参数缓存:
         *   - App 参数: 跳过 apply 回调，仅更新缓存 + 标记 dirty
         *   - IP 参数:  与 write 行为相同 (IP 无 apply 回调) */
        int (*write_cache)(param_entry_t *e, param_value_t value);
        /** 立即写入参数 — "立即" = 写到本模块可及的最深层:
         *   - IP 且注册了 write 回调: 直通硬件 (返回时寄存器已更新), 不产生 dirty;
         *   - App, 或 IP 未注册 write 回调: 更新缓存 + 清除 dirty,
         *     "立即"仅到缓存层, 硬件生效仍需 param_flush() 触发模块 flush。
         *   write 是**可选钩子** (App 与 IP 一视同仁), 缺失不报错 —— 与
         *   param_write / param_write_apply 及 IP flush 路径的既有语义一致。 */
        int (*write_immediate)(param_entry_t *e, param_value_t value);
        /** 原始字节流写入 */
        int (*write_raw)(param_entry_t *e, const uint8_t *data, uint16_t len);
        /** 单个参数的 flush 操作 */
        int (*flush)(param_entry_t *e);
        /** 将参数持久化到 Flash */
        int (*save)(param_entry_t *e);
        /** 从 Flash 恢复参数
         *  返回 param_status_t (PARAM_OK = 已恢复或存储中无该键;
         *  负值 = 错误), 与其余槽位保持一致 —— 不返回字节数,
         *  "读了多少字节"是存储后端层 (param_storage_drv_t::load) 的契约。 */
        int (*load)(param_entry_t *e);
        /** 重置参数为默认值 */
        int (*reset)(param_entry_t *e);
        /** 与本参数级 vtable 配对的模块级 vtable (条目↔模块类型一致性校验用) */
        const param_module_vtable_t *module_vtable;
    } param_vtable_t;

    /* app_vtable / ip_vtable 的声明见派生侧头文件
       (app_param_manager.h / ip_param_manager.h)。 */

    /**
     * @brief 参数基类 (平台相关: 32-bit 上 8 字节, 64-bit 上 16 字节)
     *
     * 所有参数条目均以此结构体开头，通过 vtable 指针实现多态分派。
     */
    struct param_entry
    {
        uint32_t param_id;            /**< 由 MAKE_PARAM_ID 生成的全局唯一 ID */
        const param_vtable_t *vtable; /**< 指向参数级 vtable */
    };

/**
 * @brief 公共头部宏 — 所有派生参数结构体的统一前缀 (框架内部)
 *
 * 展开为 param_entry_t base + type (1B) + flags (2B) + dirty (1B)
 *         + param_value_t cache + param_value_t default_val
 *         (平台相关: 32-bit 约 20B, 64-bit 约 36B, 不含对齐填充)。
 * 启用 PARAM_DEBUG_NAME_ENABLE 后额外增加 const char *name 字段。
 *
 * 此宏是 6 个派生结构体 (param_range_entry_t / param_enum_entry_t /
 * param_bool_entry_t / param_exec_entry_t / param_blob_entry_t /
 * param_string_entry_t) 及 param_entry_head_t 的**唯一布局来源**。
 * 修改此宏即可同步更新所有派生结构体，避免手动维护 7 处重复字段。
 *
 * @warning 应用层代码严禁直接使用此宏。请使用 PARAM_UINT /
 *          PARAM_INT / PARAM_IP_UINT 等类型化定义宏。
 */
#define PARAM_ENTRY_HEAD()     \
    param_entry_t base;        \
    uint16_t flags;            \
    uint8_t type;              \
    uint8_t dirty;             \
    param_value_t cache;       \
    param_value_t default_val; \
    IF_PARAM_DEBUG_NAME(const char *name)

#if PARAM_DEBUG_NAME_ENABLE
#define IF_PARAM_DEBUG_NAME(x) x
#define PARAM_DEBUG_NAME_INIT(nm) .name = #nm,
#else
#define IF_PARAM_DEBUG_NAME(x)
#define PARAM_DEBUG_NAME_INIT(nm)
#endif

    /**
     * @brief 数值范围型参数 (UINT / INT / FLOAT)
     */
    typedef struct
    {
        PARAM_ENTRY_HEAD();
        uint8_t has_range; /**< 是否启用范围校验 */
        param_value_t min; /**< 最小值 */
        param_value_t max; /**< 最大值 */
    } param_range_entry_t;

    /**
     * @brief 枚举型参数
     */
    typedef struct
    {
        PARAM_ENTRY_HEAD();
        const int32_t *enum_values; /**< 允许的值列表 */
        uint16_t enum_count;        /**< 列表中值的数量 */
    } param_enum_entry_t;

    /**
     * @brief 布尔型参数
     */
    typedef struct
    {
        PARAM_ENTRY_HEAD();
    } param_bool_entry_t;

    /**
     * @brief 命令型参数 (EXEC)
     *
     * 仅通过 PARAM_FLAG_EXEC 标记注册，不可通过 param_write 写入。
     * 由 param_exec / param_write_raw / param_write_immediate 路由到 exec。
     */
    typedef struct
    {
        PARAM_ENTRY_HEAD();
    } param_exec_entry_t;

    /**
     * @brief 二进制大对象 (Blob) 参数
     *
     * cache.ptr 指向外部静态缓冲区，框架不管理内存。
     * 调用者必须确保 buf 的生命周期覆盖参数使用期间。
     */
    typedef struct
    {
        PARAM_ENTRY_HEAD();
        uint16_t blob_size; /**< Blob 的字节长度 */
    } param_blob_entry_t;

    /**
     * @brief 字符串型参数 (BLOB 语义特化)
     *
     * cache.ptr 指向外部静态 char 缓冲区 (长度 = max_len + 1)。
     * 写入时自动 strncpy 并保证末尾 '\\0'。dump 输出为可读文本。
     * 调用者必须确保 buf 的生命周期覆盖参数使用期间。
     */
    typedef struct
    {
        PARAM_ENTRY_HEAD();
        uint16_t max_len; /**< 最大字符数 (不含结尾 '\\0') */
    } param_string_entry_t;

/**
 * @name 参数条目定义核心宏 (框架内部)
 *
 * App / IP 两套公开宏 (PARAM_UINT / PARAM_IP_UINT 等) 的共享初始化器。
 * 唯一差异 (行为决定者) 为 vtable 指针，由调用方通过 _vtbl 传入:
 *   App → &app_vtable, IP → &ip_vtable
 *
 * @note 应用层代码严禁直接使用本组宏。请使用 PARAM_UINT /
 *       PARAM_IP_UINT 等类型化公开宏。
 * @{
 */

/** @brief 数值范围型核心宏 (UINT) */
#define PARAM_ENTRY_RANGE_UINT(_vtbl, _name, _id, _flags, _def, _min, _max) \
    static param_range_entry_t _name = {                                    \
        .base = {(_id), (_vtbl)},                                           \
        .type = PARAM_TYPE_UINT,                                            \
        .flags = (_flags),                                                  \
        .dirty = 0,                                                         \
        .has_range = ((_min) < (_max)),                                     \
        .cache = {.u32 = (_def)},                                           \
        .default_val = {.u32 = (_def)},                                     \
        .min = {.u32 = (_min)},                                             \
        .max = {.u32 = (_max)},                                             \
        PARAM_DEBUG_NAME_INIT(_name)}

/** @brief 数值范围型核心宏 (INT) */
#define PARAM_ENTRY_RANGE_INT(_vtbl, _name, _id, _flags, _def, _min, _max) \
    static param_range_entry_t _name = {                                   \
        .base = {(_id), (_vtbl)},                                          \
        .type = PARAM_TYPE_INT,                                            \
        .flags = (_flags),                                                 \
        .dirty = 0,                                                        \
        .has_range = ((_min) < (_max)),                                    \
        .cache = {.i32 = (_def)},                                          \
        .default_val = {.i32 = (_def)},                                    \
        .min = {.i32 = (_min)},                                            \
        .max = {.i32 = (_max)},                                            \
        PARAM_DEBUG_NAME_INIT(_name)}

/** @brief 数值范围型核心宏 (FLOAT) */
#define PARAM_ENTRY_RANGE_FLOAT(_vtbl, _name, _id, _flags, _def, _min, _max) \
    static param_range_entry_t _name = {                                     \
        .base = {(_id), (_vtbl)},                                            \
        .type = PARAM_TYPE_FLOAT,                                            \
        .flags = (_flags),                                                   \
        .dirty = 0,                                                          \
        .has_range = ((_min) < (_max)),                                      \
        .cache = {.f32 = (_def)},                                            \
        .default_val = {.f32 = (_def)},                                      \
        .min = {.f32 = (_min)},                                              \
        .max = {.f32 = (_max)},                                              \
        PARAM_DEBUG_NAME_INIT(_name)}

/** @brief 枚举型核心宏 */
#define PARAM_ENTRY_ENUM(_vtbl, _name, _id, _flags, _def, _vals, _cnt) \
    static param_enum_entry_t _name = {                                \
        .base = {(_id), (_vtbl)},                                      \
        .type = PARAM_TYPE_ENUM,                                       \
        .flags = (_flags),                                             \
        .dirty = 0,                                                    \
        .cache = {.i32 = (_def)},                                      \
        .default_val = {.i32 = (_def)},                                \
        .enum_values = (_vals),                                        \
        .enum_count = (_cnt),                                          \
        PARAM_DEBUG_NAME_INIT(_name)}

/** @brief 布尔型核心宏 */
#define PARAM_ENTRY_BOOL(_vtbl, _name, _id, _flags, _def) \
    static param_bool_entry_t _name = {                  \
        .base = {(_id), (_vtbl)},                        \
        .type = PARAM_TYPE_BOOL,                         \
        .flags = (_flags),                               \
        .dirty = 0,                                      \
        .cache = {.b = (_def)},                          \
        .default_val = {.b = (_def)},                    \
        PARAM_DEBUG_NAME_INIT(_name)}

/**
 * @brief BLOB 型核心宏
 *
 * @attention cache 与 default_val 共享 _def_ptr 指向的同一缓冲区
 *   (单缓冲配置): reset 不恢复默认内容 (自拷贝被跳过)。框架有意
 *   不提供双缓冲默认值方案 — BLOB 通常体积较大, 双备份的存储
 *   收益有限。
 */
#define PARAM_ENTRY_BLOB(_vtbl, _name, _id, _flags, _def_ptr, _size) \
    static param_blob_entry_t _name = {                              \
        .base = {(_id), (_vtbl)},                                    \
        .type = PARAM_TYPE_BLOB,                                     \
        .flags = (_flags),                                           \
        .dirty = 0,                                                  \
        .cache = {.ptr = (_def_ptr)},                                \
        .default_val = {.ptr = (_def_ptr)},                          \
        .blob_size = (_size),                                        \
        PARAM_DEBUG_NAME_INIT(_name)}

/**
 * @brief STRING 型核心宏 (缓存指向调用方可写静态缓冲区, 长 max_len+1)
 *
 * @attention cache 与 default_val 共享同一指针 (单缓冲配置):
 *   - 调用方必须传入可写静态缓冲区, 严禁字符串字面量 (.rodata
 *     写入触发写保护故障);
 *   - reset 无法恢复默认 (自拷贝被跳过)。
 */
#define PARAM_ENTRY_STRING(_vtbl, _name, _id, _flags, _def_ptr, _max_len) \
    static param_string_entry_t _name = {                                \
        .base = {(_id), (_vtbl)},                                        \
        .type = PARAM_TYPE_STRING,                                       \
        .flags = (_flags),                                               \
        .dirty = 0,                                                      \
        .cache = {.ptr = (_def_ptr)},                                    \
        .default_val = {.ptr = (_def_ptr)},                              \
        .max_len = (_max_len),                                           \
        PARAM_DEBUG_NAME_INIT(_name)}

/** @brief EXEC 命令型核心宏 */
#define PARAM_ENTRY_EXEC(_vtbl, _name, _id) \
    static param_exec_entry_t _name = {     \
        .base = {(_id), (_vtbl)},           \
        .type = PARAM_TYPE_EXEC,            \
        .flags = PARAM_FLAG_EXEC,           \
        .dirty = 0,                         \
        .cache = {.u32 = 0},                \
        .default_val = {.u32 = 0},          \
        PARAM_DEBUG_NAME_INIT(_name)}

/** @} */

    /**
     * @brief 持久化后端驱动接口
     *
     * 移植到新平台时，实现此接口的所有函数即可接入存储层。
     * FlashDB、LittleFS、裸 NAND 均可通过此接口适配。
     *
     * ctx 指针使同一套回调函数支持多个物理存储实例
     * (如多分区/Bank 切换)，每个实例传入不同的 ctx 即可。
     */
    typedef struct param_storage_drv
    {
        void *ctx; /**< 实例上下文 (传递给所有回调) */
        /** 加载指定参数的数据。
         *
         *  @param ctx      实例上下文
         *  @param param_id 参数 ID
         *  @param data     目标缓冲区 (非 NULL)
         *  @param len      目标缓冲区容量 (>= 1)
         *  @param read_len [out] 实际写入 data 的字节数 (<= len), 可传 NULL。
         *                  短记录 (read_len < len) 由调用方按需补零。
         *  @return PARAM_OK               记录存在, 已拷入 data
         *          PARAM_ERR_NOT_FOUND    键不存在/已删除 → 框架保持默认值 (非错误)
         *          PARAM_ERR_OUT_OF_RANGE 存储记录长度超出 len (格式变更/数据损坏)
         *          其他负值               读取失败, 错误码上抛到 param_load_all
         *
         *  @note 状态与长度分离: 返回值只承载"存在/不存在/失败"三态, 长度经
         *        read_len 出参传递 (适配层需要它给短记录补零)。三态必须彼此
         *        可分 —— 若用"返回字节数"编码 (0 = 不存在, 负值 = 失败), 后端
         *        无法为不同失败原因产生不同的负值, 真实读失败与数据损坏就会被
         *        当成"键不存在"静默吞掉。
         *  @note 无法取得记录原始长度的后端 (如 mkv_get 内部按 max_len 截断)
         *        可跳过 PARAM_ERR_OUT_OF_RANGE 检查。 */
        int (*load)(void *ctx, uint32_t param_id, uint8_t *data, uint16_t len,
                    uint16_t *read_len);
        /** 批量加载全部参数。NULL=走逐条 load；非 NULL=param_load_all 优先调用。
         *  后端应一次扫描完成所有数据恢复（如通过 param_cache_restore），
         *  典型场景是追加写日志型存储（mkv）的 O(N²)→O(N) 优化。
         *  返回值约定: 0=全部已恢复；>0=扫描不完整 (如单条记录超出后端扫描
         *  缓冲，大 BLOB/STRING)，param_load_all 会回退到逐条 load；<0=失败。 */
        int (*load_all)(void *ctx);
        /** 保存指定参数的数据。删除操作请用 delete 回调，不要用 len==0 隐式删除。
         *
         *  @return 0 = 已写入; 非 0 = 失败。失败原因不区分 (含后端统一为 -1 的
         *          情形), 框架在 API 边界统一翻译为 PARAM_ERR_FLASH_FAIL。 */
        int (*save)(void *ctx, uint32_t param_id, const uint8_t *data, uint16_t len);
        /** 删除单个参数的持久化数据 (NULL = 后端不支持)
         *  @return 0 = 已删除; 非 0 = 失败 → PARAM_ERR_FLASH_FAIL */
        int (*delete)(void *ctx, uint32_t param_id);
        /** 擦除全部持久化数据
         *  @return 0 = 已擦除; 非 0 = 失败 → PARAM_ERR_FLASH_FAIL */
        int (*erase_all)(void *ctx);
        /** 去初始化存储后端 */
        int (*deinit)(void *ctx);
        /** 读取当前激活分区索引 (PARAM_PARTITION_FACTORY~PARAM_PARTITION_USER_MAX, NULL=单分区不支持) */
        int (*get_active_partition)(void *ctx, uint8_t *index);
        /** 写入激活分区索引 — 写入 FAL_BOOT_PART 裸分区，下次启动生效 (NULL=不支持) */
        int (*set_active_partition)(void *ctx, uint8_t index);
        /** 按索引获取指定分区的存储驱动 — 单例语义 (PARAM_PARTITION_FACTORY=factory, 其他参见 PARAM_PARTITION_USER_MIN~MAX) */
        const struct param_storage_drv *(*get_partition)(void *ctx, uint8_t index);
    } param_storage_drv_t;

    /**
     * @brief 模块基类 (链表节点)
     *
     * App 和 IP 模块共用此基类，挂入全局单条链表。
     * vtable 多态消除类型分支。
     */
    struct param_module_node
    {
        param_module_node_t *next;           /**< 链表下一节点 */
        uint16_t module_id;                  /**< 模块 ID (App 0x01~0x7F, IP 0x80~0xFF, 见 module_ids.h) */
        uint16_t param_count;                /**< 该模块的参数数量 */
        const char *name;                    /**< 模块名称 (调试用) */
        param_entry_t **table;               /**< 参数条目指针数组 */
        const param_module_vtable_t *vtable; /**< 指向模块级 vtable */
        uint8_t dirty : 1;                   /**< 模块级 dirty 标志 */
        uint8_t initialized : 1;             /**< 初始化完成标志 (vtable->init 成功置位, deinit 清除) */
    };

    /**
     * @brief 模块实例 (App 与 IP 共用, 模块基类派生)
     *
     * App 与 IP 共用此唯一结构体定义 (IP 侧以别名 ip_instance_t 引用,
     * 见 ip_param_manager.h)。回调语义两侧对称:
     *   read  (NULL=直接返缓存) / write / flush / init / exec
     *
     * ctx 字段的双重语义:
     *   - App 模块: 业务实例私有上下文
     *   - IP 模块:  driver 私有数据指针
     */
    struct param_module
    {
        param_module_node_t node; /**< 基类节点 */
        param_read_fn read;       /**< 参数读取回调 (NULL = 直接返缓存) */
        param_write_fn write;     /**< 参数写入回调 (校验/转换) */
        param_flush_fn flush;     /**< 刷入硬件的回调 */
        param_init_fn init;       /**< 模块初始化回调 */
        param_exec_fn exec;       /**< 模块命令执行回调 */
        void *ctx;                /**< 模块私有上下文 (IP 侧: driver 私有数据) */
    };

    /**
     * @brief 全局统计信息
     */
    typedef struct
    {
        uint16_t module_count;           /**< 已注册模块数 */
        uint16_t param_count;            /**< 已注册参数总数 */
        uint16_t dirty_count;            /**< 当前 dirty 参数数 */
        uint16_t persist_count;          /**< 需持久化的参数数 */
        uint16_t flush_error_count;      /**< flush 失败累计 */
        uint16_t flush_order_miss_count; /**< 不在 ORDER 中的模块数 */
    } param_stats_t;

    /**
     * @defgroup public_api 应用层 API — 用户代码直接调用
     * @{
     *
     * 分组顺序: 生命周期 → 存储后端管理 → 写入 → 读取 → exec →
     *           flush → 持久化CRUD → 重置 → 统计/遍历/工具
     */

    /* ================================================================
     *  生命周期
     * ================================================================ */

    /**
     * @brief 初始化参数管理框架
     *
     * 驱动由工厂函数预初始化后传入，此函数仅存储指针。
     *
     * @param storage 持久化后端驱动 (可为 NULL)
     * @param notify  参数变化通知回调 (可为 NULL, 由回调实现者自行处理防重入)
     * @return PARAM_OK 成功，PARAM_ERR_BUSY 重复初始化
     */
    int param_init(const param_storage_drv_t *storage, param_notify_fn notify);

    /**
     * @brief 反初始化，释放所有资源
     */
    void param_deinit(void);

    /** @brief 查询参数管理框架是否已初始化 (param_init 成功后 true, param_deinit 后 false) */
    bool param_is_initialized(void);

    /** @brief 查询指定模块是否已完成初始化 (vtable->init 成功后置位, deinit 清除) */
    bool param_module_is_initialized(uint16_t module_id);

    /* ================================================================
     *  存储后端管理
     * ================================================================ */

    /**
     * @brief 运行时替换持久化后端驱动 — 纯指针替换
     *
     * 仅替换 g_pm.storage 指针，不触发旧驱动的 deinit，不触发 save/load。
     * 旧驱动在替换后仍然有效，可保存指针并在之后切回。
     *
     * @param storage 新的存储后端驱动 (可为 NULL 停用持久化)
     */
    void param_set_storage(const param_storage_drv_t *storage);

    /**
     * @brief 按分区索引获取存储后端驱动实例
     *
     * 通过当前存储驱动的 get_partition 虚函数获取指定分区的驱动。
     * 单例语义 (工厂模式): 同一分区索引多次调用返回同一实例。
     *
     * @param index 分区索引 (PARAM_PARTITION_FACTORY=factory, PARAM_PARTITION_USER_MIN~MAX=用户)
     * @return 驱动指针，当前存储为 NULL 或不支持 get_partition 时返回 NULL
     */
    const param_storage_drv_t *param_get_storage_partition(uint8_t index);

    /** @brief 获取当前存储后端的激活分区索引
     *  @param index [out] 分区索引
     *  @return PARAM_OK 成功; PARAM_ERR_NOT_SUPPORTED 单分区/无存储;
     *          PARAM_ERR_FLASH_FAIL 读启动分区失败 */
    int param_storage_get_active_partition(uint8_t *index);
    /** @brief 设置存储后端的激活分区索引 (下次启动生效)
     *  @return PARAM_OK 成功; PARAM_ERR_NOT_SUPPORTED 单分区/无存储;
     *          PARAM_ERR_FLASH_FAIL 写入启动分区失败 */
    int param_storage_set_active_partition(uint8_t index);

    /* ================================================================
     *  参数写入
     * ================================================================ */

    /**
     * @brief 写入参数 (缓存模式，不立即刷硬件)
     *
     * @note 仅适用于值类型 (UINT/INT/FLOAT/BOOL/ENUM/EXEC)。
     *   BLOB/STRING 的 payload 经 .ptr 传递, 而 param_value_t 无法携带长度,
     *   本入口无法校验源的容量/有效性 (向 STRING 条目写 .u32 会被当成
     *   指针解引用) → 返回 PARAM_ERR_TYPE_MISMATCH。
     *   请改用带长度的专用入口: param_write_blob / param_write_string /
     *   param_write_raw。
     *
     * @param param_id 参数 ID
     * @param value    新值
     * @return PARAM_OK 成功，或错误码
     */
    int param_write(uint32_t param_id, param_value_t value);

    /**
     * @brief 写入参数缓存 (跳过 apply 回调)
     *
     * 只更新缓存值并标记 dirty，不调用模块的 apply 回调。
     * 适用场景: apply_cb 内部需要对值做裁剪修正，避免重入 param_write。
     *
     * App/IP 差异:
     *   - App 参数: 跳过 apply，与 param_write 的区别在于不触发业务校验。
     *   - IP 参数:  与 param_write 行为完全相同 (IP 无 apply 回调)。
     *
     * @note 同 param_write: 不接受 BLOB/STRING (无长度信息)。
     *
     * @param param_id 参数 ID
     * @param value    新值
     * @return PARAM_OK 成功，或错误码
     */
    int param_write_cache(uint32_t param_id, param_value_t value);

    /**
     * @brief 立即写入参数
     *
     * "立即"= 写到参数所属模块可及的最深层, 不承诺硬件已更新:
     *   - IP 参数: write 回调存在时直通硬件，更新缓存，不产生 dirty。
     *   - App 参数: 执行 apply 校验回调，更新缓存并清除 dirty 标记。
     *     注意: App 参数不会写硬件，实际硬件写入由 param_flush() 触发。
     *   - 未注册 write 回调 (App / IP 一视同仁): 仅更新缓存 + 清 dirty 并
     *     返回 PARAM_OK。适用于只用 flush 批量下发或只用 exec 命令驱动
     *     硬件的模块 —— write 是可选钩子, 不是写入的必要条件。
     *
     * @note 同 param_write: 不接受 BLOB/STRING (无长度信息)。
     *
     * @param param_id 参数 ID
     * @param value    新值
     * @return PARAM_OK 成功，或错误码
     */
    int param_write_immediate(uint32_t param_id, param_value_t value);

    /**
     * @brief 原始字节流写入
     * @param param_id 参数 ID
     * @param data     数据缓冲区
     * @param len      数据长度
     * @return PARAM_OK 成功，或错误码
     */
    int param_write_raw(uint32_t param_id, const uint8_t *data, uint16_t len);

    /**
     * @brief 用缓存当前值重新执行完整写入路径 (re-apply)
     *
     * 直接取 entry->cache 作为新值走 param_write 完整流程:
     * pre_write 校验(裁剪/枚举校验) → write/apply 回调 → 缓存更新 →
     * dirty 标记 → notify 通知。不触发任何硬件/回调读取。
     *
     * 适用场景: 外部条件变化 (range 收紧、模式切换、约束变更) 后，
     * 需要对已缓存参数重新校验/下发，无需调用者先 param_read 再 param_write。
     *
     * @note READONLY / EXEC 参数按写入规则拒绝。
     *
     * @param param_id 参数 ID
     * @return PARAM_OK 成功，或错误码
     */
    int param_update(uint32_t param_id);

    /* ================================================================
     *  参数读取
     * ================================================================ */

    /**
     * @brief 读取参数当前值 (优先硬件/回调读取，失败回退缓存)
     *
     * @attention STRING/BLOB 类型的返回值为指向框架内部静态缓冲区的指针
     *   (value->ptr)，并非值的拷贝:
     *   - 其他线程并发写入该参数后，指针内容会随之改变 (非快照)
     *   - 指针生命周期与参数条目绑定，禁止长期持有或释放
     *   - 需要安全拷贝时请改用 param_read_string() / param_read_raw()
     *
     * @param param_id 参数 ID
     * @param value    [out] 接收值
     * @return PARAM_OK 成功，或错误码
     */
    int param_read(uint32_t param_id, param_value_t *value);

    /**
     * @brief 读取参数当前缓存值 (纯缓存读取，绝不触发硬件/回调)
     *
     * 与 param_read 的区别: 直接返回 entry->cache，不调用 App 模块的
     * read 回调或 IP driver 的硬件读取。
     *
     * 适用场景: read-modify-write (读缓存→修改→param_write_cache)、
     * 高频轮询展示、硬件未就绪时需要确定性返回的场合。
     *
     * @param param_id 参数 ID
     * @param value    [out] 接收值 (STRING/BLOB 返回内部缓冲区指针，
     *                 风险与 param_read 相同)
     * @return PARAM_OK 成功，或错误码
     */
    int param_read_cache(uint32_t param_id, param_value_t *value);

    /**
     * @brief 原始字节流读取参数
     *
     * 对值类型 (UINT/INT/FLOAT/BOOL/ENUM): 拷贝 sizeof(param_value_t) 字节。
     * 对 BLOB/STRING 类型: 拷贝外部缓冲区的数据。
     *
     * @param param_id 参数 ID
     * @param data     用户缓冲区 (可为 NULL，此时仅查询长度)
     * @param len      [in/out] 输入为缓冲区容量, 输出为实际拷贝字节数
     * @return PARAM_OK 成功, PARAM_ERR_INVALID_ID 参数不存在
     */
    int param_read_raw(uint32_t param_id, uint8_t *data, uint16_t *len);

    /**
     * @name 类型化读写快捷函数
     *
     * 封装 param_write / param_read，避免手动构造 param_value_t。
     * @{
     */

    /** @brief uint32_t 类型写入 */
    int param_write_u32(uint32_t id, uint32_t val);
    /** @brief int32_t 类型写入 */
    int param_write_i32(uint32_t id, int32_t val);
    /** @brief float 类型写入 */
    int param_write_f32(uint32_t id, float val);
    /** @brief bool 类型写入 */
    int param_write_bool(uint32_t id, bool val);

    /** @brief uint32_t 类型读取 */
    int param_read_u32(uint32_t id, uint32_t *val);
    /** @brief int32_t 类型读取 */
    int param_read_i32(uint32_t id, int32_t *val);
    /** @brief float 类型读取 */
    int param_read_f32(uint32_t id, float *val);
    /** @brief bool 类型读取 */
    int param_read_bool(uint32_t id, bool *val);

    /** @} */

    /**
     * @name STRING / BLOB 快捷函数
     * @{
     */

    /** @brief STRING 类型读取到调用者缓冲区, 超 buf_size 自动截断, 末尾始终补 '\0' */
    int param_read_string(uint32_t id, char *buf, uint16_t buf_size);
    /** @brief STRING 类型写入, 超 max_len 自动截断 */
    int param_write_string(uint32_t id, const char *str);

    /**
     * @brief BLOB 类型写入 (带长度的专用入口)
     *
     * BLOB 必须整块写: len 与条目 blob_size 不等时直接拒绝 (不截断/不补零)。
     * 写入后以缓存自身为源重放完整写入路径 (pre_write → apply → dirty → notify)。
     *
     * @param id  参数 ID (类型必须为 PARAM_TYPE_BLOB)
     * @param buf 源数据 (非 NULL)
     * @param len 源长度 (必须等于 blob_size)
     * @return PARAM_OK 成功; PARAM_ERR_INVALID_ID 源为 NULL;
     *         PARAM_ERR_TYPE_MISMATCH 目标非 BLOB;
     *         PARAM_ERR_OUT_OF_RANGE 长度不符
     */
    int param_write_blob(uint32_t id, const void *buf, uint16_t len);

    /** @} */

    /* ================================================================
     *  exec 命令执行
     * ================================================================ */

    /**
     * @brief 执行模块命令
     *
     * 校验 cmd_id 已作为 PARAM_FLAG_EXEC 参数注册，随后查找模块节点，
     * 调用其 exec(param_id, arg) 直接传入全局唯一参数 ID。
     * arg 为 param_value_t 联合体，由 user_arg 转换而成。
     *
     * - param_write / param_write_raw / param_write_immediate 遇到
     *   PARAM_FLAG_EXEC 参数时自动路由到 exec，无需调用者判断类型。
     *
     * @param cmd_id  命令 ID (MAKE_PARAM_ID 风格，高 16 位 module_id)
     * @param user_arg 命令参数 (可为 NULL，框架内部转为 param_value_t)
     * @return 回调返回值，PARAM_ERR_NOT_FOUND 表示未注册或模块无 exec
     */
    int param_exec(uint32_t cmd_id, void *user_arg);

    /* ================================================================
     *  硬件刷新
     * ================================================================ */

    /**
     * @brief 将所有模块的 dirty 参数刷入硬件
     *
     * 遍历模块链表 (按 MODULE_INIT_ORDER 顺序)，对每个 dirty 模块调用 module->vtable->flush。
     * 即使某个模块 flush 失败，仍会继续处理后续模块。
     *
     * @note 锁粒度: 全局锁仅用于快照模块指针, 所有 flush 回调
     *   (含驱动寄存器写 / 总线访问) 均在锁外执行。回调内可安全调用
     *   param_read / param_read_cache / param_save_one 等加锁 API,
     *   慢速总线写也不会阻塞其他任务的参数访问。
     *   回调内不得递归调用 param_flush。
     * @note 失败重试语义: flush 失败的参数保留 dirty 标记 (缓存不回滚)，
     *   下次调用 param_flush() 会自动重试，直到硬件收敛到缓存值。
     *   App 模块 flush 回调失败时同样保留 dirty。
     * @return PARAM_OK 全部成功，或最后一个失败的错误码;
     *         PARAM_ERR_BUSY 已有 flush 在进行中
     */
    int param_flush(void);

    /**
     * @brief 校验所有已注册模块是否都在 MODULE_INIT_ORDER 中
     *
     * 未覆盖的模块计入 flush_order_miss_count。
     * @return PARAM_OK 全部覆盖，PARAM_ERR_NOT_FOUND 存在未覆盖模块
     */
    int param_check_flush_integrity(void);

    /* ================================================================
     *  持久化 CRUD
     *
     *  @note 存储后端未配置 (param_init 传 NULL / 未编译持久化) 或后端缺
     *        对应回调时, 下列接口返回 PARAM_ERR_NOT_SUPPORTED (而非
     *        PARAM_ERR_NOT_FOUND —— "支持但找不到"与"本固件不支持"是两件事)。
     *        例外: param_load_all 在无存储时不报错, 只把 LOAD 阶段跳过
     *        (INIT 阶段照常执行, 见其文档)。
     * ================================================================ */

    /** @brief 保存所有参数到持久化存储 (仅 PARAM_FLAG_PERSIST 标记的参数)
     *
     *  @return PARAM_OK 全部成功 (含未标记 PERSIST、因而未写介质的条目);
     *          PARAM_ERR_FLASH_FAIL 首个失败条目; PARAM_ERR_NOT_SUPPORTED 本固件
     *          不持久化; PARAM_ERR_NOT_FOUND 框架未初始化
     *  @note 写介质失败统一译作 PARAM_ERR_FLASH_FAIL —— 后端以 -1 报错, 若
     *        直接上抛会与 PARAM_ERR_INVALID_ID 撞码。 */
    int param_save_all(void);
    /** @brief 保存单个参数到持久化存储
     *  @return 与 param_save_all 同码; 另加 PARAM_ERR_INVALID_ID (ID 未注册)。
     *          未标记 PERSIST 的参数视为成功 (不写介质)。 */
    int param_save_one(uint32_t param_id);

    /**
     * @brief 从持久化存储加载所有参数
     *
     * 两阶段执行:
     *   - LOAD 阶段: 将存储的值恢复到全部条目的缓存 (仅回填缓存,
     *     不做逐条校验/下发);
     *   - INIT 阶段: 按 MODULE_INIT_ORDER 顺序调用各模块的 init 回调。
     *
     * @par 恢复后的生效契约 (全量重初始化模型)
     * 恢复后**不**逐条 apply/write: 批量加载无可用顺序 (LOAD 阶段按
     * 哈希槽序遍历), 逐条校验会因兄弟参数尚未恢复而产生假拒绝与
     * N 次副作用。init 回调面对的是完整恢复后的缓存集合, 是批量
     * 校验/下发 (App 侧 apply) 的唯一入口 —— 在回调中通过
     * param_read() 即可读取全部已恢复值。
     * 未实现 init 回调的模块, 其恢复值不做校验 (仅回填缓存)。
     * 与 param_load_one 对照: 单参数加载无 init 阶段, 因此以 write
     * 回调作为恢复后的重新校验入口。
     *
     * @return PARAM_OK 成功，或第一个失败的错误码
     */
    int param_load_all(void);
    /** @brief 从持久化存储加载单个参数 (恢复后经 write 回调重新校验, 见 param_load_all 的生效契约) */
    int param_load_one(uint32_t param_id);

    /**
     * @brief 删除单个参数的持久化数据
     * @param param_id 参数 ID
     * @return PARAM_OK 成功，PARAM_ERR_FLASH_FAIL 删除失败，PARAM_ERR_NOT_SUPPORTED 存储不可用
     */
    int param_delete_one(uint32_t param_id);
    /**
     * @brief 擦除全部持久化数据
     * @return PARAM_OK 成功，PARAM_ERR_FLASH_FAIL 擦除失败，PARAM_ERR_NOT_SUPPORTED 存储不可用
     */
    int param_delete_all(void);

    /* ================================================================
     *  重置
     * ================================================================ */

    /**
     * @brief 重置所有参数为默认值 (default_val)
     *
     * 两阶段执行: 先重置全部条目缓存与 dirty, 再按 MODULE_INIT_ORDER
     * 调用各模块 init。IP 模块的 init 会把其可写参数重新标脏
     * (默认值仍需下发硬件), 下一次 param_flush() 后收敛清零。
     *
     * @note BLOB/STRING 为单缓冲配置 (缓存与默认值共享缓冲, 有意
     *   不提供双缓冲默认值), reset 不恢复其初始默认内容 (自拷贝
     *   被跳过, 仅清 dirty 保持现状)。
     */
    int param_reset_all(void);
    /** @brief 重置指定参数为默认值 (BLOB/STRING 单缓冲, 不恢复初始内容, 语义同 param_reset_all) */
    int param_reset_one(uint32_t param_id);

    /* ================================================================
     *  统计 / 遍历 / 工具
     * ================================================================ */

    /** @brief 获取全局统计信息快照 */
    void param_get_stats(param_stats_t *stats);
    /** @brief 清零统计计数器 */
    void param_clear_stats(void);

    /**
     * @brief 获取参数数据大小 (字节数)
     *
     * - UINT/INT/FLOAT/BOOL/ENUM/EXEC → sizeof(param_value_t) (平台相关: 32-bit 4B, 64-bit 8B)
     * - BLOB → blob_size
     * - STRING → max_len + 1 (含结尾 '\0')
     * - 未注册 → 0
     */
    uint16_t param_get_size(uint32_t param_id);

    /**
     * @brief 参数遍历回调
     * @param entry     当前参数条目
     * @param user_data 用户传入的上下文
     * @return true 继续遍历，false 终止
     */
    typedef bool (*param_foreach_fn)(param_entry_t *entry, void *user_data);

    /**
     * @brief 遍历参数
     *
     * @note 回调在锁外执行: 回调内可安全调用 param_read / param_write /
     *   param_save_one 等加锁 API, 不会自死锁。回调收到的条目指针
     *   生命周期与注册表一致, 其缓存内容为 best-effort 快照
     *   (可能被并发写改变)。
     * @param module_id 指定模块 ID (0 表示遍历所有模块)
     * @param cb        回调函数
     * @param user_data 用户上下文
     */
    void param_foreach(uint16_t module_id, param_foreach_fn cb, void *user_data);

    /**
     * @brief 运行时设置参数的范围
     *
     * 适用于 UINT/INT/FLOAT 数值参数 (App 与 IP 均可)。若范围收紧导致
     * 当前缓存越界, 缓存会被裁剪到新范围并按写入路径重新标脏下发。
     * @param param_id 参数 ID
     * @param min_val  新最小值 (NULL 表示不改)
     * @param max_val  新最大值 (NULL 表示不改)
     * @return PARAM_OK 成功，或错误码
     */
    int param_set_range(uint32_t param_id,
                        const param_value_t *min_val,
                        const param_value_t *max_val);

    /**
     * @brief 对所有已注册参数执行范围裁剪
     *
     * 遍历所有模块的参数表，对超范围的值裁剪到合法区间。
     */
    void param_validate_all(void);

    /** @} */ /* public_api */

/**
 * @defgroup migration 参数版本迁移
 * @{
 */

/** 当前存储格式版本号。破坏性变更时递增 (极少发生)。 */
#define PARAM_SCHEMA_VERSION 1

    /**
     * @brief 迁移转换回调
     *
     * 框架已从存储后端读取旧值到 old_data[old_len]。
     * 回调负责填充 new_id / new_data / new_len。
     *
     * @param old_data  旧值原始字节
     * @param old_len   旧值字节数
     * @param new_id    [out] 新参数 ID
     * @param new_data  [out] 转换后数据 (框架提供 256 字节缓冲区)
     * @param new_len   [out] 转换后数据长度
     * @param ctx       迁移条目上下文
     * @return PARAM_OK 成功; -1 跳过本条; 其他 <0 中断迁移
     */
    typedef int (*param_migrate_fn)(const uint8_t *old_data, uint16_t old_len,
                                    uint32_t *new_id,
                                    uint8_t *new_data, uint16_t *new_len,
                                    void *ctx);

    /** 迁移条目 */
    typedef struct
    {
        uint32_t old_id;          /**< 旧参数 ID */
        param_migrate_fn convert; /**< NULL = 简单 rename 到 new_id */
        uint32_t new_id;          /**< convert==NULL 时作为目标 ID */
        void *ctx;                /**< 回调上下文 */
    } param_migrate_entry_t;

/** 定义迁移表 (ROM) */
#define PARAM_MIGRATE_TABLE(_name, ...) \
    static const param_migrate_entry_t _name[] = {__VA_ARGS__}

/**
 * @brief 执行存储层参数迁移
 * 在 param_init 之后、param_load_all 之前调用。
 * 所有操作直接走 storage 驱动，不依赖内存哈希表。
 *
 * 内部流程:
 *   1. 读 PID_SCHEMA_VER → 不存在 → 首次启动，写版本号
 *                        → 不匹配 → 全量擦除，写版本号
 *                        → 匹配   → 执行迁移表
 *   2. 遍历迁移表: load(old) → convert/save(new) → delete(old)
 *
 * @param storage  持久化后端
 * @param table    迁移表 (ROM)
 * @param count    表长度
 * @return PARAM_OK 成功
 */
int param_migrate_storage(const param_storage_drv_t *storage,
                              const param_migrate_entry_t *table,
                              uint16_t count);
/** @} */ /* migration */

/**
 * @name 模块实例定义核心宏 (框架内部)
 *
 * PARAM_MODULE_DEFINE / IP_DRIVER_DEFINE 与 PARAM_MODULE_INIT /
 * IP_DRIVER_INIT 的共享初始化器。唯一行为差异 (vtable / 注册函数 /
 * 变量名) 由调用方通过参数注入, 与 PARAM_ENTRY_* 核心宏同模式。
 *
 * @note 应用层代码严禁直接使用本组宏。请使用 PARAM_MODULE_DEFINE /
 *       IP_DRIVER_DEFINE / PARAM_MODULE_INIT / IP_DRIVER_INIT。
 * @{
 */

/**
 * @brief 模块实例定义核心宏 (框架内部)
 *
 * @param _vtbl     模块级 vtable (&app_module_vtable / &ip_module_vtable)
 * @param _var      生成的实例变量名 (包装宏负责 _module/_instance 后缀)
 * @param _mod_id   模块 ID
 * @param _label    模块调试名称
 * @param _ctx      模块私有上下文 (App: 业务实例 / IP: driver 数据)
 * @param _init_fn  初始化回调 (可 NULL)
 * @param _read_fn  read 回调 (可 NULL = 直接返缓存)
 * @param _write_fn write 回调 (可 NULL)
 * @param _exec_fn  exec 命令回调 (可 NULL)
 * @param _flush_fn flush 回调
 */
#define PARAM_MODULE_DEFINE_CORE(_vtbl, _var, _mod_id, _label, _ctx,            \
                                 _init_fn, _read_fn, _write_fn, _exec_fn, _flush_fn) \
    static param_module_t _var = {                                              \
        .node = {                                                               \
            .module_id = (_mod_id),                                             \
            .name = (_label),                                                   \
            .vtable = (_vtbl),                                                  \
        },                                                                      \
        .ctx = (_ctx),                                                          \
        .init = (_init_fn),                                                     \
        .read = (_read_fn),                                                     \
        .write = (_write_fn),                                                   \
        .exec = (_exec_fn),                                                     \
        .flush = (_flush_fn),                                                   \
    }

/**
 * @brief 模块注册函数核心宏 (框架内部)
 *
 * @param _reg_fn    注册函数 (param_module_register / ip_driver_register)
 * @param _init_name 生成的注册函数名
 * @param _inst_var  模块实例变量名
 * @param _entries   参数表数组
 */
#define PARAM_MODULE_INIT_CORE(_reg_fn, _init_name, _inst_var, _entries)        \
    void _init_name(void)                                                       \
    {                                                                           \
        _reg_fn(&_inst_var, (_entries),                                         \
                sizeof(_entries) / sizeof((_entries)[0]));                      \
    }

/** @} */

/**
 * @brief 定义参数表 (静态数组)
 * @param _name 数组名
 * @param ...   以逗号分隔的参数条目指针 (&xxx.base)
 */
#define PARAM_TABLE(_name, ...) \
    static param_entry_t *_name[] = {__VA_ARGS__}

/**
 * @brief 获取参数表元素个数
 * @param _arr 参数表数组名
 */
#define PARAM_COUNT(_arr) (sizeof(_arr) / sizeof((_arr)[0]))

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
    _Static_assert(offsetof(param_module_node_t, module_id) == sizeof(void *),
                   "param_module_node_t.module_id offset mismatch");
#endif

#ifdef __cplusplus
}
#endif
#endif /* PARAM_MANAGER_H */
