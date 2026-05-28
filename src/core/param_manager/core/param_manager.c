/**
 * @file param_manager.c
 * @brief 参数管理核心框架实现
 *
 * 提供全局生命周期管理、哈希表操作、模块注册、读写分派、
 * flush/save/load/reset 等所有功能的运行时实现。
 *
 * 全局状态: 单例 g_pm 结构体，包含:
 *   - module_head / module_tail  模块链表 (按 MODULE_INIT_ORDER 排序)
 *   - hash[PARAM_HASH_SIZE]      参数哈希表 (开放寻址/线性探测)
 *   - storage                    持久化后端驱动
 *   - stats                      运行时统计信息
 */

#include "param_manager.h"
#include "param_manager_internal.h"
#include "param_manager_port.h"
#include "param_data_ops.h"
#include "module_ids.h"
#include <string.h>

/**
 * @brief 模块初始化顺序表 (编译期常量，来自 MODULE_INIT_ORDER)
 */
static const uint16_t g_module_order[] = {MODULE_INIT_ORDER};
#define MODULE_ORDER_COUNT (sizeof(g_module_order) / sizeof(g_module_order[0]))

/**
 * @brief 全局单例 — 参数管理器所有运行时状态
 */
static struct {
    param_module_node_t *module_head;     /**< 有序模块链表头 */
    param_module_node_t *module_tail;     /**< 有序模块链表尾 (用于尾插) */
    const param_storage_drv_t *storage;   /**< 持久化后端驱动 */
    param_entry_t *hash[PARAM_HASH_SIZE]; /**< 参数哈希表 */
    uint8_t initialized : 1;              /**< 初始化标志 */
    param_stats_t stats;                  /**< 统计信息 */
    param_notify_fn notify_cb;            /**< 参数变化通知回调 (可为 NULL) */
} g_pm;

/* ================================================================
 *  全局状态访问器
 * ================================================================ */

/** @brief 获取当前持久化后端驱动 (供子模块内部使用) */
const param_storage_drv_t *param_get_storage(void)
{
    return g_pm.storage;
}

/** @brief 运行时替换持久化后端驱动 */
void param_set_storage(const param_storage_drv_t *storage)
{
    if (!g_pm.initialized) {
        return;
    }
    if (storage && (!storage->load || !storage->save)) {
        return;
    }
    g_pm.storage = storage;
}

/* ================================================================
 *  统计计数器
 * ================================================================ */

/** @brief dirty 统计计数 +1 */
void param_stats_dirty_inc(void) { g_pm.stats.dirty_count++; }
/** @brief dirty 统计计数 -1 (防负数) */
void param_stats_dirty_dec(void)
{
    if (g_pm.stats.dirty_count > 0) {
        g_pm.stats.dirty_count--;
    }
}

/** @brief 模块注册计数 +1 */
void param_stats_module_inc(void) { g_pm.stats.module_count++; }
/** @brief 参数注册计数 +n */
void param_stats_params_add(uint16_t n) { g_pm.stats.param_count += n; }
/** @brief 若参数标记了 PERSIST 则 persist_count +1 */
void param_stats_persist_inc(param_entry_t *e)
{
    if (e && (entry_flags(e) & PARAM_FLAG_PERSIST))
        g_pm.stats.persist_count++;
}

/* ================================================================
 *  哈希表 — Thomas Wang 整数哈希 + 开放寻址线性探测
 * ================================================================ */

/**
 * @brief Thomas Wang 的 32-bit 整数哈希函数
 *
 * 用于将 param_id 均匀分散到哈希槽，消除简单取模带来的聚集问题。
 * 哈希表大小 PARAM_HASH_SIZE 必须为 2 的幂，以保证 & (N-1) 等价于 % N。
 *
 * @param param_id 参数 ID
 * @return 哈希槽索引 [0, PARAM_HASH_SIZE-1]
 */
static inline uint32_t hash_index(uint32_t param_id)
{
    uint32_t h = param_id;
    h = (h ^ 61) ^ (h >> 16);
    h = h + (h << 3);
    h = h ^ (h >> 4);
    h = h * 0x27d4eb2d;
    h = h ^ (h >> 15);
    return (uint32_t)(h & (PARAM_HASH_SIZE - 1));
}

/**
 * @brief 哈希表插入 — 开放寻址 + 线性探测
 *
 * 从 hash_index() 算出的初始槽开始，最坏遍历 PARAM_HASH_SIZE 次。
 * 若表满则返回 false。
 *
 * @param entry 要插入的参数条目
 * @return true 成功，false 表满
 */
static bool hash_insert(param_entry_t *entry)
{
    uint32_t slot = hash_index(entry->param_id);
    for (uint32_t i = 0; i < PARAM_HASH_SIZE; i++) {
        if (!g_pm.hash[slot]) {
            g_pm.hash[slot] = entry;
            return true;
        }
        slot = (slot + 1) & (PARAM_HASH_SIZE - 1);
    }
    return false;
}

/**
 * @brief 哈希表查找 — 开放寻址 + 线性探测
 *
 * 遇到空槽 (NULL) 即停止，意味着目标一定不存在。
 * 最坏 O(N)，平均 O(1)。
 *
 * @param param_id 参数 ID
 * @return 找到的条目指针，NULL 表示不存在
 */
static param_entry_t *hash_find(uint32_t param_id)
{
    uint32_t slot = hash_index(param_id);
    for (uint32_t i = 0; i < PARAM_HASH_SIZE; i++) {
        param_entry_t *e = g_pm.hash[slot];
        if (!e)
            return NULL;
        if (e->param_id == param_id)
            return e;
        slot = (slot + 1) & (PARAM_HASH_SIZE - 1);
    }
    return NULL;
}

/** @brief 防重复的哈希插入 — 若已存在则返回 true (幂等) */
bool param_hash_insert_entry(param_entry_t *entry)
{
    if (hash_find(entry->param_id))
        return true;
    return hash_insert(entry);
}

/* ================================================================
 *  模块链表 — 按 MODULE_INIT_ORDER 有序插入
 * ================================================================ */

