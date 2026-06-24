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
     * 设计要点:
     *   - 基于 vtable 的 OOP-in-C 多态: 模块级和参数级各一套虚函数表
     *   - 所有参数条目静态定义在编译期 ROM 中，零动态分配
     *   - 类型分派表完全消除 switch-case 分支
     *   - App 模块与 IP 模块共用单条链表，通过 vtable 区分行为
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
    } param_status_t;

    typedef struct param_module param_module_t;
    typedef struct param_entry param_entry_t;

    typedef struct param_module_node param_module_node_t;

    /**
     * @brief 参数变化通知回调
     *
     * 当参数值通过 param_write / param_write_cache / param_write_immediate /
     * param_write_string 成功写入后调用。
     * 回调在 vtable->write 的 LOCK/UNLOCK 区域之外执行。
     * 回调内禁止调用 param_write / param_write_cache / param_write_immediate /
     * param_write_string，是否防重入由回调实现者自行决定。
     *
     * @attention 多线程并发写入同一参数时，notify 可能收到中间值
     *   (另一个线程在 notify 发出后立即覆盖了参数值)。
     *   new_value 应视为 best-effort 快照，不应假设其与缓存当前值一致。
     *
     * @param param_id  发生变化的参数 ID
     * @param new_value 写入后的新值 (best-effort 快照)
     */
    typedef void (*param_notify_fn)(uint32_t param_id, param_value_t new_value);

    /**
     * @brief App 参数读取回调
     *
     * 若提供且返回 PARAM_OK，框架回写缓存后返回。
     * NULL 或返回错误 → 回退到读取缓存值 (与 IP ip_param_read 对称)。
     *
     * @param ctx      模块私有上下文
     * @param param_id 参数 ID
     * @param value    [out] 读到的值
     * @return PARAM_OK 成功 (触发缓存回写)，其他值回退缓存
     */
    typedef int (*param_read_fn)(void *ctx, uint32_t param_id, param_value_t *value);

    /**
     * @brief App 参数写入回调 (与 IP write 回调语义对称)
     * @param param_id 参数 ID
     * @param value    待写入的新值
     * @return PARAM_OK 表示接受，其他值表示拒绝
     */
    typedef int (*param_write_fn)(void *ctx, uint32_t param_id, param_value_t value);

    /**
     * @brief App 模块 flush 回调 — 将缓存的参数批量写入硬件
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
     * @param ctx      App 模块为 ctx (业务实例), IP 模块为 driver
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

    /** App 模块的 vtable 实现 */
    extern const param_module_vtable_t app_module_vtable;
    /** IP 模块的 vtable 实现 */
    extern const param_module_vtable_t ip_module_vtable;

    /**
     * @brief 参数级虚函数表 (vtable)
     *
     * 全系统仅 2 份实例 (app_vtable / ip_vtable)，存于 .rodata。
     * 所有参数操作 (读/写/立即写/原始写/flush/存/取/重置) 均通过此表分派。
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
        /** 立即写入参数 — 行为取决于模块类型 (有意分裂):
         *   - IP 参数: 通过 driver->write 直通硬件，更新缓存，不产生 dirty。
         *              这是真正的"立即" — 硬件寄存器在函数返回时已更新。
         *   - App 参数: 执行 apply 校验 + 更新缓存 + 清除 dirty，
         *              不写硬件。App 模块没有硬件，"立即"仅到缓存层。
         *              硬件生效仍需 param_flush() 触发 IP 模块批量刷入。 */
        int (*write_immediate)(param_entry_t *e, param_value_t value);
        /** 原始字节流写入 */
        int (*write_raw)(param_entry_t *e, const uint8_t *data, uint16_t len);
        /** 单个参数的 flush 操作 */
        int (*flush)(param_entry_t *e);
        /** 将参数持久化到 Flash */
        int (*save)(param_entry_t *e);
        /** 从 Flash 恢复参数 */
        int (*load)(param_entry_t *e);
        /** 重置参数为默认值 */
        int (*reset)(param_entry_t *e);
    } param_vtable_t;

    /** App 参数的 vtable 实现 */
    extern const param_vtable_t app_vtable;
    /** IP 参数的 vtable 实现 */
    extern const param_vtable_t ip_vtable;

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
        /** 加载指定参数的数据。返回实际读取字节数，≤0 表示不存在。 */
        int (*load)(void *ctx, uint32_t param_id, uint8_t *data, uint16_t len);
        /** 批量加载全部参数。NULL=走逐条 load；非 NULL=param_load_all 优先调用。
         *  后端应一次扫描完成所有数据恢复（如通过 param_cache_restore），
         *  典型场景是追加写日志型存储（mkv）的 O(N²)→O(N) 优化。 */
        int (*load_all)(void *ctx);
        /** 保存指定参数的数据。删除操作请用 delete 回调，不要用 len==0 隐式删除。 */
        int (*save)(void *ctx, uint32_t param_id, const uint8_t *data, uint16_t len);
        /** 删除单个参数的持久化数据 (NULL = 后端不支持) */
        int (*delete)(void *ctx, uint32_t param_id);
        /** 擦除全部持久化数据 */
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
        uint16_t module_id;                  /**< 模块 ID (0x01~0xFF) */
        uint16_t param_count;                /**< 该模块的参数数量 */
        const char *name;                    /**< 模块名称 (调试用) */
        param_entry_t **table;               /**< 参数条目指针数组 */
        const param_module_vtable_t *vtable; /**< 指向模块级 vtable */
        uint8_t dirty : 1;                   /**< 模块级 dirty 标志 */
    };

    /**
     * @brief App 模块 (模块基类派生)
     *
     * 回调与 IP 模块的 driver 回调语义对称:
     *   read  (NULL=直接返缓存) / write / flush / init / exec
     */
    struct param_module
    {
        param_module_node_t node; /**< 基类节点 */
        param_read_fn read;       /**< 参数读取回调 (NULL = 直接返缓存) */
        param_write_fn write;     /**< 参数写入回调 (校验/转换) */
        param_flush_fn flush;     /**< 刷入硬件的回调 */
        param_init_fn init;       /**< 模块初始化回调 */
        param_exec_fn exec;       /**< 模块命令执行回调 */
        void *ctx;                /**< 模块私有上下文 */
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

    /** @brief 获取当前存储后端的激活分区索引 */
    int param_storage_get_active_partition(uint8_t *index);
    /** @brief 设置存储后端的激活分区索引 (下次启动生效) */
    int param_storage_set_active_partition(uint8_t index);

    /* ================================================================
     *  参数写入
     * ================================================================ */

    /**
     * @brief 写入参数 (缓存模式，不立即刷硬件)
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
     * @param param_id 参数 ID
     * @param value    新值
     * @return PARAM_OK 成功，或错误码
     */
    int param_write_cache(uint32_t param_id, param_value_t value);

    /**
     * @brief 立即写入参数
     *
     * 行为取决于参数所属模块类型:
     *   - IP 参数: 通过 write 直通硬件，更新缓存，不产生 dirty。
     *   - App 参数: 执行 apply 校验回调，更新缓存并清除 dirty 标记。
     *     注意: App 参数不会写硬件，实际硬件写入由 param_flush() 触发。
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

    /* ================================================================
     *  参数读取
     * ================================================================ */

    /**
     * @brief 读取参数当前缓存值
     * @param param_id 参数 ID
     * @param value    [out] 接收值
     * @return PARAM_OK 成功，或错误码
     */
    int param_read(uint32_t param_id, param_value_t *value);

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
     * @return PARAM_OK 全部成功，或最后一个失败的错误码
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
     * ================================================================ */

    /** @brief 保存所有参数到持久化存储 (仅 PARAM_FLAG_PERSIST 标记的参数) */
    int param_save_all(void);
    /** @brief 保存单个参数到持久化存储 */
    int param_save_one(uint32_t param_id);

    /**
     * @brief 从持久化存储加载所有参数
     *
     * 两阶段执行:
     *   - 第一阶段: 遍历哈希表加载所有 entry 的缓存值
     *   - 第二阶段: 按 MODULE_INIT_ORDER 顺序调用各模块的 init 回调
     * @return PARAM_OK 成功，或第一个失败的错误码
     */
    int param_load_all(void);
    /** @brief 从持久化存储加载单个参数 */
    int param_load_one(uint32_t param_id);

    /**
     * @brief 删除单个参数的持久化数据
     * @param param_id 参数 ID
     * @return PARAM_OK 成功，PARAM_ERR_FLASH_FAIL 删除失败，PARAM_ERR_NOT_FOUND 存储不可用
     */
    int param_delete_one(uint32_t param_id);
    /**
     * @brief 擦除全部持久化数据
     * @return PARAM_OK 成功，PARAM_ERR_FLASH_FAIL 擦除失败，PARAM_ERR_NOT_FOUND 存储不可用
     */
    int param_delete_all(void);

    /* ================================================================
     *  重置
     * ================================================================ */

    /** @brief 重置所有参数为默认值 (default_val) */
    int param_reset_all(void);
    /** @brief 重置指定参数为默认值 */
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
     * @param module_id 指定模块 ID (0 表示遍历所有模块)
     * @param cb        回调函数
     * @param user_data 用户上下文
     */
    void param_foreach(uint16_t module_id, param_foreach_fn cb, void *user_data);

    /**
     * @brief 运行时设置参数的范围
     * @param param_id 参数 ID (仅 App 且 UINT/INT/FLOAT 类型)
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
     * 框架已从 FlashDB 读旧数据到 old_data[old_len]。
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
