#ifndef PARAM_MANAGER_INTERNAL_H
#define PARAM_MANAGER_INTERNAL_H

#include "param_manager.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @file param_manager_internal.h
     * @brief 框架内部 API — 仅供 param_manager 子系统内部使用
     *
     * 这些函数不被应用层代码直接调用，仅由 app_param_manager.c /
     * ip_param_manager.c / param_data_ops.c / param_dump.c 等
     * 内部模块使用，不应出现在 param_manager.h 的公共 API 中。
     *
     * ================================================================
     *  锁策略 (Lock Discipline)
     * ================================================================
     *
     * 全局互斥锁通过 LOCK() / UNLOCK() 宏操作，底层由 param_manager_port
     * 提供 (RTOS 互斥锁 / 裸机关中断 / 无操作)。
     *
     * 原则:
     *   1. 所有对 g_pm 全局状态的读写 (哈希表、模块链表、统计计数)
     *      必须在 LOCK/UNLOCK 对内完成。
     *   2. 参数级 vtable 函数 (app_write / ip_param_write 等) 内部自行
     *      加锁保护缓存更新和 dirty 标记 —— 调用者进入 vtable 前不持锁。
     *   3. 模块级 dirty 标记 (mark_dirty / clear_dirty) 由公共 API 层
     *      (param_write 系列) 在 vtable 返回后单独加锁调用。
     *      这意味着 vtable->write 返回 与 mark_dirty 之间存在窗口:
     *      多线程并发写入同一参数时，notify 可能收到中间值。
     *      notify 回调的实现者应将收到的值视为 best-effort 快照。
     *   4. apply / exec / notify 回调在锁外执行，以避免死锁
     *      (回调中可能递归调用 param_write)。回调实现者需自行处理
     *      防重入。
     *   5. 初始化 (param_init) 和反初始化 (param_deinit) 假定单线程
     *      调用；init 不加锁，deinit 加锁以安全清理。
     *   6. 模块注册 (param_module_register_node) 全程持锁以保证
     *      哈希表 + 链表插入的原子性。
     */

    /** @brief 获取当前持久化后端驱动 */
    const param_storage_drv_t *param_get_storage(void);

    /** @brief 通过 module_id 查找模块节点 (遍历链表) */
    param_module_node_t *param_module_find(uint16_t module_id);

    /** @brief 通过 param_id 查找参数条目 (哈希查找) */
    param_entry_t *param_entry_find(uint32_t param_id);

    /** @brief dirty 统计计数 +1 */
    void param_stats_dirty_inc(void);
    /** @brief dirty 统计计数 -1 (防负数) */
    void param_stats_dirty_dec(void);
    /** @brief 模块注册计数 +1 */
    void param_stats_module_inc(void);
    /** @brief 参数注册计数 +n */
    void param_stats_params_add(uint16_t n);
    /** @brief 若参数标记了 PERSIST 则 persist_count +1 */
    void param_stats_persist_inc(param_entry_t *e);

    /** @brief 哈希插入 (幂等: 已存在则跳过) */
    bool param_hash_insert_entry(param_entry_t *entry);

    /** @brief 按 MODULE_INIT_ORDER 有序插入模块节点到链表 */
    void param_module_node_insert(param_module_node_t *node);

    /** 模块遍历回调 */
    typedef void (*param_module_iter_fn)(param_module_node_t *m, void *ctx);

    /** @brief 遍历所有已注册模块节点 */
    void param_module_foreach(param_module_iter_fn cb, void *ctx);

    /**
     * @brief 通用模块注册接口 — 将模块节点及参数表注册到框架
     *
     * App 和 IP 模块均通过此接口注册，消除双轨注册。
     * 注册前调用者必须先设置 node->vtable 和 node->module_id。
     *
     * @param node    模块基类节点指针
     * @param entries 参数条目指针数组
     * @param count   参数数量
     * @return PARAM_OK 成功，或错误码
     */
    int param_module_register_node(param_module_node_t *node,
                                   param_entry_t **entries,
                                   uint16_t count);

    /** @brief 注册 App 模块及其参数表 (便捷包装, 内部调用 param_module_register_node) */
    int param_module_register(param_module_t *module,
                              param_entry_t **entries,
                              uint16_t count);

    /* ================================================================
     *  内联访问器 — 统一参数条目字段访问 (仅框架内部使用)
     *
     *  通过 param_entry_head_t 强转实现编译期偏移量计算，
     *  对 App 和 IP 参数均适用。
     * ================================================================ */

    /**
     * @brief 公共头部具名结构体 — 提供编译期偏移量计算
     *
     * 通过 param_manager.h 中的 PARAM_ENTRY_HEAD() 宏生成，
     * 与该宏定义的 6 个公共派生结构体布局同源。
     */
    typedef struct
    {
        PARAM_ENTRY_HEAD();
    } param_entry_head_t;

    /** @brief 判断参数是否为 App 类型 */
    static inline bool is_app(const param_entry_t *e)
    {
        return e && e->vtable == &app_vtable;
    }

    /** @brief 判断参数是否为 IP 类型 */
    static inline bool is_ip(const param_entry_t *e)
    {
        return e && e->vtable == &ip_vtable;
    }

    /**
     * @brief 判断参数是否为合法的 EXEC 命令参数
     *
     * 同时满足 PARAM_FLAG_EXEC 标志和 PARAM_TYPE_EXEC 类型才返回 true。
     * 若 FLAG_EXEC 置位但类型不是 EXEC (配置错误)，返回 false。
     *
     * 所有 FLAG_EXEC 相关判断统一使用此函数，消除分散的 flag+type 双重检查。
     */
    static inline bool param_entry_is_exec(const param_entry_t *e)
    {
        return e && (entry_flags(e) & PARAM_FLAG_EXEC) && entry_type(e) == PARAM_TYPE_EXEC;
    }

    /** @brief 获取参数类型 */
    static inline param_type_t entry_type(const param_entry_t *e)
    {
        return ((const param_entry_head_t *)e)->type;
    }

    /** @brief 获取参数属性标志 */
    static inline uint16_t entry_flags(const param_entry_t *e)
    {
        return ((const param_entry_head_t *)e)->flags;
    }

    /** @brief 获取参数 dirty 标志 */
    static inline uint8_t entry_dirty(const param_entry_t *e)
    {
        return ((const param_entry_head_t *)e)->dirty;
    }

    /** @brief 设置参数 dirty 标志 */
    static inline void entry_set_dirty(param_entry_t *e, uint8_t v)
    {
        ((param_entry_head_t *)e)->dirty = v;
    }

    /**
     * @brief 清除参数 dirty 标志并同步全局统计
     */
    static inline void param_entry_clear_dirty(param_entry_t *e)
    {
        if (entry_dirty(e))
        {
            entry_set_dirty(e, 0);
            param_stats_dirty_dec();
        }
    }

    /**
     * @brief 标记参数 dirty 并同步全局统计
     */
    static inline void param_entry_mark_dirty(param_entry_t *e)
    {
        if (!entry_dirty(e))
        {
            param_stats_dirty_inc();
            entry_set_dirty(e, 1);
        }
    }

    /** @brief 获取缓存值指针（可写） */
    static inline param_value_t *entry_cache_ptr(param_entry_t *e)
    {
        return &((param_entry_head_t *)e)->cache;
    }

    /** @brief 获取缓存值（只读） */
    static inline const param_value_t *entry_cache(const param_entry_t *e)
    {
        return &((const param_entry_head_t *)e)->cache;
    }

    /** @brief 获取默认值（只读） */
    static inline const param_value_t *entry_default(const param_entry_t *e)
    {
        return &((const param_entry_head_t *)e)->default_val;
    }

#ifdef PARAM_DEBUG_NAME
    /** @brief 获取参数调试名称 */
    static inline const char *param_entry_name(const param_entry_t *e)
    {
        return ((const param_entry_head_t *)e)->name;
    }
#else
#define param_entry_name(e) "?"
#endif

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
    _Static_assert(offsetof(param_entry_head_t, base) == 0,
                   "param_entry_head_t.base must be first");
    _Static_assert(offsetof(param_entry_head_t, type) == sizeof(param_entry_t),
                   "param_entry_head_t.type offset mismatch");
#endif

#ifdef __cplusplus
}
#endif

#endif /* PARAM_MANAGER_INTERNAL_H */