param_module_node_t *param_module_find(uint16_t module_id)
{
    param_module_node_t *m = g_pm.module_head;
    while (m) {
        if (m->module_id == module_id)
            return m;
        m = m->next;
    }
    return NULL;
}

/** @brief 查找模块在 g_module_order 中的位置索引 (越靠前越小) */
static inline uint16_t get_module_order_index(uint16_t module_id)
{
    for (uint16_t i = 0; i < MODULE_ORDER_COUNT; i++) {
        if (g_module_order[i] == module_id)
            return i;
    }
    return MODULE_ORDER_COUNT; /* 未在 ORDER 中的模块排末尾 */
}

/**
 * @brief 将模块节点按 ORDER 顺序插入有序链表
 *
 * 遍历链表找到第一个 ORDER 索引大于当前模块的位置，插入其前。
 * 不在 ORDER 中的模块 (索引 == MODULE_ORDER_COUNT) 始终排在末尾。
 *
 * 三种情况:
 *   - 空链表: 直接作为 head+tail
 *   - 头插: ORDER < head 的 ORDER
 *   - 中插: 找到 prev->ORDER < new->ORDER < curr->ORDER 的位置
 *   - 尾插: 排最后
 *
 * @param node 要插入的模块节点
 */
void param_module_node_insert(param_module_node_t *node)
{
    uint16_t new_order = get_module_order_index(node->module_id);

    node->next = NULL;

    /* 空链表 */
    if (!g_pm.module_head) {
        g_pm.module_head = node;
        g_pm.module_tail = node;
        return;
    }

    /* 头插: 当前模块的 ORDER 比 head 还靠前 */
    if (new_order < get_module_order_index(g_pm.module_head->module_id)) {
        node->next = g_pm.module_head;
        g_pm.module_head = node;
        return;
    }

    /* 中插: 在 prev 和 curr 之间找到插入点 */
    param_module_node_t *prev = g_pm.module_head;
    param_module_node_t *curr = g_pm.module_head->next;

    while (curr) {
        uint16_t curr_order = get_module_order_index(curr->module_id);
        if (new_order < curr_order) {
            prev->next = node;
            node->next = curr;
            return;
        }
        prev = curr;
        curr = curr->next;
    }

    /* 尾插 */
    prev->next = node;
    g_pm.module_tail = node;
}

static param_entry_t *find_entry(uint32_t param_id)
{
    return hash_find(param_id);
}

param_entry_t *param_entry_find(uint32_t param_id)
{
    return find_entry(param_id);
}

/* ================================================================
 *  生命周期: init / deinit
 * ================================================================ */

/**
 * @brief 初始化参数管理框架
 *
 * 驱动由工厂函数预初始化后传入，此函数仅存储指针。
 *
 * @note 假定系统启动阶段单线程调用，因此不加锁。
 *       反初始化 param_deinit 会加锁以安全清理多线程环境中
 *       可能仍被持有的资源。两函数锁语义不对称是有意设计。
 *
 * @param storage 持久化后端驱动 (可为 NULL)
 * @param notify  参数变化通知回调 (可为 NULL)
 * @return PARAM_OK 成功，PARAM_ERR_BUSY 重复初始化
 */
int param_init(const param_storage_drv_t *storage, param_notify_fn notify)
{
    if (g_pm.initialized)
        return PARAM_ERR_BUSY;

    memset(&g_pm, 0, sizeof(g_pm));
    g_pm.storage = storage;
    g_pm.notify_cb = notify;

    g_pm.initialized = 1;
    return PARAM_OK;
}

/**
 * @brief 反初始化
 *
 * 逆序调用 storage->deinit() 和所有模块的 vtable->deinit()，
 * 最后 memset 清零全局状态。
 *
 * @note 加锁以安全清理多线程环境中可能仍被持有的资源。
 *       初始化 param_init 假定单线程调用不加锁，
 *       两函数锁语义不对称是有意设计。
 */
void param_deinit(void)
{
    LOCK();

    if (g_pm.storage && g_pm.storage->deinit)
        g_pm.storage->deinit(g_pm.storage->ctx);

    param_module_node_t *m = g_pm.module_head;
    while (m) {
        param_module_node_t *next = m->next;
        if (m->vtable && m->vtable->deinit)
            m->vtable->deinit(m);
        m->next = NULL;
        m = next;
    }

    memset(&g_pm, 0, sizeof(g_pm));
    UNLOCK();
}

/* ================================================================
 *  模块注册
 *
 *  param_module_register_node() 为通用注册入口 (App/IP 共用)。
 *  param_module_register() 为 App 便捷包装。
 *
 *  注册流程:
 *    1. 检查模块是否已注册 (module_id 重复)
 *    2. 检查所有参数 ID 是否冲突
 *    3. 参数插入哈希表
 *    4. 模块节点按 ORDER 插入有序链表
 *    5. 更新统计信息
 * ================================================================ */

/**
 * @brief 通用模块注册入口 — 将模块节点及参数表注册到框架
 *
 * @details
 * 注册流程分 5 步，全程持锁以保证哈希表 + 链表插入的原子性:
 *
 *   1. **去重检查**: 检查 module_id 是否已注册 → 返回 PARAM_ERR_ALREADY_REG
 *   2. **参数冲突检查**: 遍历所有参数条目，检查 param_id 是否已存在于哈希表
 *      → 返回 PARAM_ERR_ALREADY_REG
 *   3. **哈希插入**: 逐条将参数条目插入哈希表（开放寻址 + 线性探测）
 *      → 表满返回 PARAM_ERR_NO_MEMORY
 *   4. **有序链表插入**: 按 MODULE_INIT_ORDER 顺序将模块节点插入全局链表
 *   5. **统计更新**: 更新 module_count / param_count / persist_count
 *
 * App 和 IP 模块均通过此接口注册，消除双轨注册。
 * 注册前调用者必须先设置 node->vtable 和 node->module_id。
 *
 * @param node    模块基类节点指针（vtable + module_id 已设）
 * @param entries 参数条目指针数组
 * @param count   参数数量
 * @return PARAM_OK 成功，或错误码
 */
