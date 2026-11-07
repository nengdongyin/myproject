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
#include "port.h"
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
    uint8_t flushing : 1;                 /**< flush 进行中 (用户回调锁外执行期间为 1) */
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

/**
 * @brief 运行时替换持久化后端驱动 — 纯指针替换
 *
 * @details
 * 仅替换 g_pm.storage 指针，不触发旧驱动的 deinit，不触发 save/load。
 * 这意味着旧驱动在替换后仍然有效，调用者可以保存旧指针并在之后切回。
 *
 * 典型用法 (临时切分区):
 * @code
 *   const param_storage_drv_t *saved = param_get_storage();
 *   param_set_storage(param_get_storage_partition(1));
 *   param_load_all();           // 从分区 1 加载
 *   // ... 操作 ...
 *   param_set_storage(saved);   // 切回原分区
 * @endcode
 *
 * @param storage 新的存储后端驱动 (可为 NULL 停用持久化)
 * @note 若传入非 NULL 但缺少 load/save 回调的驱动，调用被静默忽略
 */
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

/** @brief 哈希表遍历回调 */
typedef void (*param_entry_visit_fn)(param_entry_t *e, void *ctx);

/**
 * @brief 遍历哈希表全部条目 (调用者持锁, 无 I/O 场景专用)
 *
 * 逐槽扫描 PARAM_HASH_SIZE 个槽位, 跳过空槽。仅供 param_reset_all 使用:
 * 它是纯内存操作, 一整轮放在同一个临界区里比逐条进出锁更划算。
 *
 * @note 本函数**不**自行加锁 —— 调用者须已持 SYS_LOCK_PARAM。
 * @note 需要**锁外执行介质 I/O** 的调用方 (param_save_all /
 *       param_load_all) 请用 param_hash_scan, 不要用本函数。
 * @param fn  对每个非空条目调用一次
 * @param ctx 透传给 fn 的调用方上下文
 */
static void param_hash_foreach(param_entry_visit_fn fn, void *ctx)
{
    for (uint16_t i = 0; i < PARAM_HASH_SIZE; i++) {
        param_entry_t *e = g_pm.hash[i];
        if (e)
            fn(e, ctx);
    }
}

/** @brief 哈希分批遍历的批容量 (栈上指针数组, 与 PARAM_MODULE_BATCH_MAX 同量级) */
#define PARAM_HASH_BATCH_MAX 8

/**
 * @brief 分批遍历哈希表 — 锁内只快照指针, 锁外执行回调
 *
 * 持久化入口 (param_save_all / param_load_all 的兜底路径) 专用: 把介质
 * 访问放到锁外, 避免"整轮持久化期间一直持锁"。
 *
 * 锁外使用快照指针之所以安全: 条目只在注册期插入、运行期不摘除 (静态
 * 对象), 指针生命周期与注册表一致; 其缓存内容为 best-effort 快照。
 *
 * @note 回调在锁外执行, 可安全调用加锁 API。
 * @param fn  对每个非空条目调用一次 (锁外)
 * @param ctx 透传给 fn 的调用方上下文
 */