int param_module_register_node(param_module_node_t *node,
                               param_entry_t **entries,
                               uint16_t count)
{
    if (!g_pm.initialized)
        return PARAM_ERR_NOT_FOUND;
    if (!node || !entries || count == 0)
        return PARAM_ERR_INVALID_ID;

    LOCK();

    /* 步骤 1: 检查模块是否已注册 */
    if (param_module_find(node->module_id)) {
        UNLOCK();
        return PARAM_ERR_ALREADY_REG;
    }

    /* 步骤 2: 检查所有参数 ID 是否冲突 */
    for (uint16_t i = 0; i < count; i++) {
        if (!entries[i])
            continue;
        if (hash_find(entries[i]->param_id)) {
            UNLOCK();
            return PARAM_ERR_ALREADY_REG;
        }
    }

    node->table = entries;
    node->param_count = count;

    /* 步骤 3: 参数插入哈希表 */
    for (uint16_t i = 0; i < count; i++) {
        if (!entries[i])
            continue;
        if (!hash_insert(entries[i])) {
            UNLOCK();
            return PARAM_ERR_NO_MEMORY;
        }
    }

    /* 步骤 4: 模块节点按 ORDER 插入有序链表 */
    param_module_node_insert(node);

    /* 步骤 5: 更新统计信息 */
    param_stats_module_inc();
    param_stats_params_add(count);

    for (uint16_t i = 0; i < count; i++)
        param_stats_persist_inc(entries[i]);

    UNLOCK();
    return PARAM_OK;
}

int param_module_register(param_module_t *module,
                          param_entry_t **entries,
                          uint16_t count)
{
    return param_module_register_node(&module->node, entries, count);
}

/* ================================================================
 *  读写操作 — vtable 分派
 *
 *  所有 param_write / param_read 类的操作都遵循同一模式:
 *    1. 检查初始化状态
 *    2. 获取锁
 *    3. 通过哈希表找到 param_entry
 *    4. 调用 entry->vtable->xxx() 分派
 *    5. 根据返回值更新模块 dirty 标记
 *    6. 释放锁
 * ================================================================ */

/** @brief 写入参数 (缓存模式，不立即刷硬件，标记 dirty) */
int param_write(uint32_t param_id, param_value_t value)
{
    if (!g_pm.initialized)
        return PARAM_ERR_NOT_FOUND;

    param_entry_t *e = find_entry(param_id);
    if (!e)
        return PARAM_ERR_INVALID_ID;

    int ret = e->vtable->write(e, value);
    if (ret == PARAM_OK) {
        param_module_node_t *m = param_module_find(PARAM_MODULE_ID(param_id));
        if (m && m->vtable && m->vtable->mark_dirty) {
            LOCK();
            m->vtable->mark_dirty(m, PARAM_LOCAL_ID(param_id));
            UNLOCK();
        }
        if (g_pm.notify_cb)
            g_pm.notify_cb(param_id, value);
    }
    return ret;
}

/** @brief 写入参数缓存 (跳过 apply 回调，仅更新缓存 + 标记 dirty) */
int param_write_cache(uint32_t param_id, param_value_t value)
{
    if (!g_pm.initialized)
        return PARAM_ERR_NOT_FOUND;

    param_entry_t *e = find_entry(param_id);
    if (!e)
        return PARAM_ERR_INVALID_ID;

    int ret = e->vtable->write_cache(e, value);
    if (ret == PARAM_OK) {
        param_module_node_t *m = param_module_find(PARAM_MODULE_ID(param_id));
        if (m && m->vtable && m->vtable->mark_dirty) {
            LOCK();
            m->vtable->mark_dirty(m, PARAM_LOCAL_ID(param_id));
            UNLOCK();
        }
        if (g_pm.notify_cb)
            g_pm.notify_cb(param_id, value);
    }
    return ret;
}

/** @brief 立即写入参数 (直通硬件/回调，不产生 dirty) */
int param_write_immediate(uint32_t param_id, param_value_t value)
{
    if (!g_pm.initialized)
        return PARAM_ERR_NOT_FOUND;

    param_entry_t *e = find_entry(param_id);
    if (!e)
        return PARAM_ERR_INVALID_ID;

    int ret = e->vtable->write_immediate(e, value);
    if (ret == PARAM_OK) {
        param_module_node_t *m = param_module_find(PARAM_MODULE_ID(param_id));
        if (m && m->vtable && m->vtable->clear_dirty) {
            LOCK();
            m->vtable->clear_dirty(m, PARAM_LOCAL_ID(param_id));
            UNLOCK();
        }
        if (g_pm.notify_cb)
            g_pm.notify_cb(param_id, value);
    }
    return ret;
}

/** @brief 原始字节流写入 (EXEC 参数自动路由到 exec) */
int param_write_raw(uint32_t param_id, const uint8_t *data, uint16_t len)
{
    if (!g_pm.initialized)
        return PARAM_ERR_NOT_FOUND;
    if (!data || len == 0)
        return PARAM_ERR_INVALID_ID;

    param_entry_t *e = find_entry(param_id);
    if (!e)
        return PARAM_ERR_INVALID_ID;

    int ret = e->vtable->write_raw(e, data, len);
    if (ret == PARAM_OK) {
        param_module_node_t *m = param_module_find(PARAM_MODULE_ID(param_id));
        if (m && m->vtable && m->vtable->mark_dirty) {
            LOCK();
            m->vtable->mark_dirty(m, PARAM_LOCAL_ID(param_id));
            UNLOCK();
        }
        if (g_pm.notify_cb) {
            param_value_t change;
            /* 从 raw data 直接构造 notify 值，避免 param_read
               对 IP 参数触发硬件重读而返回非写入值 */
            memset(&change, 0, sizeof(change));
            if (len <= sizeof(param_value_t))
                memcpy(&change, data, len);
            else
                change.ptr = (void *)data;
            g_pm.notify_cb(param_id, change);
        }
    }
    return ret;
}

/** @brief 读取参数当前缓存值 */
int param_read(uint32_t param_id, param_value_t *value)
{
    if (!g_pm.initialized)
        return PARAM_ERR_NOT_FOUND;
    if (!value)
        return PARAM_ERR_INVALID_ID;

    param_entry_t *e = find_entry(param_id);
    if (!e)
        return PARAM_ERR_INVALID_ID;

    LOCK();
    int ret = e->vtable->read(e, value);
    UNLOCK();
    return ret;
}

/**
 * @brief 原始字节流读取参数
 *
 * @details
 * 根据参数类型分三种路径读取:
 *   - 值类型 (UINT/INT/FLOAT/BOOL/ENUM/EXEC): 从 entry_cache 直接拷贝
 *     sizeof(param_value_t) 字节 (平台相关: 32-bit 4B, 64-bit 8B)
 *   - BLOB: 从 entry_cache()->ptr 指向的外部缓冲区读取 blob_size 字节
 *   - STRING: 从 entry_cache()->ptr 指向的外部缓冲区读取 max_len+1 字节
 *
 * data==NULL 的查询模式用于先探测数据总大小再分配缓冲区。
 * 典型用法:
 * @code
 *   uint16_t len = 0;
 *   param_read_raw(id, NULL, &len);       // 查询大小
 *   uint8_t *buf = malloc(len);
 *   param_read_raw(id, buf, &len);        // 实际读取
 * @endcode
 *
 * @param param_id 参数 ID
 * @param data     用户缓冲区 (可为 NULL，此时仅查询长度)
 * @param len      [in/out] 输入为缓冲区容量, 输出为实际拷贝字节数
 * @return PARAM_OK 成功, PARAM_ERR_INVALID_ID 参数不存在,
 *         PARAM_ERR_TYPE_MISMATCH 未知类型, PARAM_ERR_NOT_FOUND 源指针为空
 */
int param_read_raw(uint32_t param_id, uint8_t *data, uint16_t *len)
{
    if (!g_pm.initialized)
        return PARAM_ERR_NOT_FOUND;
    if (!len)
        return PARAM_ERR_INVALID_ID;

    LOCK();
    param_entry_t *e = param_entry_find(param_id);
    if (!e) {
        UNLOCK();
        return PARAM_ERR_INVALID_ID;
    }

    param_type_t t = entry_type(e);
    uint16_t total;
    const uint8_t *src;

    switch (t) {
    case PARAM_TYPE_UINT:
    case PARAM_TYPE_INT:
    case PARAM_TYPE_FLOAT:
    case PARAM_TYPE_BOOL:
    case PARAM_TYPE_ENUM:
    case PARAM_TYPE_EXEC:
        total = sizeof(param_value_t);
        src = (const uint8_t *)entry_cache(e);
        break;
    case PARAM_TYPE_BLOB:
        total = ((param_blob_entry_t *)e)->blob_size;
        src = (const uint8_t *)entry_cache(e)->ptr;
        break;
    case PARAM_TYPE_STRING:
        total = ((param_string_entry_t *)e)->max_len + 1;
        src = (const uint8_t *)entry_cache(e)->ptr;
        break;
    default:
        UNLOCK();
        return PARAM_ERR_TYPE_MISMATCH;
    }

    if (!src) {
        UNLOCK();
        return PARAM_ERR_NOT_FOUND;
    }

    if (data) {
        uint16_t copy = (*len < total) ? *len : total;
        memcpy(data, src, copy);
        *len = copy;
    }
    else {
        *len = total;
    }

    UNLOCK();
    return PARAM_OK;
}

/* ---- 类型化读写包装函数 ---- */

/** @brief uint32_t 类型写入快捷函数 */
int param_write_u32(uint32_t id, uint32_t val)
{
    param_value_t v = {.u32 = val};
    return param_write(id, v);
}
/** @brief int32_t 类型写入快捷函数 */
int param_write_i32(uint32_t id, int32_t val)
{
    param_value_t v = {.i32 = val};
    return param_write(id, v);
}
/** @brief float 类型写入快捷函数 */
int param_write_f32(uint32_t id, float val)
{
    param_value_t v = {.f32 = val};
    return param_write(id, v);
}
/** @brief bool 类型写入快捷函数 */
int param_write_bool(uint32_t id, bool val)
{
    param_value_t v = {.b = val};
    return param_write(id, v);
}

/** @brief uint32_t 类型读取快捷函数 */
int param_read_u32(uint32_t id, uint32_t *val)
{
    param_value_t v;
    int ret = param_read(id, &v);
    if (ret == PARAM_OK && val)
        *val = v.u32;
    return ret;
}
/** @brief int32_t 类型读取快捷函数 */
int param_read_i32(uint32_t id, int32_t *val)
{
    param_value_t v;
    int ret = param_read(id, &v);
    if (ret == PARAM_OK && val)
        *val = v.i32;
    return ret;
}
/** @brief float 类型读取快捷函数 */
int param_read_f32(uint32_t id, float *val)
{
    param_value_t v;
    int ret = param_read(id, &v);
    if (ret == PARAM_OK && val)
        *val = v.f32;
    return ret;
}
/** @brief bool 类型读取快捷函数 */
int param_read_bool(uint32_t id, bool *val)
{
    param_value_t v;
    int ret = param_read(id, &v);
    if (ret == PARAM_OK && val)
        *val = v.b;
    return ret;
}

/**
 * @brief STRING 类型读取到调用者缓冲区
 *
 * 末尾始终补 '\0'。buf_size 不足时静默截断，不返回错误。
 */
int param_read_string(uint32_t id, char *buf, uint16_t buf_size)
{
    if (!g_pm.initialized)
        return PARAM_ERR_NOT_FOUND;
    if (!buf || buf_size == 0)
        return PARAM_ERR_INVALID_ID;

    param_entry_t *e = param_entry_find(id);
    if (!e)
        return PARAM_ERR_INVALID_ID;
    if (entry_type(e) != PARAM_TYPE_STRING)
        return PARAM_ERR_TYPE_MISMATCH;

    LOCK();
    const char *src = (const char *)entry_cache(e)->ptr;
    if (!src) {
        UNLOCK();
        return PARAM_ERR_NOT_FOUND;
    }

    uint16_t copy = buf_size - 1;
    strncpy(buf, src, copy);
    buf[copy] = '\0';
    UNLOCK();

    return PARAM_OK;
}