static void param_hash_scan(param_entry_visit_fn fn, void *ctx)
{
    uint16_t slot = 0;

    while (slot < PARAM_HASH_SIZE) {
        param_entry_t *batch[PARAM_HASH_BATCH_MAX];
        uint16_t n = 0;

        /* ---- 阶段 1 (锁内): 只收集指针, 不执行回调 ---- */
        system_lock(SYS_LOCK_PARAM);
        while (slot < PARAM_HASH_SIZE && n < PARAM_HASH_BATCH_MAX) {
            param_entry_t *e = g_pm.hash[slot++];
            if (e)
                batch[n++] = e;
        }
        system_unlock(SYS_LOCK_PARAM);

        /* ---- 阶段 2 (锁外): 执行调用方回调 ---- */
        for (uint16_t i = 0; i < n; i++)
            fn(batch[i], ctx);
    }
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

/**
 * @brief 通过 param_id 查找参数条目 (哈希 + 线性探测, 见 hash_find)
 *
 * 框架内各路径与 param_data_ops.c 的批量恢复均经此函数查找条目 ——
 * 只此一层, 不再包转发函数。
 *
 * @note 调用方若需哈希表一致性保证, 自行在外层持 SYS_LOCK_PARAM。
 * @param param_id 参数 ID
 * @return 条目指针, NULL = 未注册
 */
param_entry_t *param_entry_find(uint32_t param_id)
{
    return hash_find(param_id);
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
 *       可能仍被持有的资源。
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
 *       初始化 param_init 假定单线程调用不加锁。
 */
void param_deinit(void)
{
    system_lock(SYS_LOCK_PARAM);

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
    system_unlock(SYS_LOCK_PARAM);
}

/** @brief 查询参数管理框架是否已初始化 */
bool param_is_initialized(void)
{
    return g_pm.initialized != 0;
}

/** @brief 查询指定模块是否已完成初始化 (vtable->init 成功后置位) */
bool param_module_is_initialized(uint16_t module_id)
{
    if (!g_pm.initialized)
        return false;
    param_module_node_t *m = param_module_find(module_id);
    return m && m->initialized;
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
 * @note 模块应在 param_load_all / param_reset_all 之前注册。
 *       晚注册的模块不会自动初始化 (initialized 保持 0),
 *       需再次调用 param_load_all / param_reset_all 触发 vtable->init。
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
        return PARAM_ERR_NOT_INITIALIZED;
    if (!node || !entries || count == 0)
        return PARAM_ERR_INVALID_ID;

    system_lock(SYS_LOCK_PARAM);

    /* 步骤 1: 检查模块是否已注册 */
    if (param_module_find(node->module_id)) {
        system_unlock(SYS_LOCK_PARAM);
        return PARAM_ERR_ALREADY_REG;
    }

    /* 步骤 2: 检查所有参数 ID 是否冲突 */
    for (uint16_t i = 0; i < count; i++) {
        if (!entries[i])
            continue;
        if (hash_find(entries[i]->param_id)) {
            system_unlock(SYS_LOCK_PARAM);
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
            system_unlock(SYS_LOCK_PARAM);
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

    system_unlock(SYS_LOCK_PARAM);
    return PARAM_OK;
}

int param_module_register(param_module_t *module,
                          param_entry_t **entries,
                          uint16_t count)
{
    return param_module_register_node(&module->node, entries, count);
}

/* ================================================================
 *  vtable 基类实现 — App/IP 共用的两级虚函数默认实现
 *
 *  App 与 IP 的唯一结构性差异收敛为:
 *   - 参数级: write 是否携带 apply 回调 (write_apply vs write)
 *   - 模块级: flush / init 两条通路 (保留在 app/ip 各自实现文件)
 *  其余槽位 (read / write / write_immediate / write_raw / load /
 *  reset / mark_dirty / clear_dirty / exec / reset / deinit) 均为
 *  共用实现, 在此定义一次, 由两侧 vtable 常量引用。
 * ================================================================ */

/**
 * @brief 判断参数条目是否可写 (READONLY/EXEC 条目永不进入写路径)
 *
 * App/IP 通用语义: READONLY (硬件状态/计数器) 与 EXEC (命令) 条目
 * 只走读/命令路径, 永不进入写路径。init 标脏、load 标脏、flush
 * 循环与模块级 mark_dirty 均以此过滤。
 */
bool param_entry_is_writable(const param_entry_t *e)
{
    return !(entry_flags(e) & (PARAM_FLAG_READONLY | PARAM_FLAG_EXEC));
}

/**
 * @brief 按模块 ID 查找模块节点 (带 vtable 类型校验)
 *
 * 调用方传入期望的模块级 vtable, 防御"参数条目与模块注册类型错配"
 * 的配置错误。App/IP 各自以本函数 + 自己的 vtable 完成带类型校验的查找。
 */
param_module_t *param_module_find_typed(uint16_t module_id,
                                        const param_module_vtable_t *vt)
{
    param_module_node_t *node = param_module_find(module_id);
    if (node && node->vtable == vt)
        return (param_module_t *)node;
    return NULL;
}

/**
 * @brief 按参数条目查找所属模块 — 期望模块类型来自条目 vtable 自带的配对字段
 *
 * 依赖方向 (派生 → 基类, 无反向): 各子系统在组装自己的 param_vtable_t
 * 实例时填入 module_vtable, 基类实现仅跟随该指针完成"条目↔模块"类型
 * 一致性校验, 不命名任何派生侧的 vtable 符号。
 */
static param_module_t *param_entry_module(const param_entry_t *e)
{
    return param_module_find_typed((uint16_t)PARAM_MODULE_ID(e->param_id),
                                   e->vtable->module_vtable);
}

/* ---- 参数级基类实现 ---- */

int param_entry_read(param_entry_t *e, param_value_t *value)
{
    param_module_t *m = param_entry_module(e);

    /* 阶段 1: 优先通过 read 回调获取实时值。成功则回写缓存并返回。
       (App: 业务实时计算/硬件查询; IP: driver 硬件寄存器读取) */
    if (m && m->read) {
        /* 防御性清零: 回调未处理该 id 却返回 PARAM_OK 时,
           输出参数不会携带栈上随机值污染 cache */
        memset(value, 0, sizeof(*value));
        int ret = m->read(m->ctx, e->param_id, value);
        if (ret == PARAM_OK) {
            param_type_t t = entry_type(e);
            if (t < PARAM_TYPE_COUNT) {
                system_lock(SYS_LOCK_PARAM);
                g_param_data_ops[t].cache_update(e, *value);
                system_unlock(SYS_LOCK_PARAM);
            }
            return PARAM_OK;
        }
    }

    /* 阶段 2: 无回调 / 回调失败 / 硬件未就绪 → 静默回退缓存
       (有意的优雅降级, 调用者无法区分此值与新鲜值) */
    param_type_t t = entry_type(e);
    if (t >= PARAM_TYPE_COUNT) return PARAM_ERR_TYPE_MISMATCH;
    return g_param_data_ops[t].read(e, value);
}

/**
 * @brief 参数写入基类实现 — pre_write → 缓存更新 + 标脏 (无 apply)
 *
 * IP 的 write / write_cache 与 App 的 write_cache 均指向本实现。
 */
int param_entry_write(param_entry_t *e, param_value_t value)
{
    param_module_t *m = param_entry_module(e);
    if (!m) return PARAM_ERR_NOT_FOUND;

    param_type_t t = entry_type(e);
    if (t >= PARAM_TYPE_COUNT) return PARAM_ERR_TYPE_MISMATCH;

    if (entry_flags(e) & PARAM_FLAG_EXEC)
        return param_entry_is_exec(e) ? PARAM_ERR_READONLY : PARAM_ERR_TYPE_MISMATCH;

    if (entry_flags(e) & PARAM_FLAG_READONLY) return PARAM_ERR_READONLY;

    if (!g_param_pre_write[t](e, &value)) return PARAM_ERR_OUT_OF_RANGE;

    system_lock(SYS_LOCK_PARAM);
    g_param_data_ops[t].cache_update(e, value);
    param_entry_mark_dirty(e);
    system_unlock(SYS_LOCK_PARAM);
    return PARAM_OK;
}

/**
 * @brief 参数写入 (带 apply) — 基类写入 + 模块业务回调
 *
 * App 专用槽位: 缓存先行更新后调用模块 write 回调 (校验/拒绝/副作用),
 * 回调返回非 OK 时缓存保持新值仅不标脏。
 */
int param_entry_write_apply(param_entry_t *e, param_value_t value)
{
    param_module_t *m = param_entry_module(e);
    if (!m) return PARAM_ERR_NOT_FOUND;

    param_type_t t = entry_type(e);
    if (t >= PARAM_TYPE_COUNT) return PARAM_ERR_TYPE_MISMATCH;

    if (entry_flags(e) & PARAM_FLAG_EXEC)
        return param_entry_is_exec(e) ? PARAM_ERR_READONLY : PARAM_ERR_TYPE_MISMATCH;

    if (entry_flags(e) & PARAM_FLAG_READONLY) return PARAM_ERR_READONLY;

    if (!g_param_pre_write[t](e, &value)) return PARAM_ERR_OUT_OF_RANGE;

    /* 缓存先行 (不回滚): 回调执行期间缓存已为新值,
       回调内 param_read / param_save_one 均作用于新值 (含 BLOB/STRING)。
       回调拒绝时缓存保持新值, 仅不标记 dirty。 */
    system_lock(SYS_LOCK_PARAM);
    g_param_data_ops[t].cache_update(e, value);
    system_unlock(SYS_LOCK_PARAM);

    if (m->write) {
        int ret = m->write(m->ctx, e->param_id, value);
        if (ret != PARAM_OK) return ret;
    }

    system_lock(SYS_LOCK_PARAM);
    param_entry_mark_dirty(e);
    system_unlock(SYS_LOCK_PARAM);

    return PARAM_OK;
}

/**
 * @brief 立即写入 — EXEC 路由 / READONLY 过滤 / pre_write + 缓存提交
 *
 * "立即"的含义是"写到本模块可及的最深层", **不是**"硬件寄存器已更新":
 *   - App 模块: 最深层就是缓存, 硬件由模块的 flush 回调稍后拉取;
 *   - IP 模块:  write 回调存在时直通硬件; 不存在时同样只到缓存,
 *                值留在缓存里等 flush 的批量回调或 exec 命令下发。
 *
 * 因此 write 是**可选钩子**, 缺失不构成错误 (App / IP 一视同仁)。这与仓库里
 * 既有的两条语义一致:
 *   - param_entry_write / param_entry_write_apply 都不要求 write 存在;
 *   - _ip_flush_write_entry 对无 write 回调的驱动也是"清脏后跳过"。
 * 若在此返回错误, 同一配置下 param_write 成功而 param_write_immediate 失败,
 * 调用者无法从 API 层解释该差异; 而且"该 IP 驱动既无 write 又无 flush"这类
 * 配置问题属于注册期诊断 (param_check_flush_integrity 一类), 不该由单次
 * 写入路径承担。
 *
 * @param e     参数条目
 * @param value 待写入值 (pre_write 会就地裁剪)
 * @return PARAM_OK 成功, 或其他错误码
 */
int param_entry_write_immediate(param_entry_t *e, param_value_t value)
{
    param_module_t *m = param_entry_module(e);
    if (!m) return PARAM_ERR_NOT_FOUND;

    param_type_t t = entry_type(e);
    if (t >= PARAM_TYPE_COUNT) return PARAM_ERR_TYPE_MISMATCH;

    if (entry_flags(e) & PARAM_FLAG_EXEC) {
        if (!param_entry_is_exec(e)) return PARAM_ERR_TYPE_MISMATCH;
        if (m->exec) {
            int ret = m->exec(m->ctx, e->param_id, value);
            if (ret == PARAM_OK) {
                system_lock(SYS_LOCK_PARAM);
                param_entry_clear_dirty(e);
                system_unlock(SYS_LOCK_PARAM);
            }
            return ret;
        }
        return PARAM_ERR_NOT_FOUND;
    }

    if (entry_flags(e) & PARAM_FLAG_READONLY) return PARAM_ERR_READONLY;

    if (!g_param_pre_write[t](e, &value)) return PARAM_ERR_OUT_OF_RANGE;

    /* 缓存先行 (有意不回滚): 回调执行期间缓存已为新值。
       回调语义由模块类型决定 (App: apply 校验; IP: driver 直通硬件),
       失败不清脏、错误码上抛。 */
    system_lock(SYS_LOCK_PARAM);
    g_param_data_ops[t].cache_update(e, value);
    system_unlock(SYS_LOCK_PARAM);

    /* write 为可选钩子: 缺失时"立即"只到缓存层, 不报错 (见函数头注释) */
    if (m->write) {
        int ret = m->write(m->ctx, e->param_id, value);
        if (ret != PARAM_OK)
            return ret;
    }

    system_lock(SYS_LOCK_PARAM);
    param_entry_clear_dirty(e);
    system_unlock(SYS_LOCK_PARAM);
    return PARAM_OK;
}

/** @brief write_raw 公共主体 — EXEC 路由 / BLOB 长度校验 / 值类型转换 + 委托 write_fn */
static int param_entry_write_raw_to(param_entry_t *e, const uint8_t *data, uint16_t len,
                                    int (*write_fn)(param_entry_t *, param_value_t))
{
    param_module_t *m = param_entry_module(e);
    if (!m) return PARAM_ERR_NOT_FOUND;

    param_type_t t = entry_type(e);
    if (t >= PARAM_TYPE_COUNT) return PARAM_ERR_TYPE_MISMATCH;

    if (entry_flags(e) & PARAM_FLAG_EXEC) {
        if (!param_entry_is_exec(e)) return PARAM_ERR_TYPE_MISMATCH;
        if (m->exec) {
            param_value_t arg = { .ptr = (void *)data };
            int ret = m->exec(m->ctx, e->param_id, arg);
            if (ret == PARAM_OK) {
                system_lock(SYS_LOCK_PARAM);
                param_entry_clear_dirty(e);
                system_unlock(SYS_LOCK_PARAM);
            }
            return ret;
        }
        return PARAM_ERR_NOT_FOUND;
    }

    if (entry_flags(e) & PARAM_FLAG_READONLY) return PARAM_ERR_READONLY;

    if (t == PARAM_TYPE_BLOB) {
        param_blob_entry_t *be = (param_blob_entry_t *)e;
        if (len != be->blob_size) return PARAM_ERR_OUT_OF_RANGE;
        param_value_t value = { .ptr = (void *)data };
        return write_fn(e, value);
    }
    if (t == PARAM_TYPE_STRING) {
        /* 按 len 定界落缓存: cache_update_string 是 strncpy(max_len) 语义,
           调用者缓冲在 len 内没有 '\0' 时会被越过 len 继续扫描。
           此处先按 len (且不超 max_len) 拷贝并补 '\0', 再以 "缓存自身"
           作为源交给 write_fn —— cache_update_string 检测到 src==dst 会
           跳过自拷贝, pre_write / apply / dirty / notify 语义不变。 */
        param_string_entry_t *se = (param_string_entry_t *)e;
        if (!se->cache.ptr || se->max_len == 0) return PARAM_ERR_NOT_FOUND;

        uint16_t n = (len > se->max_len) ? se->max_len : len;
        system_lock(SYS_LOCK_PARAM);
        if (se->cache.ptr != (const void *)data)
            memcpy(se->cache.ptr, data, n);
        ((char *)se->cache.ptr)[n] = '\0';
        system_unlock(SYS_LOCK_PARAM);

        param_value_t value = { .ptr = (void *)se->cache.ptr };
        return write_fn(e, value);
    }
    /* 值类型 (UINT/INT/FLOAT/BOOL/ENUM) 处理 */
    if (len == 0 || len > sizeof(param_value_t)) return PARAM_ERR_TYPE_MISMATCH;

    param_value_t value;
    memset(&value, 0, sizeof(value));
    memcpy(&value, data, len);

    return write_fn(e, value);
}

/** @brief 原始字节流写入 — 委托基类写入 (IP write_raw 槽位) */
int param_entry_write_raw(param_entry_t *e, const uint8_t *data, uint16_t len)
{
    return param_entry_write_raw_to(e, data, len, param_entry_write);
}

/** @brief 原始字节流写入 — 委托带 apply 的写入 (App write_raw 槽位) */
int param_entry_write_raw_apply(param_entry_t *e, const uint8_t *data, uint16_t len)
{
    return param_entry_write_raw_to(e, data, len, param_entry_write_apply);
}

int param_entry_load(param_entry_t *e)
{
    param_type_t t = entry_type(e);
    if (t >= PARAM_TYPE_COUNT) return PARAM_ERR_TYPE_MISMATCH;
    return g_param_data_ops[t].load(e);
}

int param_entry_reset(param_entry_t *e)
{
    param_type_t t = entry_type(e);
    if (t >= PARAM_TYPE_COUNT) return PARAM_ERR_TYPE_MISMATCH;
    return g_param_data_ops[t].reset(e);
}

/** @brief 条目级 flush 空实现 (App flush 槽位: App 无硬件, 条目级 flush 为 no-op) */
int param_entry_flush_nop(param_entry_t *e)
{
    (void)e;
    return PARAM_OK;
}

/* ---- 模块级基类实现 ---- */

void param_module_mark_dirty(param_module_node_t *m, uint16_t local_id)
{
    /* READONLY/EXEC 条目永不进入写路径: 模块级 dirty 一并跳过。
       防御外部调用点对非可写条目的标记 (否则会出现"模块 dirty
       但零条目 dirty"的假脏状态, 触发一次空 flush)。 */
    for (uint16_t i = 0; i < m->param_count; i++) {
        param_entry_t *e = m->table[i];
        if (e && PARAM_LOCAL_ID(e->param_id) == local_id) {
            if (!param_entry_is_writable(e))
                return;
            break;
        }
    }
    m->dirty = 1;
}

void param_module_clear_dirty(param_module_node_t *m, uint16_t local_id)
{
    (void)local_id;
    uint8_t has_dirty = 0;
    for (uint16_t i = 0; i < m->param_count; i++) {
        param_entry_t *pe = m->table[i];
        if (pe && entry_dirty(pe)) { has_dirty = 1; break; }
    }
    if (!has_dirty) m->dirty = 0;
}

int param_module_exec(param_module_node_t *m, uint32_t cmd_id, param_value_t arg)
{
    param_module_t *mod = (param_module_t *)m;
    if (mod->exec)
        return mod->exec(mod->ctx, cmd_id, arg);
    return PARAM_ERR_NOT_FOUND;
}

void param_module_reset(param_module_node_t *m)
{
    m->dirty = 0;
}

void param_module_deinit(param_module_node_t *m)
{
    m->initialized = 0;
}

/**
 * @brief 通知回调 — 携带缓存中的**生效值**
 *
 * pre_write 会裁剪超范围值 (UINT/INT/FLOAT), apply 回调也可能修正值,
 * 因此调用者传入的原始值不一定等于最终写入缓存的值。notify 必须以
 * 缓存为准, 否则订阅者会收到一个从未被存储的值。
 *
 * 快照在锁内完成, 回调本身在锁外执行 (锁纪律第 4 条: 回调一律锁外)。
 * BLOB/STRING 的 .ptr 指向框架缓存缓冲区 (生命周期覆盖整个运行期),
 * 调用者的瞬态缓冲区不参与。
 *
 * @param param_id 参数 ID
 * @param e        参数条目 (已成功写入)
 */
static void param_notify_effective(uint32_t param_id, param_entry_t *e)
{
    if (!g_pm.notify_cb)
        return;

    param_value_t effective;
    system_lock(SYS_LOCK_PARAM);
    effective = *entry_cache(e);
    system_unlock(SYS_LOCK_PARAM);

    g_pm.notify_cb(param_id, effective);
}

/**
 * @brief 写入入口公共前缀 — 初始化/存在性校验 + 无长度类型拦截
 *
 * param_write / param_write_cache / param_write_immediate / param_write_raw
 * 共用的第一步: 校验框架已初始化、条目已注册, 并按需拒绝 BLOB/STRING。
 *
 * @param param_id        参数 ID
 * @param[out] out        解析到的条目 (仅在返回 PARAM_OK 时有效)
 * @param reject_ptr_types true = 拒绝 BLOB/STRING (调用入口没有长度信息,
 *                        向 STRING 条目写 .u32 会被当成指针解引用)
 * @return PARAM_OK 成功; PARAM_ERR_NOT_INITIALIZED 未初始化;
 *         PARAM_ERR_INVALID_ID 参数不存在; PARAM_ERR_TYPE_MISMATCH 类型被拦
 */
static int param_write_resolve(uint32_t param_id, param_entry_t **out,
                               bool reject_ptr_types)
{
    if (!g_pm.initialized)
        return PARAM_ERR_NOT_INITIALIZED;

    param_entry_t *e = param_entry_find(param_id);
    if (!e)
        return PARAM_ERR_INVALID_ID;

    if (reject_ptr_types) {
        /* BLOB/STRING 的 payload 经 .ptr 传递, 而 param_value_t 无法携带长度,
           本路径无法校验源的容量/有效性。请使用带长度的专用入口:
           param_write_blob / param_write_string / param_write_raw */
        param_type_t t = entry_type(e);
        if (t == PARAM_TYPE_BLOB || t == PARAM_TYPE_STRING)
            return PARAM_ERR_TYPE_MISMATCH;
    }

    *out = e;
    return PARAM_OK;
}

/**
 * @brief 按参数所属模块同步模块级 dirty 标志 (置脏或清脏)
 *
 * 这是全部写入路径共用的唯一实现: "找模块 → 选 mark_dirty/clear_dirty →
 * 锁内调用" 三步只在此处存在。调用方 (param_write_complete /
 * param_load_one) 用 mark 参数表达语义, 不自行展开。
 *
 * @note 内部自行持锁: 调用者**不得**持有 SYS_LOCK_PARAM (端口层为非递归锁,
 *       否则自死锁)。
 * @param param_id 参数 ID (用于定位模块与本地 ID)
 * @param mark     true = 置脏 (缓存待下发), false = 清脏 (已写到最深层)
 */
static void param_module_dirty_sync(uint32_t param_id, bool mark)
{
    param_module_node_t *m = param_module_find((uint16_t)PARAM_MODULE_ID(param_id));
    if (!m || !m->vtable)
        return;

    void (*fn)(param_module_node_t *, uint16_t) =
        mark ? m->vtable->mark_dirty : m->vtable->clear_dirty;
    if (!fn)
        return;

    system_lock(SYS_LOCK_PARAM);
    fn(m, (uint16_t)PARAM_LOCAL_ID(param_id));
    system_unlock(SYS_LOCK_PARAM);
}

/**
 * @brief 写入成功后的公共尾巴 — 同步模块脏位 + 通知生效值
 *
 * 所有写入入口 (EXEC 原始写这一特例除外) 在 vtable 返回 PARAM_OK 后都执行这两步,
 * 且顺序不可交换: 先落脏位再通知, 保证 notify 订阅者观察到模块状态已与缓存一致。
 *
 * @param param_id   参数 ID
 * @param e          已成功写入的条目
 * @param mark_dirty true = 置脏 (待下发), false = 清脏 (已到最深层)
 */
static void param_write_complete(uint32_t param_id, param_entry_t *e, bool mark_dirty)
{
    param_module_dirty_sync(param_id, mark_dirty);
    /* 通知生效值而非入参: pre_write 裁剪 / apply 修正后的缓存值才是真值 */
    param_notify_effective(param_id, e);
}

/* ================================================================
 *  读写操作 — vtable 分派
 *
 *  所有写入入口共用同一骨架, 差异只有三处 (入口语义决定):
 *    1. 前置拦截: 有无长度信息 → param_write_resolve(reject_ptr_types)
 *    2. 分派目标: entry->vtable 的哪个槽位 (write / write_cache /
 *       write_immediate / write_raw)
 *    3. 收尾: 置脏还是清脏 → param_write_complete(mark_dirty)
 *  其余步骤 (存在性校验、模块查找、加锁、生效值通知) 均由上面两个函数提供,
 *  各入口只表达自己那三处差异。
 * ================================================================ */

/** @brief 写入参数 (缓存模式，不立即刷硬件，标记 dirty) */
int param_write(uint32_t param_id, param_value_t value)
{
    param_entry_t *e = NULL;
    int ret = param_write_resolve(param_id, &e, true);
    if (ret != PARAM_OK)
        return ret;

    ret = e->vtable->write(e, value);
    if (ret == PARAM_OK)
        param_write_complete(param_id, e, true);
    return ret;
}

/** @brief 写入参数缓存 (跳过 apply 回调，仅更新缓存 + 标记 dirty) */
int param_write_cache(uint32_t param_id, param_value_t value)
{
    param_entry_t *e = NULL;
    int ret = param_write_resolve(param_id, &e, true);
    if (ret != PARAM_OK)
        return ret;

    ret = e->vtable->write_cache(e, value);
    if (ret == PARAM_OK)
        param_write_complete(param_id, e, true);
    return ret;
}

/** @brief 立即写入参数 (直通硬件/回调，不产生 dirty) */
int param_write_immediate(uint32_t param_id, param_value_t value)
{
    param_entry_t *e = NULL;
    int ret = param_write_resolve(param_id, &e, true);
    if (ret != PARAM_OK)
        return ret;

    ret = e->vtable->write_immediate(e, value);
    if (ret == PARAM_OK)
        param_write_complete(param_id, e, false);
    return ret;
}

/** @brief 原始字节流写入 (EXEC 参数自动路由到 exec) */
int param_write_raw(uint32_t param_id, const uint8_t *data, uint16_t len)
{
    /* 检查顺序: 未初始化优先于 data/len 校验 —— 框架未初始化时任何调用
       都返回 PARAM_ERR_NOT_INITIALIZED */
    if (!g_pm.initialized)
        return PARAM_ERR_NOT_INITIALIZED;
    if (!data || len == 0)
        return PARAM_ERR_INVALID_ID;

    param_entry_t *e = NULL;
    int ret = param_write_resolve(param_id, &e, false);
    if (ret != PARAM_OK)
        return ret;

    ret = e->vtable->write_raw(e, data, len);
    if (ret != PARAM_OK)
        return ret;

    if (param_entry_is_exec(e)) {
        /* EXEC 不写缓存 (EXEC 类型的 cache_update 为空操作), 不存在
           "生效值": 通知携带命令负载, 与 exec 回调收到的 arg 一致。
           memset 清零后只填充有效字节，剩余字段为 0 (best-effort)。
           注: 此分支不调 param_module_dirty_sync —— EXEC 条目被模块级
           mark_dirty 的可写过滤挡掉, 该调用恒为空操作。 */
        param_value_t arg;
        memset(&arg, 0, sizeof(arg));
        if (len <= sizeof(arg))
            memcpy(&arg, data, len);
        else
            arg.ptr = (void *)data;
        if (g_pm.notify_cb)
            g_pm.notify_cb(param_id, arg);
        return PARAM_OK;
    }

    /* 值类型 / BLOB / STRING: 以缓存生效值为准 (裁剪结果已体现,
       BLOB/STRING 的 .ptr 指向框架缓存缓冲区而非调用者瞬态缓冲) */
    param_write_complete(param_id, e, true);
    return PARAM_OK;
}

/** @brief 读取参数当前缓存值 */
int param_read(uint32_t param_id, param_value_t *value)
{
    if (!g_pm.initialized)
        return PARAM_ERR_NOT_INITIALIZED;
    if (!value)
        return PARAM_ERR_INVALID_ID;

    param_entry_t *e = param_entry_find(param_id);
    if (!e)
        return PARAM_ERR_INVALID_ID;
    int ret = e->vtable->read(e, value);
    return ret;
}

int param_read_cache(uint32_t param_id, param_value_t *value)
{
    if (!g_pm.initialized)
        return PARAM_ERR_NOT_INITIALIZED;
    if (!value)
        return PARAM_ERR_INVALID_ID;

    system_lock(SYS_LOCK_PARAM);
    param_entry_t *e = param_entry_find(param_id);
    if (!e) {
        system_unlock(SYS_LOCK_PARAM);
        return PARAM_ERR_INVALID_ID;
    }

    param_type_t t = entry_type(e);
    if (t >= PARAM_TYPE_COUNT) {
        system_unlock(SYS_LOCK_PARAM);
        return PARAM_ERR_TYPE_MISMATCH;
    }

    int ret = g_param_data_ops[t].read(e, value);
    system_unlock(SYS_LOCK_PARAM);
    return ret;
}

int param_update(uint32_t param_id)
{
    if (!g_pm.initialized)
        return PARAM_ERR_NOT_INITIALIZED;

    /* 先取缓存快照 (param_read_cache 锁内), 再重放完整写入路径
       (pre_write → apply → cache_update → dirty → notify)。
       注意: 快照与 write 之间非原子, 并发写者可能覆盖此值。
       直接走 vtable 而非 param_write: 后者拒绝 BLOB/STRING, 而本
       函数必须支持它们 (以缓存自身为源的重放, 不涉外部指针) */
    param_value_t cached;
    int ret = param_read_cache(param_id, &cached);
    if (ret != PARAM_OK)
        return ret;

    param_entry_t *e = param_entry_find(param_id);
    if (!e)
        return PARAM_ERR_INVALID_ID;

    ret = e->vtable->write(e, cached);
    if (ret != PARAM_OK)
        return ret;

    /* 通知生效值而非入参: 快照 cached 是 re-apply 前的旧值,
       pre_write 裁剪后缓存可能已变化 */
    param_write_complete(param_id, e, true);

    return PARAM_OK;
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
 * 三种长度定义见 param_entry_data_size() (与 param_get_size 共用),
 * 本函数不自行推导。
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
        return PARAM_ERR_NOT_INITIALIZED;
    if (!len)
        return PARAM_ERR_INVALID_ID;

    system_lock(SYS_LOCK_PARAM);
    param_entry_t *e = param_entry_find(param_id);
    if (!e) {
        system_unlock(SYS_LOCK_PARAM);
        return PARAM_ERR_INVALID_ID;
    }

    param_type_t t = entry_type(e);
    if (t >= PARAM_TYPE_COUNT) {
        system_unlock(SYS_LOCK_PARAM);
        return PARAM_ERR_TYPE_MISMATCH;
    }

    /* 长度: 值类型取 param_value_t 宽度, BLOB/STRING 取各自容量;
       源指针: 值类型取缓存本体, BLOB/STRING 取缓存指向的外部缓冲区 */
    uint16_t total = param_entry_data_size(e);
    const uint8_t *src =
        (t == PARAM_TYPE_BLOB || t == PARAM_TYPE_STRING)
            ? (const uint8_t *)entry_cache(e)->ptr
            : (const uint8_t *)entry_cache(e);

    if (!src) {
        system_unlock(SYS_LOCK_PARAM);
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

    system_unlock(SYS_LOCK_PARAM);
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
        return PARAM_ERR_NOT_INITIALIZED;
    if (!buf || buf_size == 0)
        return PARAM_ERR_INVALID_ID;

    param_entry_t *e = param_entry_find(id);
    if (!e)
        return PARAM_ERR_INVALID_ID;
    if (entry_type(e) != PARAM_TYPE_STRING)
        return PARAM_ERR_TYPE_MISMATCH;

    system_lock(SYS_LOCK_PARAM);
    const char *src = (const char *)entry_cache(e)->ptr;
    if (!src) {
        system_unlock(SYS_LOCK_PARAM);
        return PARAM_ERR_NOT_FOUND;
    }

    uint16_t copy = buf_size - 1;
    strncpy(buf, src, copy);
    buf[copy] = '\0';
    system_unlock(SYS_LOCK_PARAM);

    return PARAM_OK;
}

/**
 * @brief 带长度 typed 入口的公共尾巴 — 源已校验、缓存已就绪
 *
 * 与 param_write 的差异: 不做 "union 无长度类型" 的拒绕 —— 调用者已用
 * 专用签名给出长度并校验过源指针。value 传缓存自身指针, 因此
 * cache_update_* 的 src==dst 分支会跳过自拷贝, 只保留
 * pre_write / apply / dirty / notify 语义。
 *
 * @note apply/notify 收到的 BLOB/STRING 指针是**参数缓存自身**,
 *       生命周期覆盖整个运行期, 调用者的瞬态缓冲不参与。
 */
static int param_write_typed_tail(uint32_t param_id, param_entry_t *e)
{
    param_value_t value = { .ptr = entry_cache_ptr(e)->ptr };

    int ret = e->vtable->write(e, value);
    if (ret != PARAM_OK)
        return ret;

    /* 通知生效值: BLOB/STRING 的 .ptr 即框架缓存缓冲区 */
    param_write_complete(param_id, e, true);

    return PARAM_OK;
}

/**
 * @brief STRING 类型写入 (带长度语义的专用入口)
 *
 * 先校验目标参数类型为 PARAM_TYPE_STRING，再按"最多 max_len + 1 字节"
 * 有界读取源串 (遇 '\0' 即停) 并截断到 max_len，随后以缓存自身为源
 * 重放完整写入路径 (pre_write → apply → dirty → notify)。
 *
 * @note 不经 param_write: 该入口因无长度信息而拒收 BLOB/STRING。
 */
int param_write_string(uint32_t id, const char *str)
{
    if (!g_pm.initialized)
        return PARAM_ERR_NOT_INITIALIZED;
    if (!str)
        return PARAM_ERR_INVALID_ID;

    param_entry_t *e = param_entry_find(id);
    if (!e)
        return PARAM_ERR_INVALID_ID;
    if (entry_type(e) != PARAM_TYPE_STRING)
        return PARAM_ERR_TYPE_MISMATCH;

    param_string_entry_t *se = (param_string_entry_t *)e;
    if (!se->cache.ptr || se->max_len == 0)
        return PARAM_ERR_NOT_FOUND;

    /* 有界求长: memchr 只扫 max_len + 1 字节, 不依赖 strnlen (非 C17 标准) */
    uint16_t n = se->max_len;
    const char *nul = (const char *)memchr(str, '\0', (size_t)se->max_len + 1);
    if (nul)
        n = (uint16_t)(nul - str);

    system_lock(SYS_LOCK_PARAM);
    if (se->cache.ptr != (const void *)str)
        memcpy(se->cache.ptr, str, n);
    ((char *)se->cache.ptr)[n] = '\0';
    system_unlock(SYS_LOCK_PARAM);

    return param_write_typed_tail(id, e);
}

/**
 * @brief BLOB 类型写入 (带长度的专用入口)
 *
 * BLOB 必须**整块**写: len 与条目 blob_size 不等时直接拒绝, 不做截断/补零
 * (避免静默损坏)。写入成功后以缓存自身为源重放完整写入路径。
 *
 * @param id  参数 ID (类型必须为 PARAM_TYPE_BLOB)
 * @param buf 源数据 (非 NULL)
 * @param len 源长度 (必须等于 blob_size)
 * @return PARAM_OK 成功; PARAM_ERR_INVALID_ID 源为 NULL;
 *         PARAM_ERR_TYPE_MISMATCH 目标不是 BLOB;
 *         PARAM_ERR_OUT_OF_RANGE 长度与 blob_size 不符
 */
int param_write_blob(uint32_t id, const void *buf, uint16_t len)
{
    if (!g_pm.initialized)
        return PARAM_ERR_NOT_INITIALIZED;
    if (!buf)
        return PARAM_ERR_INVALID_ID;

    param_entry_t *e = param_entry_find(id);
    if (!e)
        return PARAM_ERR_INVALID_ID;
    if (entry_type(e) != PARAM_TYPE_BLOB)
        return PARAM_ERR_TYPE_MISMATCH;

    param_blob_entry_t *be = (param_blob_entry_t *)e;
    if (!be->cache.ptr || be->blob_size == 0)
        return PARAM_ERR_NOT_FOUND;
    if (len != be->blob_size)
        return PARAM_ERR_OUT_OF_RANGE;

    system_lock(SYS_LOCK_PARAM);
    if (be->cache.ptr != buf)
        memcpy(be->cache.ptr, buf, len);
    system_unlock(SYS_LOCK_PARAM);

    return param_write_typed_tail(id, e);
}

/**
 * @brief 获取参数数据大小 (字节数)
 *
 * 长度定义见 param_entry_data_size() (与 param_read_raw 共用):
 * 值类型 sizeof(param_value_t) (平台相关: 32-bit 4B, 64-bit 8B);
 * BLOB → blob_size; STRING → max_len + 1 (含结尾 '\0')。
 *
 * @param param_id 参数 ID
 * @return 字节数; 未注册或类型越界返回 0
 */
uint16_t param_get_size(uint32_t param_id)
{
    system_lock(SYS_LOCK_PARAM);
    param_entry_t *e = param_entry_find(param_id);
    uint16_t result = e ? param_entry_data_size(e) : 0;
    system_unlock(SYS_LOCK_PARAM);
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
        return PARAM_ERR_NOT_INITIALIZED;
    system_lock(SYS_LOCK_PARAM);
    param_entry_t *e = param_entry_find(cmd_id);
    if (!param_entry_is_exec(e)) {
        system_unlock(SYS_LOCK_PARAM);
        return PARAM_ERR_NOT_FOUND;
    }
    param_module_node_t *m = param_module_find(PARAM_MODULE_ID(cmd_id));
    if (!m || !m->vtable || !m->vtable->exec) {
        system_unlock(SYS_LOCK_PARAM);
        return PARAM_ERR_NOT_FOUND;
    }
    system_unlock(SYS_LOCK_PARAM);
    param_value_t arg = {.ptr = user_arg};
    return m->vtable->exec(m, cmd_id, arg);
}

/**
 * @brief 模块链表快照的批容量上限
 *
 * 栈上指针数组容量, 不是模块总数上限: 分批遍历统一由 param_module_scan
 * 实现 (锁内按谓词快照至多本值个指针, 锁外回调, 再进锁续扫)。
 * 因此栈占用有界，而模块数无上限。
 */
#define PARAM_MODULE_BATCH_MAX 8

/** @brief 分批遍历的"谁进批"谓词 — 锁内调用, 不得调用任何加锁 API */
typedef bool (*param_module_accept_fn)(const param_module_node_t *m, void *ctx);
/** @brief 分批遍历的"进批后做什么" — 锁外调用, 返回 false 终止整次遍历 */
typedef bool (*param_module_visit_fn)(param_module_node_t *m, void *ctx);

/**
 * @brief 分批遍历模块链表 — 锁内只快照指针, 锁外执行 visit
 *
 * param_flush / param_foreach / param_module_foreach 共用的唯一实现:
 * 每批在锁内按 accept 谓词收集至多 PARAM_MODULE_BATCH_MAX 个模块指针,
 * 解锁后逐个 visit; 本批未扫完则重新进锁从游标处续扫。
 *
 * 因此栈占用有界 (一个指针数组), 模块数无上限; 且 visit 期间锁是释放的,
 * 回调内可安全调用 param_read / param_write / param_save_one 等加锁 API。
 *
 * @param accept NULL = 全部模块进批; 谓词在锁内执行, 不得调加锁 API
 * @param visit  锁外对每个已快照模块调用; 返回 false 立即终止整次遍历
 * @param ctx    透传给 accept / visit 的调用方上下文
 */
static void param_module_scan(param_module_accept_fn accept,
                              param_module_visit_fn visit, void *ctx)
{
    uint16_t cursor = 0; /* 链表游标: 已快照过的节点数 */

    for (;;) {
        param_module_node_t *batch[PARAM_MODULE_BATCH_MAX];
        uint16_t n = 0;
        uint16_t pos = 0;
        uint8_t more = 0;

        /* ---- 阶段 1 (锁内): 只收集模块指针, 不执行回调 ---- */
        system_lock(SYS_LOCK_PARAM);
        for (param_module_node_t *m = g_pm.module_head; m; m = m->next, pos++) {
            if (pos < cursor)
                continue;
            if (n >= PARAM_MODULE_BATCH_MAX) {
                more = 1; /* 本批已满, 下一轮从当前节点续扫 */
                break;
            }
            if (!accept || accept(m, ctx))
                batch[n++] = m;
        }
        cursor = pos;
        system_unlock(SYS_LOCK_PARAM);

        /* ---- 阶段 2 (锁外): 执行调用方回调 ---- */
        for (uint16_t i = 0; i < n; i++) {
            if (!visit(batch[i], ctx))
                return;
        }

        if (!more)
            return;
    }
}

/** @brief param_flush 的分批遍历上下文 */
typedef struct {
    int last_err;       /**< 最后一个失败的错误码 */
    uint16_t err_count; /**< 本轮失败模块数 (饱和于 UINT16_MAX) */
} flush_ctx_t;

/** @brief param_flush 谓词: 只对"有脏位且提供了 flush 通路"的模块进批 */
static bool flush_accept(const param_module_node_t *m, void *ctx)
{
    (void)ctx;
    return m->dirty && m->vtable && m->vtable->flush;
}

/** @brief param_flush 访问体: 执行模块 flush, 失败只记录不中断 */
static bool flush_visit(param_module_node_t *m, void *ctx)
{
    flush_ctx_t *f = (flush_ctx_t *)ctx;

    int ret = m->vtable->flush(m);
    if (ret != PARAM_OK) {
        f->last_err = ret;
        if (f->err_count < UINT16_MAX)
            f->err_count++;
    }
    return true; /* 单个模块失败不中断整轮 */
}

/**
 * @brief 将所有模块的 dirty 参数刷入硬件
 *
 * @details
 * 两阶段执行 (与 param_load_all 的锁策略对齐), 全局锁只保护框架状态,
 * **不覆盖任何用户回调或硬件访问**:
 *
 *   **阶段 1 (锁内)**: 遍历模块链表 (按 MODULE_INIT_ORDER 顺序)，
 *     仅把 dirty 模块的**指针**快照到栈上数组，不执行任何回调
 *     (分批机制与谓词由 param_module_scan / flush_accept 提供)。
 *     单批容量 PARAM_MODULE_BATCH_MAX，超出则分批: 每批处理完重新进锁
 *     续扫 (cursor 游标)，因此栈占用有界、模块数无上限。
 *
 *   **阶段 2 (锁外)**: 逐模块调用 module->vtable->flush。所有硬件访问与
 *     用户回调均在此阶段执行 —— 期间其他任务可正常调用 param_read /
 *     param_write / param_save_one 等加锁 API，不会被一次慢速总线写
 *     阻塞; 回调内也不会因重入全局锁而自死锁 (非递归 mutex 语义)。
 *
 * 并发语义:
 *   - 同一时刻只允许一个 flusher (g_pm.flushing)。回调内递归调用
 *     param_flush() 返回 PARAM_ERR_BUSY，而不是死锁或无限递归。
 *   - 阶段 2 期间被并发置脏的参数/模块由下一轮 param_flush 补发:
 *     脏位清理采用 take-and-clear (锁内"取值 + 清脏"原子完成),
 *     新脏位不会被本次 flush 吞掉。
 *
 * 即使某个模块 flush 失败，仍会继续处理后续模块，
 * 错误计数累加到 flush_error_count。
 *
 * @return PARAM_OK 全部成功，或最后一个失败的错误码，
 *         PARAM_ERR_BUSY 已有 flush 在进行中
 */
int param_flush(void)
{
    if (!g_pm.initialized)
        return PARAM_ERR_NOT_INITIALIZED;

    /* ---- 阶段 0 (锁内): 单 flusher 互斥 ---- */
    system_lock(SYS_LOCK_PARAM);
    if (g_pm.flushing) {
        system_unlock(SYS_LOCK_PARAM);
        return PARAM_ERR_BUSY;
    }
    g_pm.flushing = 1;
    system_unlock(SYS_LOCK_PARAM);

    flush_ctx_t fctx = { .last_err = PARAM_OK, .err_count = 0 };
    param_module_scan(flush_accept, flush_visit, &fctx);

    /* ---- 阶段 3 (锁内): 回写统计 + 释放 flusher ---- */
    system_lock(SYS_LOCK_PARAM);
    if (fctx.err_count) {
        uint32_t sum = (uint32_t)g_pm.stats.flush_error_count + fctx.err_count;
        g_pm.stats.flush_error_count =
            (sum > UINT16_MAX) ? UINT16_MAX : (uint16_t)sum;
    }
    g_pm.flushing = 0;
    system_unlock(SYS_LOCK_PARAM);

    return fctx.last_err;
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
        return PARAM_ERR_NOT_INITIALIZED;

    g_pm.stats.flush_order_miss_count = 0;

    system_lock(SYS_LOCK_PARAM);

    param_module_node_t *m = g_pm.module_head;
    while (m) {
        if (!module_order_contains(g_module_order, MODULE_ORDER_COUNT,
                                   m->module_id))
        {
            g_pm.stats.flush_order_miss_count++;
        }
        m = m->next;
    }

    system_unlock(SYS_LOCK_PARAM);

    if (g_pm.stats.flush_order_miss_count > 0)
        return PARAM_ERR_NOT_FOUND;

    return PARAM_OK;
}

/**
 * @brief 存储后端返回值 → param_status_t (API 边界的唯一翻译点)
 *
 * param_storage_drv_t 的回调以 "0 = 成功, 非 0 = 失败" 表达结果 (三个后端
 * 均为 0/-1), 而公共 API 一律返回 param_status_t。所有持久化入口 (save /
 * delete / erase_all) 都经由本函数翻译, 使"写介质失败"在任何入口上都报
 * 同一个错误码 —— 若直接上抛后端值, -1 会被调用方误读为
 * PARAM_ERR_INVALID_ID ("无效 ID")。
 *
 * @note PARAM_ERR_NOT_SUPPORTED 原样保留: 它表示"本固件不支持该能力"
 *       (未编译持久化 / 后端缺该回调), 与"介质写入失败"是两件事。
 *
 * @param backend_ret 后端回调的返回值
 * @return PARAM_OK 成功; PARAM_ERR_NOT_SUPPORTED 后端不支持;
 *         其余一律 PARAM_ERR_FLASH_FAIL
 */
static int param_status_from_backend(int backend_ret)
{
    if (backend_ret == PARAM_OK)
        return PARAM_OK;
    if (backend_ret == PARAM_ERR_NOT_SUPPORTED)
        return PARAM_ERR_NOT_SUPPORTED;
    return PARAM_ERR_FLASH_FAIL;
}

/** @brief 持久化入口所需的存储后端能力 */
typedef enum {
    PARAM_CAP_SAVE = 0,   /**< storage->save (逐条写) */
    PARAM_CAP_LOAD,       /**< storage->load (逐条读) */
    PARAM_CAP_DELETE,     /**< storage->delete (删单条) */
    PARAM_CAP_ERASE_ALL,  /**< storage->erase_all (全擦) */
    PARAM_CAP_GET_ACTIVE, /**< storage->get_active_partition */
    PARAM_CAP_SET_ACTIVE, /**< storage->set_active_partition */
} param_storage_cap_t;

/**
 * @brief 持久化入口公共前导 — 校验框架就绪 + 后端具备指定能力
 *
 * 所有持久化 API 的第一步都是同一判断; "框架未初始化"(NOT_INITIALIZED) 与
 * "本固件不持久化 / 后端缺该回调"(NOT_SUPPORTED) 的区分策略也只在此处定义。
 *
 * @param cap 需要的后端回调
 * @param out 成功时写出当前驱动 (可传 NULL); 失败时不改写
 * @return PARAM_OK 可用; PARAM_ERR_NOT_INITIALIZED 框架未初始化;
 *         PARAM_ERR_NOT_SUPPORTED 无存储后端或后端缺该回调
 */
static int param_storage_require(param_storage_cap_t cap,
                                 const param_storage_drv_t **out)
{
    if (!g_pm.initialized)
        return PARAM_ERR_NOT_INITIALIZED;

    const param_storage_drv_t *drv = g_pm.storage;
    if (!drv)
        return PARAM_ERR_NOT_SUPPORTED;

    bool ok;
    switch (cap) {
    case PARAM_CAP_SAVE:
        ok = drv->save != NULL;
        break;
    case PARAM_CAP_LOAD:
        ok = drv->load != NULL;
        break;
    case PARAM_CAP_DELETE:
        ok = drv->delete != NULL;
        break;
    case PARAM_CAP_ERASE_ALL:
        ok = drv->erase_all != NULL;
        break;
    case PARAM_CAP_GET_ACTIVE:
        ok = drv->get_active_partition != NULL;
        break;
    case PARAM_CAP_SET_ACTIVE:
        ok = drv->set_active_partition != NULL;
        break;
    default:
        ok = false;
        break;
    }
    if (!ok)
        return PARAM_ERR_NOT_SUPPORTED;

    if (out)
        *out = drv;
    return PARAM_OK;
}

/** @brief param_save_all 的遍历体: 记录首个失败的错误码, 不中断遍历
 *
 * @note 由 param_hash_scan 在锁外调用, 故本函数**自行持锁**包住单条 save。
 *       不得省掉这把锁: vtable->save 虽然只读缓存, 但它驱动存储后端,
 *       而后端的内部可变状态 (mkv 的 write_offset / iniparser 的字典与
 *       .tmp 文件) 没有自己的锁 —— 框架锁是它们唯一的串行化来源。
 *       去掉后两次并发保存会写到同一偏移 / 同一临时文件, 造成记录覆盖。
 *       锁持有时间因此收敛到单条介质写, 而不是整轮。
 */
static void save_all_visit(param_entry_t *e, void *ctx)
{
    int *first_err = (int *)ctx;

    system_lock(SYS_LOCK_PARAM);
    int ret = e->vtable->save(e);
    system_unlock(SYS_LOCK_PARAM);

    if (ret != PARAM_OK && *first_err == PARAM_OK)
        *first_err = param_status_from_backend(ret);
}

/**
 * @brief 保存所有 PARAM_FLAG_PERSIST 参数到持久化存储
 *
 * 遍历哈希表，对每个 entry 调用 entry->vtable->save()。
 * 仅当 entry 的 flags 含 PARAM_FLAG_PERSIST 时才实际写入 (由子类实现判断)。
 *
 * 后端提供 save_begin/save_commit 时走批量事务: begin → 逐条 save →
 * commit, 把"N 次全量重写"降为"一次落盘"。后端只在自己内部打开批量
 * 模式, 不遍历框架条目 (依赖方向: 后端 → 端口)。
 *
 * 错误处理策略与 param_load_all 对称:
 *   - 遇错收集首个错误码，继续保存后续参数 (不提前退出)
 *   - 避免"第 N 个失败导致剩余参数全部丢失"的不一致状态
 *
 * @return PARAM_OK 全部成功，或首个失败的错误码 (经 param_status_from_backend
 *         归一, 与 param_save_one 同码)
 */
int param_save_all(void)
{
    const param_storage_drv_t *drv = NULL;
    int ret = param_storage_require(PARAM_CAP_SAVE, &drv);
    if (ret != PARAM_OK)
        return ret;

    /* 后端提供批量事务 → 先打开它, 逐条 save 仍走同一条路径, 最后一次性
       落盘。begin/commit 均在锁外调用, 实现内可安全使用加锁 API。 */
    bool batch = (drv->save_begin != NULL && drv->save_commit != NULL);
    if (batch) {
        int bret = drv->save_begin(drv->ctx);
        if (bret != PARAM_OK)
            return param_status_from_backend(bret); /* 不退化: 免得"批量不可用"被静默掩盖 */
    }

    /* 逐条保存 (无批量后端走同一路径): 收集首个错误码, 不提前退出。
       遍历经 param_hash_scan: 锁内只快照条目指针, 回调内再单独持锁包住
       单条 save —— 两次并发保存因此不会同时进入后端 (见 save_all_visit),
       而锁的持有时间也从"整轮"收敛到"单条"。 */
    int first_err = PARAM_OK;

    param_hash_scan(save_all_visit, &first_err);

    /* 提交: 即使中间有条目 save 失败也照样调用 (提交已成功部分,
       错误码已在 first_err 里, 此处只补"无更早错误时"的提交失败) */
    if (batch) {
        int cret = drv->save_commit(drv->ctx);
        if (cret != PARAM_OK && first_err == PARAM_OK)
            first_err = param_status_from_backend(cret);
    }

    return first_err;
}

/** @brief 保存单个参数到持久化存储 (错误码经归一, 与 param_save_all 一致)
 *
 * @note 查找无需持锁 (条目只增不删), 但 save 本身必须持锁: 存储后端的内部
 *       可变状态 (mkv 的 write_offset、iniparser 的字典/临时文件) 靠框架锁
 *       串行化, 没有自己的锁 (与 save_all_visit 同一理由)。
 */
int param_save_one(uint32_t param_id)
{
    int ret = param_storage_require(PARAM_CAP_SAVE, NULL);
    if (ret != PARAM_OK)
        return ret;

    param_entry_t *e = param_entry_find(param_id);
    if (!e)
        return PARAM_ERR_INVALID_ID;

    system_lock(SYS_LOCK_PARAM);
    ret = e->vtable->save(e);
    system_unlock(SYS_LOCK_PARAM);

    return param_status_from_backend(ret);
}

/* ================================================================
 *  存储后端分区管理
 * ================================================================ */

int param_storage_get_active_partition(uint8_t *index)
{
    const param_storage_drv_t *drv;
    int ret = param_storage_require(PARAM_CAP_GET_ACTIVE, &drv);
    if (ret != PARAM_OK)
        return ret;

    return param_status_from_backend(drv->get_active_partition(drv->ctx, index));
}

int param_storage_set_active_partition(uint8_t index)
{
    const param_storage_drv_t *drv;
    int ret = param_storage_require(PARAM_CAP_SET_ACTIVE, &drv);
    if (ret != PARAM_OK)
        return ret;

    return param_status_from_backend(drv->set_active_partition(drv->ctx, index));
}
/**
 * @brief 按分区索引获取存储后端驱动实例
 *
 * @details
 * 封装 `g_pm.storage->get_partition(ctx, index)` 虚函数调用。
 * 调用者通常随后用 param_set_storage() 切换并 param_load_all()。
 *
 * @note 锁域: 锁内只快照 g_pm.storage 指针, get_partition 在锁外调用 ——
 *       工厂会触发介质 I/O 与后端初始化 (mkv_init 扇区扫描 / iniparser
 *       解析 INI / fdb_kvdb_init), 不该占着参数锁。取到快照后即使并发
 *       param_set_storage() 换掉后端也没问题: 旧驱动在替换后依然有效
 *       (见 param_set_storage 的语义), 且本次调用本就返回旧后端的实例。
 *
 * @param index 分区索引 (PARAM_PARTITION_FACTORY=factory, PARAM_PARTITION_USER_MIN~MAX=用户)
 * @return 驱动指针，存储未初始化或不支持 get_partition 时返回 NULL
 */
const param_storage_drv_t *param_get_storage_partition(uint8_t index)
{
    system_lock(SYS_LOCK_PARAM);
    const param_storage_drv_t *drv = g_pm.initialized ? g_pm.storage : NULL;
    system_unlock(SYS_LOCK_PARAM);

    if (!drv || !drv->get_partition)
        return NULL;

    return drv->get_partition(drv->ctx, index);
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
 * @return PARAM_OK 成功, PARAM_ERR_NOT_SUPPORTED 无存储驱动或后端缺 load/save,
 *         PARAM_ERR_FLASH_FAIL Flash 操作失败,
 *         其他 <0 表示迁移回调返回的致命错误
 */
int param_migrate_storage(const param_storage_drv_t *storage,
                          const param_migrate_entry_t *table,
                          uint16_t count)
{
    if (!storage || !storage->load || !storage->save)
        return PARAM_ERR_NOT_SUPPORTED;

    uint8_t buf[256];
    uint8_t schema_ver;
    uint16_t got = 0;
    int ret;

    /* ---- 阶段 1: 版本检查 ---- */
    ret = storage->load(storage->ctx, PID_SCHEMA_VER, &schema_ver, 1, &got);
    if (ret == PARAM_ERR_NOT_FOUND) {
        /* 首次启动: 无版本键 → 写入当前版本, 跳过迁移 */
        schema_ver = PARAM_SCHEMA_VERSION;
        ret = storage->save(storage->ctx, PID_SCHEMA_VER, &schema_ver, 1);
        return (ret < 0) ? PARAM_ERR_FLASH_FAIL : PARAM_OK;
    }
    if (ret != PARAM_OK)
        return PARAM_ERR_FLASH_FAIL;  /**< 读失败: 无法判定版本, 不动存储 */

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

        /* 尝试加载旧数据: 不存在/已迁移 → 跳过; 读失败或记录超出
           buf 容量 → 中断迁移 (不静默截断, 否则 convert 收到的是残缺数据) */
        uint16_t old_len = 0;
        ret = storage->load(storage->ctx, e->old_id, buf, (uint16_t)sizeof(buf), &old_len);
        if (ret == PARAM_ERR_NOT_FOUND)
            continue;
        if (ret != PARAM_OK)
            return ret;

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

/* ================================================================
 *  load_all 输出端口 — 把 param_cache_restore 包装成 sink
 * ================================================================ */

/**
 * @brief sink 的 restore 实现 — 直接委托 param_cache_restore
 *
 * 框架在 SYS_LOCK_PARAM 内调用本函数 (见 param_restore_sink_t 的契约),
 * 因此此处不再加锁; param_cache_restore 自身也不加锁。
 *
 * @note 本函数是 param_cache_restore 与存储后端之间唯一的桥。后端的
 *       load_all 只持有 sink 指针, 不需要也不得包含 param_manager_internal.h。
 */
static int param_load_restore(void *ctx, uint32_t param_id,
                              const uint8_t *data, uint16_t len)
{
    (void)ctx;

    /* 框架在锁外把 sink 交给后端 (整个 Flash 扫描不在临界区内), 故本处
       自行加锁保护缓存写入; param_cache_restore 本身假定调用者持锁。
       每条记录的提交是原子的, 后端扫描期间其他任务可正常访问参数。 */
    system_lock(SYS_LOCK_PARAM);
    int ret = param_cache_restore(param_id, data, len);
    system_unlock(SYS_LOCK_PARAM);

    return ret;
}

/** @brief 供 load_all 注入的数据出口 (无状态, 可静态共享) */
static const param_restore_sink_t g_load_sink = {
    .ctx = NULL,
    .restore = param_load_restore,
};

/** @brief param_load_all 的遍历体 (兜底逐条加载): 记录首个错误码, 不中断
 *
 * @note 由 param_hash_scan 在锁外调用, 故本函数自行持锁: vtable->load 把
 *       "读介质"与"写缓存"绑在一起, 无法拆分到锁外。持有时间因此收敛到
 *       单条, 而不是整轮 —— 配合 sink 批量通路, 锁外 I/O 成为常态。
 *       (blob_load/string_load 直接把数据读进缓存缓冲, 拆不开。)
 */
static void load_all_visit(param_entry_t *e, void *ctx)
{
    int *first_err = (int *)ctx;

    system_lock(SYS_LOCK_PARAM);
    int ret = e->vtable->load(e);
    system_unlock(SYS_LOCK_PARAM);

    if (ret != PARAM_OK && *first_err == PARAM_OK)
        *first_err = ret;
}

/**
 * @brief 从持久化存储加载所有参数
 *
 * 两阶段执行:
 *   1. LOAD 阶段: 遍历哈希表，对所有 entry 调用 entry->vtable->load()
 *      将存储的值恢复到 entry->cache。仅收集第一个错误码。
 *      (即使某个 entry 加载失败，仍继续加载后续 entry)
 *   2. INIT 阶段: 按链表顺序遍历模块 (即 MODULE_INIT_ORDER)，
 *      对每个模块调用 vtable->init()。
 *
 * 两阶段分离的意义: 确保 init 回调中通过 param_read() 能读到正确的缓存值。
 *
 * 恢复后的生效契约 (全量重初始化模型): 恢复后不逐条 apply/write
 * (批量加载无可用顺序, 逐条校验会产生假拒绝与 N 次副作用)。
 * init 回调是批量校验/下发的唯一入口; 未实现 init 的模块其恢复值
 * 仅回填缓存、不做校验。单参数路径 param_load_one 无 init 阶段,
 * 以 write 回调作为重新校验入口 —— 两者契约互补。
 *
 * @note 存储后端不可用时跳过 LOAD 阶段, INIT 阶段仍执行
 *       (param_init 允许 storage=NULL 的系统依赖此路径完成模块初始化)。
 */
int param_load_all(void)
{
    if (!g_pm.initialized)
        return PARAM_ERR_NOT_INITIALIZED;
    int first_err = PARAM_OK;

    /* 无存储后端时跳过 LOAD 阶段, 但 INIT 阶段仍须执行:
       模块初始化 (标脏/下发默认值) 不依赖持久化。
       param_storage_require 返回 PARAM_OK 即表示"有后端且提供 load 回调" */
    const param_storage_drv_t *drv = NULL;
    if (param_storage_require(PARAM_CAP_LOAD, &drv) == PARAM_OK) {
        /* 第一阶段 (优先, 锁外): 存储后端批量加载 — 一次扫描恢复所有缓存。
           后端只负责把记录逐条推给注入的 sink; sink 内部自行加锁, 因此
           整段 Flash 扫描**不**在临界区内, 也不阻塞其他任务的参数访问。
           返回值: 0 = 全部恢复; >0 = 扫描不完整 (单条记录超出后端扫描
           缓冲, 或 restore 拒绝了某条), 必须走兜底; <0 = 失败 */
        int batch_ret = drv->load_all ? drv->load_all(drv->ctx, &g_load_sink) : 0;

        /* 第一阶段 (兜底, 逐条): 后端未实现批量 / 批量不完整 / 批量失败。
           逐条 load 按条目真实容量读取 (大 BLOB/STRING 也能恢复), 结果权威。
           走 param_hash_scan: 锁内只快照指针, 回调内置锁只包住单条 I/O。 */
        if (!drv->load_all || batch_ret != 0)
            param_hash_scan(load_all_visit, &first_err);
    }

    /* 第二阶段: 按 MODULE_INIT_ORDER 顺序初始化每个模块。
       收集首个 init 失败错误码 (模块 initialized 标志保持 0,
       可由 param_module_is_initialized 查询) */
    param_module_node_t *m = g_pm.module_head;
    while (m) {
        if (m->vtable && m->vtable->init) {
            int ret = m->vtable->init(m);
            if (ret != PARAM_OK && first_err == PARAM_OK)
                first_err = ret;
        }
        m = m->next;
    }

    return first_err;
}

/**
 * @brief 从持久化存储加载单个参数
 *
 * 加载后调用 vtable->write 使业务逻辑 (apply 回调) 对恢复的值
 * 重新校验，确保多分区切换后参数满足当前约束。
 * (单参数路径无 init 阶段, write 即恢复后的重新校验入口 —
 *  与 param_load_all 的批量生效契约互补, 见其文档)
 *
 * @note 此路径直接调用 vtable->write 而非 param_write，
 *       因此不触发 notify 回调。加载/分区切换属于批量恢复操作，
 *       逐参数 notify 会产生噪音，由调用者决定是否在批量加载后
 *       统一通知。
 */
int param_load_one(uint32_t param_id)
{
    int ret = param_storage_require(PARAM_CAP_LOAD, NULL);
    if (ret != PARAM_OK)
        return ret;

    param_entry_t *e = param_entry_find(param_id);
    if (!e)
        return PARAM_ERR_INVALID_ID;

    system_lock(SYS_LOCK_PARAM);
    ret = e->vtable->load(e);
    system_unlock(SYS_LOCK_PARAM);
    /* 恢复值需重新下发硬件: 标脏模块 (此刻未持锁, 可安全走自加锁的公共封装) */
    if (ret == PARAM_OK)
        param_module_dirty_sync(param_id, true);
    /* apply 重新校验恢复值 (多分区兼容); 有意不走 param_write 以避免 notify */
    if (ret == PARAM_OK)
        ret = e->vtable->write(e, *entry_cache(e));

    return ret;
}

/** @brief 删除单个参数的持久化数据 */
int param_delete_one(uint32_t param_id)
{
    const param_storage_drv_t *drv;
    int ret = param_storage_require(PARAM_CAP_DELETE, &drv);
    if (ret != PARAM_OK)
        return ret;

    return param_status_from_backend(drv->delete(drv->ctx, param_id));
}

/** @brief 擦除全部持久化数据 */
int param_delete_all(void)
{
    const param_storage_drv_t *drv;
    int ret = param_storage_require(PARAM_CAP_ERASE_ALL, &drv);
    if (ret != PARAM_OK)
        return ret;

    return param_status_from_backend(drv->erase_all(drv->ctx));
}

/* ================================================================
 *  reset / stats / foreach / range / validate
 * ================================================================ */

/** @brief param_reset_all 的遍历体: 全部条目恢复默认值 */
static void reset_all_visit(param_entry_t *e, void *ctx)
{
    (void)ctx;
    e->vtable->reset(e);
}

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
        return PARAM_ERR_NOT_INITIALIZED;

    system_lock(SYS_LOCK_PARAM);

    param_hash_foreach(reset_all_visit, NULL);

    system_unlock(SYS_LOCK_PARAM);

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
        return PARAM_ERR_NOT_INITIALIZED;

    param_entry_t *e;
    int ret;

    system_lock(SYS_LOCK_PARAM);
    e = param_entry_find(param_id);
    if (!e) {
        system_unlock(SYS_LOCK_PARAM);
        return PARAM_ERR_INVALID_ID;
    }
    ret = e->vtable->reset(e);
    if (ret == PARAM_OK) {
        /* 此处已持 SYS_LOCK_PARAM (reset 与标脏必须原子), 故内联调用模块
           vtable 的 mark_dirty, 而非 param_module_dirty_sync —— 后者内部
           自行加锁, 而端口层是非递归锁, 会自死锁。 */
        param_module_node_t *m = param_module_find(PARAM_MODULE_ID(param_id));
        if (m && m->vtable && m->vtable->mark_dirty)
            m->vtable->mark_dirty(m, PARAM_LOCAL_ID(param_id));
    }
    system_unlock(SYS_LOCK_PARAM);

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
        system_lock(SYS_LOCK_PARAM);
        *stats = g_pm.stats;
        system_unlock(SYS_LOCK_PARAM);
    }
}

/** @brief 清零统计计数器 (dirty/error/miss) */
void param_clear_stats(void)
{
    if (!g_pm.initialized)
        return;
    system_lock(SYS_LOCK_PARAM);
    g_pm.stats.dirty_count = 0;
    g_pm.stats.flush_error_count = 0;
    g_pm.stats.flush_order_miss_count = 0;
    system_unlock(SYS_LOCK_PARAM);
}

/** @brief param_foreach 的分批遍历上下文 */
typedef struct {
    uint16_t module_id;
    param_foreach_fn cb;
    void *user_data;
} foreach_ctx_t;

/** @brief param_foreach 谓词: module_id == 0 表示全部模块 */
static bool foreach_accept(const param_module_node_t *m, void *ctx)
{
    const foreach_ctx_t *f = (const foreach_ctx_t *)ctx;
    return f->module_id == 0 || m->module_id == f->module_id;
}

/** @brief param_foreach 访问体: 逐条目调用 cb, 调用者返 false 则终止整次遍历 */
static bool foreach_visit(param_module_node_t *m, void *ctx)
{
    foreach_ctx_t *f = (foreach_ctx_t *)ctx;

    for (uint16_t k = 0; k < m->param_count; k++) {
        param_entry_t *e = m->table[k];
        if (!e)
            continue;
        if (!f->cb(e, f->user_data))
            return false;
    }
    return true;
}

/**
 * @brief 遍历指定模块或全部模块的参数条目
 *
 * @details
 * 两阶段执行, 用户回调在**锁外**调用 (与 param_flush 同策略):
 *   阶段 1 (锁内): 把目标模块的**指针**快照到栈上数组 (每批
 *     PARAM_MODULE_BATCH_MAX 个, 超出分批续扫);
 *   阶段 2 (锁外): 遍历各模块的 table 调用 cb。
 *     m->table / m->param_count 在注册后不可变, 且条目均为静态对象,
 *     所以锁外遍历安全。
 *
 * 因此回调内可安全调用 param_read / param_write / param_save_one 等
 * 加锁 API, 不会自死锁。
 *
 * @param module_id 模块 ID (0 表示全部)
 * @param cb        回调，返回 false 终止遍历
 * @param user_data 用户上下文
 */
void param_foreach(uint16_t module_id, param_foreach_fn cb, void *user_data)
{
    if (!g_pm.initialized || !cb)
        return;

    foreach_ctx_t fctx = { .module_id = module_id, .cb = cb, .user_data = user_data };
    param_module_scan(foreach_accept, foreach_visit, &fctx);
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
        return PARAM_ERR_NOT_INITIALIZED;
    system_lock(SYS_LOCK_PARAM);
    param_entry_t *e = param_entry_find(param_id);
    if (!e) {
        system_unlock(SYS_LOCK_PARAM);
        return PARAM_ERR_INVALID_ID;
    }

    param_type_t t = entry_type(e);
    if (t != PARAM_TYPE_UINT && t != PARAM_TYPE_INT && t != PARAM_TYPE_FLOAT) {
        system_unlock(SYS_LOCK_PARAM);
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

    system_unlock(SYS_LOCK_PARAM);

    if (clamped)
        return param_write(param_id, new_val);

    return PARAM_OK;
}

/**
 * @brief 对所有已注册参数执行范围裁剪
 *
 * @note 全程持锁, **不**走 param_module_scan 的分批路径: 裁剪直接改写
 *       entry->cache, 若在锁外执行, 与并发 param_write 的
 *       "读缓存 → 裁剪 → 写回" 之间会丢失对方刚写入的值。
 *       此处只做内存内的 clamp (无 I/O), 持锁代价可忽略。
 */
void param_validate_all(void)
{
    if (!g_pm.initialized)
        return;
    system_lock(SYS_LOCK_PARAM);
    param_module_node_t *m = g_pm.module_head;
    while (m) {
        for (uint16_t i = 0; i < m->param_count; i++) {
            param_entry_t *e = m->table[i];
            if (e)
                param_clamp_entry(e);
        }
        m = m->next;
    }
    system_unlock(SYS_LOCK_PARAM);
}

/** @brief param_module_foreach 的分批遍历上下文 */
typedef struct {
    param_module_iter_fn cb;
    void *ctx;
} module_iter_ctx_t;

/** @brief param_module_foreach 访问体: 模块级回调无返回值, 恒继续 */
static bool module_iter_visit(param_module_node_t *m, void *ctx)
{
    module_iter_ctx_t *f = (module_iter_ctx_t *)ctx;
    f->cb(m, f->ctx);
    return true;
}

/**
 * @brief 遍历所有已注册模块节点
 *
 * @details
 * 分批机制见 param_module_scan: 锁内只快照模块指针, 锁外调用 cb。
 * 因此 cb 内可安全调用 param_* 加锁 API。
 */
void param_module_foreach(param_module_iter_fn cb, void *ctx)
{
    if (!g_pm.initialized || !cb)
        return;

    module_iter_ctx_t fctx = { .cb = cb, .ctx = ctx };
    param_module_scan(NULL, module_iter_visit, &fctx);
}

#if PARAM_MODULE_AUTO_REGISTER_ENABLE

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