/**
 * @brief STRING 类型写入，委托到 param_write
 *
 * 先校验目标参数类型为 PARAM_TYPE_STRING (防止向非字符串参数
 * 写入指针值导致数据语义错误)，再构造 param_value_t 委托写入。
 * 底层 cache_update_string 自动截断并补 '\0'。
 */
int param_write_string(uint32_t id, const char *str)
{
    if (!g_pm.initialized)
        return PARAM_ERR_NOT_FOUND;
    if (!str)
        return PARAM_ERR_INVALID_ID;

    param_entry_t *e = param_entry_find(id);
    if (!e)
        return PARAM_ERR_INVALID_ID;
    if (entry_type(e) != PARAM_TYPE_STRING)
        return PARAM_ERR_TYPE_MISMATCH;

    param_value_t value = { .ptr = (void *)str };
    return param_write(id, value);
}

/**
 * @brief 获取参数数据大小 (字节数)
 *
 * 值类型 (UINT/INT/FLOAT/BOOL/ENUM/EXEC) → sizeof(param_value_t) (平台相关: 32-bit 4B, 64-bit 8B)
 * BLOB → blob_size
 * STRING → max_len + 1 (含结尾 '\0')
 * 未注册 → 0
 */
uint16_t param_get_size(uint32_t param_id)
{
    LOCK();
    param_entry_t *e = param_entry_find(param_id);
    if (!e) {
        UNLOCK();
        return 0;
    }

    uint16_t result;
    switch (entry_type(e)) {
    case PARAM_TYPE_UINT:
    case PARAM_TYPE_INT:
    case PARAM_TYPE_FLOAT:
    case PARAM_TYPE_BOOL:
    case PARAM_TYPE_ENUM:
    case PARAM_TYPE_EXEC:
        result = sizeof(param_value_t);
        break;
    case PARAM_TYPE_BLOB:
        result = ((param_blob_entry_t *)e)->blob_size;
        break;
    case PARAM_TYPE_STRING:
        result = ((param_string_entry_t *)e)->max_len + 1;
        break;
    default:
        result = 0;
        break;
    }
    UNLOCK();
    return result;
}

/* ================================================================
 *  exec / flush / save / load
 * ================================================================ */

/**
 * @brief 执行模块命令
 *
 * 仅当 cmd_id 已作为 PARAM_FLAG_EXEC 参数注册且模块存在 exec 时执行。
 * 未注册的命令返回 PARAM_ERR_NOT_FOUND。
 * user_arg 传入后封装为 param_value_t 联合体再传给回调。
 */
int param_exec(uint32_t cmd_id, void *user_arg)
{
    if (!g_pm.initialized)
        return PARAM_ERR_NOT_FOUND;
    LOCK();
    param_entry_t *e = param_entry_find(cmd_id);
    if (!param_entry_is_exec(e)) {
        UNLOCK();
        return PARAM_ERR_NOT_FOUND;
    }
    param_module_node_t *m = param_module_find(PARAM_MODULE_ID(cmd_id));
    if (!m || !m->vtable || !m->vtable->exec) {
        UNLOCK();
        return PARAM_ERR_NOT_FOUND;
    }
    UNLOCK();
    param_value_t arg = {.ptr = user_arg};
    return m->vtable->exec(m, cmd_id, arg);
}

/**
 * @brief 将所有模块的 dirty 参数刷入硬件
 *
 * 遍历模块链表 (按 MODULE_INIT_ORDER 顺序)，对每个 dirty 模块调用 module->vtable->flush。
 * 即使某个模块 flush 失败，仍会继续处理后续模块，错误计数累加到 flush_error_count。
 *
 * @return PARAM_OK 全部成功，或最后一个失败的错误码
 */
int param_flush(void)
{
    if (!g_pm.initialized)
        return PARAM_ERR_NOT_FOUND;

    int last_err = PARAM_OK;

    LOCK();

    param_module_node_t *m = g_pm.module_head;
    while (m) {
        if (m->dirty && m->vtable && m->vtable->flush) {
            int ret = m->vtable->flush(m);
            if (ret != PARAM_OK) {
                last_err = ret;
                if (g_pm.stats.flush_error_count < UINT16_MAX)
                    g_pm.stats.flush_error_count++;
            }
        }
        m = m->next;
    }

    UNLOCK();
    return last_err;
}

/** @brief 校验 module_id 是否在 MODULE_INIT_ORDER 中 */
static bool module_order_contains(const uint16_t *order, uint16_t count,
                                  uint16_t module_id)
{
    for (uint16_t i = 0; i < count; i++)
        if (order[i] == module_id)
            return true;
    return false;
}

/**
 * @brief 校验所有已注册模块是否都在 MODULE_INIT_ORDER 中
 *
 * 未覆盖的模块计入 flush_order_miss_count。
 * @return PARAM_OK 全部覆盖，PARAM_ERR_NOT_FOUND 存在未覆盖模块
 */
int param_check_flush_integrity(void)
{
    if (!g_pm.initialized)
        return PARAM_ERR_NOT_FOUND;

    g_pm.stats.flush_order_miss_count = 0;

    LOCK();

    param_module_node_t *m = g_pm.module_head;
    while (m) {
        if (!module_order_contains(g_module_order, MODULE_ORDER_COUNT,
                                   m->module_id))
        {
            g_pm.stats.flush_order_miss_count++;
        }
        m = m->next;
    }

    UNLOCK();

    if (g_pm.stats.flush_order_miss_count > 0)
        return PARAM_ERR_NOT_FOUND;

    return PARAM_OK;
}

/**
 * @brief 保存所有 PARAM_FLAG_PERSIST 参数到持久化存储
 *
 * 遍历哈希表，对每个 entry 调用 entry->vtable->save()。
 * 仅当 entry 的 flags 含 PARAM_FLAG_PERSIST 时才实际写入 (由子类实现判断)。
 *
 * 错误处理策略与 param_load_all 对称:
 *   - 遇错收集首个错误码，继续保存后续参数 (不提前退出)
 *   - 避免"第 N 个失败导致剩余参数全部丢失"的不一致状态
 *
 * @return PARAM_OK 全部成功，或第一个失败的错误码
 */
int param_save_all(void)
{
    if (!g_pm.initialized)
        return PARAM_ERR_NOT_FOUND;
    if (!g_pm.storage || !g_pm.storage->save)
        return PARAM_ERR_NOT_FOUND;
    int first_err = PARAM_OK;

    LOCK();
    for (uint16_t i = 0; i < PARAM_HASH_SIZE; i++) {
        param_entry_t *e = g_pm.hash[i];
        if (!e)
            continue;
        int ret = e->vtable->save(e);
        if (ret != 0 && first_err == PARAM_OK)
            first_err = (ret < 0) ? ret : PARAM_ERR_FLASH_FAIL;
    }
    UNLOCK();
    return first_err;
}

/** @brief 保存单个参数到持久化存储 */
int param_save_one(uint32_t param_id)
{
    if (!g_pm.initialized)
        return PARAM_ERR_NOT_FOUND;
    if (!g_pm.storage || !g_pm.storage->save)
        return PARAM_ERR_NOT_FOUND;
    LOCK();
    param_entry_t *e = find_entry(param_id);
    if (!e) {
        UNLOCK();
        return PARAM_ERR_INVALID_ID;
    }
    int ret = e->vtable->save(e);
    UNLOCK();
    return ret;
}

/* ================================================================
 *  存储后端分区管理
 * ================================================================ */

int param_storage_get_active_partition(uint8_t *index)
{
    if (!g_pm.storage || !g_pm.storage->get_active_partition)
        return PARAM_ERR_NOT_FOUND;
    return g_pm.storage->get_active_partition(g_pm.storage->ctx, index);
}

int param_storage_set_active_partition(uint8_t index)
{
    if (!g_pm.storage || !g_pm.storage->set_active_partition)
        return PARAM_ERR_NOT_FOUND;
    return g_pm.storage->set_active_partition(g_pm.storage->ctx, index);
}
/** @brief 按分区索引获取存储后端驱动实例 */
const param_storage_drv_t *param_get_storage_partition(uint8_t index)
{
    if (!g_pm.initialized || !g_pm.storage)
        return NULL;
    if (!g_pm.storage->get_partition)
        return NULL;
    return g_pm.storage->get_partition(g_pm.storage->ctx, index);
}

/* ================================================================
 *  参数版本迁移
 * ================================================================ */

/**
 * @brief 执行参数版本迁移（固件升级时自动转换旧参数格式）
 *
 * @details
 * 迁移流程分为三个阶段:
 *
 *   **阶段 1 — 版本检查:**
 *   从存储中读取 PID_SCHEMA_VER（1 字节版本号）:
 *     - 不存在 (ret≤0): 首次启动 → 写入当前版本号，跳过迁移
 *     - 不匹配: 存储格式破坏性变更 → 全量 erase_all + 重写版本号
 *     - 匹配: 进入阶段 2
 *
 *   **阶段 2 — 迁移执行:**
 *   遍历迁移表 (param_migrate_entry_t[]), 对每条旧 ID:
 *     1. 尝试加载旧数据 (不存在或已迁移 → 跳过)
 *     2. 根据 convert 回调决定迁移模式:
 *        - convert==NULL: 简单改名 — 以旧数据 + new_id 保存
 *        - convert!=NULL: 回调转换 — 回调填充 new_id + new_data + new_len
 *     3. 新值保存成功后删除旧键（保证原子性: 先写后删）
 *
 *   **阶段 3 — 错误处理:**
 *     - convert 返回 < -1: 致命错误，立即中断迁移
 *     - convert 返回 -1: 跳过本条（保留旧数据）
 *     - 单条保存失败: 不删除旧键，继续处理后续条目
 *
 * 设计意图: "先写新后删旧"保证 Flash 写入失败时不会丢失旧数据。
 * 即使迁移中断（如断电），下次启动可安全重试。
 *
 * @param storage 持久化后端驱动（必须支持 load/save）
 * @param table   迁移条目表（可为 NULL + count==0 跳过迁移）
 * @param count   迁移条目数
 * @return PARAM_OK 成功, PARAM_ERR_FLASH_FAIL Flash 操作失败,
 *         其他 <0 表示迁移回调返回的致命错误
 */
int param_migrate_storage(const param_storage_drv_t *storage,
                          const param_migrate_entry_t *table,
                          uint16_t count)
{
    if (!storage || !storage->load || !storage->save)
        return PARAM_ERR_NOT_FOUND;

    uint8_t buf[256];
    uint8_t schema_ver;
    int ret;

    /* ---- 阶段 1: 版本检查 ---- */
    ret = storage->load(storage->ctx, PID_SCHEMA_VER, &schema_ver, 1);
    if (ret <= 0) {
        /* 首次启动: 无版本键 → 写入当前版本, 跳过迁移 */
        schema_ver = PARAM_SCHEMA_VERSION;
        ret = storage->save(storage->ctx, PID_SCHEMA_VER, &schema_ver, 1);
        return (ret < 0) ? PARAM_ERR_FLASH_FAIL : PARAM_OK;
    }

    if (schema_ver != PARAM_SCHEMA_VERSION) {
        /* 存储格式破坏性变更 → 全量擦除, 写版本号, 跳过迁移 */
        if (storage->erase_all)
            storage->erase_all(storage->ctx);
        schema_ver = PARAM_SCHEMA_VERSION;
        ret = storage->save(storage->ctx, PID_SCHEMA_VER, &schema_ver, 1);
        return (ret < 0) ? PARAM_ERR_FLASH_FAIL : PARAM_OK;
    }

    /* ---- 阶段 2: 版本匹配 → 执行迁移表 ---- */
    if (!table || count == 0)
        return PARAM_OK;

    for (uint16_t i = 0; i < count; i++) {
        const param_migrate_entry_t *e = &table[i];

        /* 尝试加载旧数据 */
        ret = storage->load(storage->ctx, e->old_id, buf, sizeof(buf));
        if (ret <= 0)
            continue;  /**< 不存在或已迁移 → 跳过 */

        uint16_t old_len = (uint16_t)ret;
        bool saved = false;

        if (e->convert) {
            /* 转换模式: 回调负责填充 new_id + new_data + new_len */
            uint32_t new_id;
            uint16_t new_len = 0;
            ret = e->convert(buf, old_len, &new_id, buf, &new_len, e->ctx);
            if (ret < 0 && ret != -1)
                return ret;  /**< 致命错误 → 中断迁移 */
            if (ret == PARAM_OK && new_len > 0) {
                ret = storage->save(storage->ctx, new_id, buf, new_len);
                saved = (ret >= 0);
            }
        } else {
            /* 简单改名: 数据不变，仅变更 ID */
            ret = storage->save(storage->ctx, e->new_id, buf, old_len);
            saved = (ret >= 0);
        }

        /* 先写后删: 仅在新值保存成功后才删除旧键，
           避免 Flash 写入失败导致数据丢失 */
        if (saved && storage->delete)
            storage->delete(storage->ctx, e->old_id);
    }

    return PARAM_OK;
}

/**
 * @brief 从持久化存储加载所有参数
 *
 * 两阶段执行:
 *   1. LOAD 阶段: 遍历哈希表，对所有 entry 调用 entry->vtable->load()
 *      将存储的值恢复到 entry->cache。仅收集第一个错误码。
 *      (即使某个 entry 加载失败，仍继续加载后续 entry)
 *   2. INIT 阶段: 按链表顺序遍历模块 (即 MODULE_INIT_ORDER)，
 *      对每个模块调用 vtable->init()。通常用于将恢复的缓存值下发到硬件。
 *
 * 两阶段分离的意义: 确保 init 回调中通过 param_read() 能读到正确的缓存值。
 */
int param_load_all(void)
{
    if (!g_pm.initialized)
        return PARAM_ERR_NOT_FOUND;
    if (!g_pm.storage || !g_pm.storage->load)
        return PARAM_ERR_NOT_FOUND;
    int first_err = PARAM_OK;

    LOCK();

    /* 第一阶段: 遍历哈希表，恢复所有 entry 的缓存值 */
    for (uint16_t i = 0; i < PARAM_HASH_SIZE; i++) {
        param_entry_t *e = g_pm.hash[i];
        if (!e)
            continue;
        int ret = e->vtable->load(e);
        if (ret != 0 && first_err == PARAM_OK)
            first_err = (ret < 0) ? ret : PARAM_ERR_FLASH_FAIL;
    }

    UNLOCK();

    /* 第二阶段: 按 MODULE_INIT_ORDER 顺序初始化每个模块 */
    param_module_node_t *m = g_pm.module_head;
    while (m) {
        if (m->vtable && m->vtable->init)
            m->vtable->init(m);
        m = m->next;
    }

    return first_err;
}

/**
 * @brief 从持久化存储加载单个参数
 *
 * 加载后调用 vtable->write 使业务逻辑 (apply 回调) 对恢复的值
 * 重新校验，确保多分区切换后参数满足当前约束。
 *
 * @note 此路径直接调用 vtable->write 而非 param_write，
 *       因此不触发 notify 回调。加载/分区切换属于批量恢复操作，
 *       逐参数 notify 会产生噪音，由调用者决定是否在批量加载后
 *       统一通知。
 */
int param_load_one(uint32_t param_id)
{
    if (!g_pm.initialized)
        return PARAM_ERR_NOT_FOUND;
    if (!g_pm.storage || !g_pm.storage->load)
        return PARAM_ERR_NOT_FOUND;

    param_entry_t *e = find_entry(param_id);
    if (!e)
        return PARAM_ERR_INVALID_ID;

    int ret;
    LOCK();
    ret = e->vtable->load(e);
    UNLOCK();
    if (ret == PARAM_OK) {
        param_module_node_t *m = param_module_find(PARAM_MODULE_ID(param_id));
        if (m && m->vtable && m->vtable->mark_dirty)
            m->vtable->mark_dirty(m, PARAM_LOCAL_ID(param_id));
    }
    /* apply 重新校验恢复值 (多分区兼容); 有意不走 param_write 以避免 notify */
    if (ret == PARAM_OK)
        ret = e->vtable->write(e, *entry_cache(e));

    return ret;
}

/** @brief 删除单个参数的持久化数据 */
int param_delete_one(uint32_t param_id)
{
    if (!g_pm.initialized)
        return PARAM_ERR_NOT_FOUND;
    if (!g_pm.storage || !g_pm.storage->delete)
        return PARAM_ERR_NOT_FOUND;
    return g_pm.storage->delete(g_pm.storage->ctx, param_id);
}

/** @brief 擦除全部持久化数据 */
int param_delete_all(void)
{
    if (!g_pm.initialized)
        return PARAM_ERR_NOT_FOUND;
    if (!g_pm.storage || !g_pm.storage->erase_all)
        return PARAM_ERR_NOT_FOUND;
    return g_pm.storage->erase_all(g_pm.storage->ctx);
}

/* ================================================================
 *  reset / stats / foreach / range / validate
 * ================================================================ */

/**
 * @brief 重置所有参数为默认值
 *
 * 两阶段:
 *   1. RESET 阶段: 遍历哈希表重置所有 entry 为默认值
 *   2. INIT 阶段: 遍历模块链表调用 vtable->init()，将默认值下发到硬件
 *
 * 与 param_load_all 对称: 数据来源不同 (默认值 vs Flash)，
 * 但缓存 → 硬件生效的路径一致。
 *
 * @return PARAM_OK
 */
int param_reset_all(void)
{
    if (!g_pm.initialized)
        return PARAM_ERR_NOT_FOUND;

    LOCK();

    for (uint16_t i = 0; i < PARAM_HASH_SIZE; i++) {
        param_entry_t *e = g_pm.hash[i];
        if (!e)
            continue;
        e->vtable->reset(e);
    }

    UNLOCK();

    param_module_node_t *m = g_pm.module_head;
    while (m) {
        if (m->vtable && m->vtable->init)
            m->vtable->init(m);
        m = m->next;
    }

    return PARAM_OK;
}

/**
 * @brief 重置指定参数为默认值，并标记 dirty 以便刷入硬件
 *
 * 流程: reset 恢复默认值 → mark_dirty 标记模块 → write 触发 apply 校验。
 *
 * 末尾的 vtable->write 有两个作用:
 *   1. 使 App 模块的 apply 回调对默认值重新校验 (防止默认值与
 *      当前业务约束冲突)
 *   2. 标记 entry 级 dirty (reset 已清除 dirty, write 重新置位)
 *
 * @note 与 param_load_one 相同, 直接调用 vtable->write 而非
 *       param_write, 不触发 notify。重置通常是批量操作的一部分，
 *       由调用者统一处理通知。
 */
int param_reset_one(uint32_t param_id)
{
    if (!g_pm.initialized)
        return PARAM_ERR_NOT_FOUND;

    param_entry_t *e;
    int ret;

    LOCK();
    e = find_entry(param_id);
    if (!e) {
        UNLOCK();
        return PARAM_ERR_INVALID_ID;
    }
    ret = e->vtable->reset(e);
    if (ret == PARAM_OK) {
        param_module_node_t *m = param_module_find(PARAM_MODULE_ID(param_id));
        if (m && m->vtable && m->vtable->mark_dirty)
            m->vtable->mark_dirty(m, PARAM_LOCAL_ID(param_id));
    }
    UNLOCK();

    /* apply 重新校验默认值 + 恢复 entry 级 dirty; 有意不走 param_write 以避免 notify */
    if (ret == PARAM_OK)
        ret = e->vtable->write(e, *entry_cache(e));

    return ret;
}

/** @brief 获取全局统计信息快照 */
void param_get_stats(param_stats_t *stats)
{
    if (!g_pm.initialized)
        return;
    if (stats) {
        LOCK();
        *stats = g_pm.stats;
        UNLOCK();
    }
}

/** @brief 清零统计计数器 (dirty/error/miss) */
void param_clear_stats(void)
{
    if (!g_pm.initialized)
        return;
    LOCK();
    g_pm.stats.dirty_count = 0;
    g_pm.stats.flush_error_count = 0;
    g_pm.stats.flush_order_miss_count = 0;
    UNLOCK();
}

/**
 * @brief 遍历指定模块或全部模块的参数条目
 *
 * @param module_id 模块 ID (0 表示全部)
 * @param cb        回调，返回 false 终止遍历
 * @param user_data 用户上下文
 */
void param_foreach(uint16_t module_id, param_foreach_fn cb, void *user_data)
{
    if (!g_pm.initialized || !cb)
        return;
    LOCK();

    param_module_node_t *m = g_pm.module_head;
    while (m) {
        if (module_id == 0 || m->module_id == module_id) {
            for (uint16_t i = 0; i < m->param_count; i++) {
                param_entry_t *e = m->table[i];
                if (!e)
                    continue;
                if (!cb(e, user_data)) {
                    UNLOCK();
                    return;
                }
            }
        }
        m = m->next;
    }
    UNLOCK();
}

/**
 * @brief 运行时设置参数的范围
 *
 * @param param_id 参数 ID
 * @param min_val  新最小值 (NULL 表示不改)
 * @param max_val  新最大值 (NULL 表示不改)
 * @return PARAM_OK 成功，或错误码
 */
int param_set_range(uint32_t param_id,
                    const param_value_t *min_val,
                    const param_value_t *max_val)
{
    if (!g_pm.initialized)
        return PARAM_ERR_NOT_FOUND;
    LOCK();
    param_entry_t *e = find_entry(param_id);
    if (!e) {
        UNLOCK();
        return PARAM_ERR_INVALID_ID;
    }

    param_type_t t = entry_type(e);
    if (t != PARAM_TYPE_UINT && t != PARAM_TYPE_INT && t != PARAM_TYPE_FLOAT) {
        UNLOCK();
        return PARAM_ERR_TYPE_MISMATCH;
    }

    param_range_entry_t *re = (param_range_entry_t *)e;
    if (min_val)
        re->min = *min_val;
    if (max_val)
        re->max = *max_val;
    if (min_val || max_val)
        re->has_range = 1;

    /* 保存旧值 → 裁剪 → 捕获新值 (持锁): 若范围缩小导致缓存越界,
       则走 param_write 完整路径 (apply + dirty + notify),
       确保硬件最终也被更新。新值在锁内捕获，避免 UNLOCK 后重读。 */
    param_value_t old_val = *entry_cache(e);
    param_clamp_entry(e);
    param_value_t new_val = *entry_cache(e);
    bool clamped = memcmp(&old_val, &new_val, sizeof(param_value_t)) != 0;

    UNLOCK();

    if (clamped)
        return param_write(param_id, new_val);

    return PARAM_OK;
}

/** @brief 对所有已注册参数执行范围裁剪 */
void param_validate_all(void)
{
    if (!g_pm.initialized)
        return;
    LOCK();
    param_module_node_t *m = g_pm.module_head;
    while (m) {
        for (uint16_t i = 0; i < m->param_count; i++) {
            param_entry_t *e = m->table[i];
            if (e)
                param_clamp_entry(e);
        }
        m = m->next;
    }
    UNLOCK();
}

/** @brief 遍历所有已注册模块节点 */
void param_module_foreach(param_module_iter_fn cb, void *ctx)
{
    if (!g_pm.initialized || !cb)
        return;
    LOCK();
    param_module_node_t *m = g_pm.module_head;
    while (m) {
        cb(m, ctx);
        m = m->next;
    }
    UNLOCK();
}

#if PARAM_MODULE_AUTO_REGISTER

/**
 * @brief 编译器段自动注册 —
 *        遍历链接器脚本定义的 .rodata.param_modules 段边界，
 *        依次调用每个模块的 init 函数
 *
 * param_modules_start[] / param_modules_end[] 由链接器脚本提供，
 * 指向同一 rodata 段的起止地址。
 */
extern const param_module_reg_t param_modules_start[];
extern const param_module_reg_t param_modules_end[];

int param_modules_register_all(void)
{
    const param_module_reg_t *p;
    for (p = param_modules_start; p < param_modules_end; p++) {
        if (p->init)
            p->init();
    }
    return PARAM_OK;
}

#endif
