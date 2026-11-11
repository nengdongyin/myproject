/**
 * @file param_manager.c
 * @brief 参数管理核心框架实现 — 生命周期、哈希表、模块注册、读写分派
 *        以及 flush/save/load/reset 的运行时实现
 *
 * 运行时状态集中在单例 g_pm (模块链表 + 参数哈希表 + 存储驱动 + 统计),
 * 各字段含义见其自身定义。架构分层、两级 vtable 与锁纪律分别见
 * param_manager.h / param_manager_internal.h。
 */

#include "param_manager.h"
#include "param_manager_internal.h"
#include "port.h"
#include "param_data_ops.h"
#include "module_ids.h"
#include <string.h>

/**
 * @brief 模块初始化顺序表 (编译期常量，来自 MODULE_INIT_ORDER)
 *
 * MODULE_ORDER_COUNT 与本表同源于 PARAM_MODULE_LIST, 与 module_ids.h 的
 * PARAM_MODULE_COUNT 恒等: 二者都按"清单行数"计算, 不依赖任何 ID 数值。
 * 强制转 uint16_t: sizeof 结果为 size_t, 与 uint16_t 循环变量混比会在
 * -Wextra / -Wconversion 下告警。
 */
static const uint16_t g_module_order[] = {MODULE_INIT_ORDER};
#define MODULE_ORDER_COUNT \
    ((uint16_t)(sizeof(g_module_order) / sizeof(g_module_order[0])))

_Static_assert(PARAM_MODULE_COUNT ==
                   ((uint16_t)(sizeof(g_module_order) / sizeof(g_module_order[0]))),
               "MODULE_ORDER_COUNT 与 PARAM_MODULE_COUNT 同源, 不得分叉");

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

/* 存储格式版本键的补齐入口 — 后端无关, 只经 drv->load / drv->save。
   只在**落盘**时调用 (param_save_all / param_save_one), 定义见
   param_migrate_storage 附近。 */
static void param_schema_version_ensure(const param_storage_drv_t *drv);

/**
 * @brief 运行时替换持久化后端驱动 — 纯指针替换
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
 */
/**
 * @brief 运行时替换持久化后端驱动 — 纯指针替换
 *
 * 仅替换 g_pm.storage 指针，不触发旧驱动的 deinit，不触发 save/load。
 * 旧驱动在替换后仍然有效，可保存指针并在之后切回。
 *
 * @param storage 新的存储后端驱动 (可为 NULL 停用持久化)
 * @return PARAM_OK 成功; PARAM_ERR_NOT_INITIALIZED 框架未初始化;
 *         PARAM_ERR_NOT_SUPPORTED 驱动缺 load/save 回调 (被拒绝,
 *         调用方可直接依赖返回值确认切换生效)
 */
int param_set_storage(const param_storage_drv_t *storage)
{
    if (!g_pm.initialized) {
        return PARAM_ERR_NOT_INITIALIZED;
    }
    if (storage && (!storage->load || !storage->save)) {
        return PARAM_ERR_NOT_SUPPORTED;
    }
    g_pm.storage = storage;
    return PARAM_OK;
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

/**
 * @brief 清空哈希表中指向指定条目的槽位 (注册回滚用)
 *
 * @attention 开放寻址下直接置 NULL 会打断探测链 —— 必须配合
 *   hash_rebuild 整表重建, 否则插在该槽之后的同链条目会查不到。
 */
static void hash_remove(param_entry_t *entry)
{
    for (uint32_t s = 0; s < PARAM_HASH_SIZE; s++) {
        if (g_pm.hash[s] == entry) {
            g_pm.hash[s] = NULL;
            return;
        }
    }
}

/**
 * @brief 哈希表整表重建 — 收集非空槽后清表重插 (注册回滚用)
 *
 * 删除槽位后重建探测链, 恢复开放寻址不变量。仅在注册失败的
 * 罕见路径上执行, O(N) 代价可接受。
 * 收集缓冲为静态 (PARAM_HASH_SIZE 个指针): 不占调用线程栈
 * (嵌入式小栈友好); 调用期间持有 SYS_LOCK_PARAM 且注册路径
 * 单线程, 无重入。
 */
static void hash_rebuild(void)
{
    static param_entry_t *keep[PARAM_HASH_SIZE];
    uint32_t n = 0;
    for (uint32_t s = 0; s < PARAM_HASH_SIZE; s++) {
        if (g_pm.hash[s]) {
            keep[n++] = g_pm.hash[s];
            g_pm.hash[s] = NULL;
        }
    }
    for (uint32_t k = 0; k < n; k++)
        hash_insert(keep[k]);
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
 * 只此一层, 不另设转发包装。
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
 * 仅存储驱动指针 —— 驱动由工厂函数预初始化 (见 param_storage_*_create)。
 * @note 假定启动阶段单线程调用, 故不加锁; 公共契约见头文件同名声明。
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
 *  模块注册 — param_module_register_node() 为通用入口 (App/IP 共用),
 *  param_module_register() 为 App 侧便捷包装。流程见函数注释。
 * ================================================================ */

/**
 * @brief 通用模块注册入口 — 将模块节点及参数表注册到框架
 *
 * @details
 * 全程持锁完成五步: module_id 去重 → 逐条 param_id 查重 → 哈希插入
 * (开放寻址 + 线性探测) → 按 MODULE_INIT_ORDER 插入有序链表 → 更新统计。
 * 任一步失败即返回, 不继续后续步骤。
 *
 * App 与 IP 模块共用本入口, 不存在双轨注册; 调用前必须已设置
 * node->vtable 与 node->module_id。
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
    /* vtable 必须已由 DEFINE 宏写入: 注册不代选 App/IP 类型 ——
       手工构造 node 而漏设 vtable 属配置错误, 在此拦截 (防御
       "注册成功但后续 vtable 调用野指针")。 */
    if (!node->vtable)
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

    /* 步骤 3: 参数插入哈希表 (失败时回滚已插入条目并整表重建 ——
       否则中途失败会污染注册表: 条目留在表中而模块不在链表,
       重试注册永远撞 PARAM_ERR_ALREADY_REG) */
    for (uint16_t i = 0; i < count; i++) {
        if (!entries[i])
            continue;
        if (!hash_insert(entries[i])) {
            for (uint16_t k = 0; k < i; k++) {
                if (entries[k])
                    hash_remove(entries[k]);
            }
            hash_rebuild();
            node->table = NULL;
            node->param_count = 0;
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
 *  其余槽位 (read / write / write_sync / write_raw / load /
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
        return param_entry_is_exec(e) ? PARAM_ERR_EXEC_ONLY : PARAM_ERR_TYPE_MISMATCH;

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
 * App 专用槽位。回调返回非 OK 时**缓存无变化**:
 *   - 值类型: 缓存先行 (回调内 param_read / param_save_one 反映新值),
 *     拒绝时回滚到先行快照;
 *   - BLOB/STRING: 回调先行 (新 payload 经 value.ptr 传递, 缓存保持旧值),
 *     接受后才提交字节级拷贝 —— 单缓冲配置没有备份缓冲, 字节级回滚
 *     不可行, 以"先验后写"达成同一契约: 返回错误 = 缓存未变。
 */
int param_entry_write_apply(param_entry_t *e, param_value_t value)
{
    param_module_t *m = param_entry_module(e);
    if (!m) return PARAM_ERR_NOT_FOUND;

    param_type_t t = entry_type(e);
    if (t >= PARAM_TYPE_COUNT) return PARAM_ERR_TYPE_MISMATCH;

    if (entry_flags(e) & PARAM_FLAG_EXEC)
        return param_entry_is_exec(e) ? PARAM_ERR_EXEC_ONLY : PARAM_ERR_TYPE_MISMATCH;

    if (entry_flags(e) & PARAM_FLAG_READONLY) return PARAM_ERR_READONLY;

    if (!g_param_pre_write[t](e, &value)) return PARAM_ERR_OUT_OF_RANGE;

    bool is_ptr = (t == PARAM_TYPE_BLOB || t == PARAM_TYPE_STRING);
    param_value_t old_cache;

    if (!is_ptr) {
        system_lock(SYS_LOCK_PARAM);
        old_cache = *entry_cache(e);
        g_param_data_ops[t].cache_update(e, value);
        system_unlock(SYS_LOCK_PARAM);
    }

    if (m->write) {
        int ret = m->write(m->ctx, e->param_id, value);
        if (ret != PARAM_OK) {
            if (!is_ptr) {
                system_lock(SYS_LOCK_PARAM);
                *entry_cache_ptr(e) = old_cache;
                system_unlock(SYS_LOCK_PARAM);
            }
            return ret;
        }
    }

    if (is_ptr) {
        system_lock(SYS_LOCK_PARAM);
        g_param_data_ops[t].cache_update(e, value);
        system_unlock(SYS_LOCK_PARAM);
    }

    system_lock(SYS_LOCK_PARAM);
    param_entry_mark_dirty(e);
    system_unlock(SYS_LOCK_PARAM);

    return PARAM_OK;
}

/**
 * @brief 同步写入 (IP 槽位) — EXEC 路由 / READONLY 过滤 / 缓存提交 + 直通硬件
 *
 * IP 参数: write 回调存在时直通硬件, 成功后清脏 (值已生效, 无需 flush
 * 重试); 回调缺失时只到缓存层并清脏 (与 param_write_sync 的公开契约一致,
 * write 是可选钩子, 不构成错误)。
 *
 * 缓存先行 (IP 不回滚): 硬件写失败时缓存保持新值、脏位保留,
 * 由 param_flush 重试收敛 (flush 重试模型需要)。
 *
 * @param e     参数条目
 * @param value 待写入值 (pre_write 已拒绝超范围, 不做裁剪)
 * @return PARAM_OK 成功, 或其他错误码
 */
int param_entry_write_sync(param_entry_t *e, param_value_t value)
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

    /* 缓存先行 (IP 有意不回滚): 回调执行期间缓存已为新值;
       硬件写失败保留脏位, 由 flush 重试收敛 */
    system_lock(SYS_LOCK_PARAM);
    g_param_data_ops[t].cache_update(e, value);
    system_unlock(SYS_LOCK_PARAM);

    /* write 为可选钩子: 缺失时"同步"只到缓存层, 不报错 */
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

/**
 * @brief 同步写入 (App 槽位) — EXEC 路由 + apply 校验 + 标脏
 *
 * App 参数: "同步" = 立即执行 apply 校验并**标脏**, 硬件由
 * param_flush 按 dirty 下发 (与 param_write 一致, 值在下一轮 flush
 * 到达硬件)。
 *
 * EXEC 路由与 IP 侧相同 (命令与缓存/脏位无关)。
 *
 * @param e     参数条目
 * @param value 待写入值
 * @return PARAM_OK 成功, 或其他错误码
 */
int param_entry_write_sync_apply(param_entry_t *e, param_value_t value)
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

    /* apply 校验 + 标脏 (拒绝回滚) —— 与 param_write 同语义 */
    return param_entry_write_apply(e, value);
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
        if (!be->cache.ptr || be->blob_size == 0) return PARAM_ERR_NOT_FOUND;
        if (len != be->blob_size) return PARAM_ERR_OUT_OF_RANGE;
        /* 回调先行 (与 STRING 同构): 新 payload 经 .ptr 传递,
           缓存保持旧值, apply 拒绝时无字节级污染; 接受后 write_apply
           提交整块拷贝 (len 已校验等于 blob_size, 无越界读)。 */
        param_value_t value = { .ptr = (void *)data };
        return write_fn(e, value);
    }
    if (t == PARAM_TYPE_STRING) {
        /* 回调先行: 新 payload 经 .ptr 传递, 缓存保持旧值, apply
           拒绝时无字节级污染。接受后 write_apply 会以 cache_update_string
           提交 (memchr 限界 ≤ max_len+1, 无越界读); 为精确尊重 len 界
           (调用方缓冲在 len 内无 '\0' 时不得越过 len 读取), 成功后本函数
           再做一次 len 限界拷贝覆盖 (代价 ≤ max_len 字节, 仅本路径)。 */
        param_string_entry_t *se = (param_string_entry_t *)e;
        if (!se->cache.ptr || se->max_len == 0) return PARAM_ERR_NOT_FOUND;

        param_value_t value = { .ptr = (void *)data };
        int ret = write_fn(e, value);
        if (ret == PARAM_OK) {
            uint16_t n = (len > se->max_len) ? se->max_len : len;
            system_lock(SYS_LOCK_PARAM);
            if (se->cache.ptr != (const void *)data)
                memcpy(se->cache.ptr, data, n);
            ((char *)se->cache.ptr)[n] = '\0';
            system_unlock(SYS_LOCK_PARAM);
        }
        return ret;
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
 * pre_write 对超范围值拒绝, 但 apply 回调仍可能修正值,
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
 * param_write / param_write_cache / param_write_sync / param_write_raw
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
           param_write_string / param_write_raw */
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
 *       write_sync / write_raw)
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

/** @brief 同步写入参数 (App: apply+标脏; IP: 直通硬件+清脏) */
int param_write_sync(uint32_t param_id, param_value_t value)
{
    param_entry_t *e = NULL;
    int ret = param_write_resolve(param_id, &e, true);
    if (ret != PARAM_OK)
        return ret;

    ret = e->vtable->write_sync(e, value);
    if (ret != PARAM_OK)
        return ret;

    if (param_entry_is_exec(e)) {
        /* EXEC 无缓存可读: 通知携带命令负载 (与 exec 回调收到的
           arg 一致的 value 本体) */
        if (g_pm.notify_cb)
            g_pm.notify_cb(param_id, value);
        return PARAM_OK;
    }

    /* 模块脏位镜像条目脏位: App 侧标脏 (值等 flush 下发), IP 侧清脏
       (write 回调已直通硬件) —— 两侧差异由各自的 write_sync 槽位
       实现完成, 此处按条目结果统一镜像 */
    param_module_dirty_sync(param_id, entry_dirty(e) != 0);
    param_notify_effective(param_id, e);
    return PARAM_OK;
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
           "生效值": 通知携带命令负载, 与 exec 回调收到的 arg 一致
           (param_entry_write_raw_to 的 EXEC 分支恒以 {.ptr = data}
           传给回调, 此处保持同一形状)。
           注: 此分支不调 param_module_dirty_sync —— EXEC 条目被模块级
           mark_dirty 的可写过滤挡掉, 该调用恒为空操作。 */
        param_value_t arg = { .ptr = (void *)data };
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

/**
 * @brief 只读权威源: 直接走模块 read 回调, 不回退缓存
 *
 * 与 param_read 的区别: param_read 在回调缺失/失败时静默返回缓存,
 * 调用者无法区分"寄存器实时值"与"旧缓存"; 本函数只信权威源,
 * 取不到即报错, 诊断场景 (确认硬件真实状态) 用。
 *
 * @note 回调成功时与 param_read 一样回写缓存。
 * @return PARAM_OK 已从权威源读取并回写缓存;
 *         PARAM_ERR_NOT_FOUND 模块不存在或无 read 回调;
 *         其他错误码原样上抛 (回调的失败原因)
 */
int param_read_hw(uint32_t param_id, param_value_t *value)
{
    if (!g_pm.initialized)
        return PARAM_ERR_NOT_INITIALIZED;
    if (!value)
        return PARAM_ERR_INVALID_ID;

    param_entry_t *e = param_entry_find(param_id);
    if (!e)
        return PARAM_ERR_INVALID_ID;

    param_module_t *m = param_entry_module(e);
    if (!m || !m->read)
        return PARAM_ERR_NOT_FOUND;

    memset(value, 0, sizeof(*value));
    int ret = m->read(m->ctx, e->param_id, value);
    if (ret != PARAM_OK)
        return ret;

    param_type_t t = entry_type(e);
    if (t >= PARAM_TYPE_COUNT)
        return PARAM_ERR_TYPE_MISMATCH;
    system_lock(SYS_LOCK_PARAM);
    g_param_data_ops[t].cache_update(e, *value);
    system_unlock(SYS_LOCK_PARAM);
    return PARAM_OK;
}

/**
 * @brief 维护路径的"裁剪后重放" — 数值类型先裁剪再走 vtable->write
 *
 * 语义分界: 用户直写路径 (param_write 系列) 对超范围值
 * **拒绝**; 维护路径 (param_reapply / param_load_one / param_reset_one)
 * 的输入是"已在缓存中的旧值", 可能因外部范围收紧 (param_set_range)
 * 而越界 —— 裁剪而非拒绝才能让缓存收敛到合法状态。
 *
 * 裁剪发生在局部副本上, 不直接改缓存 (与 param_clamp_entry 区分)。
 * BLOB/STRING 与 ENUM 无裁剪语义, 原值重放。
 */
static int param_replay_clamped(param_entry_t *e, param_value_t v)
{
    param_type_t t = entry_type(e);
    if (t == PARAM_TYPE_UINT || t == PARAM_TYPE_INT || t == PARAM_TYPE_FLOAT) {
        param_range_entry_t *re = (param_range_entry_t *)e;
        v = param_clamp_value_to_range(re, v);
    }
    return e->vtable->write(e, v);
}

int param_reapply(uint32_t param_id)
{
    if (!g_pm.initialized)
        return PARAM_ERR_NOT_INITIALIZED;

    param_entry_t *e = param_entry_find(param_id);
    if (!e)
        return PARAM_ERR_INVALID_ID;

    /* 先取缓存快照 (param_read_cache 锁内), 再重放完整写入路径
       (pre_write → apply → cache_update → dirty → notify)。
       注意: 快照与 write 之间非原子, 并发写者可能覆盖此值。
       直接走 vtable 而非 param_write: 后者拒绝 BLOB/STRING, 而本
       函数必须支持它们 (以缓存自身为源的重放, 不涉外部指针)。 */
    param_value_t cached;
    int ret = param_read_cache(param_id, &cached);
    if (ret != PARAM_OK)
        return ret;

    /* 维护语义: 越界旧值裁剪收敛而非拒绝 */
    ret = param_replay_clamped(e, cached);
    if (ret != PARAM_OK)
        return ret;

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

/**
 * @brief typed 包装的公共类型校验 — 目标条目必须与期望类型匹配
 *
 * typed 包装给调用者的类型安全保证是真实的 —— 对 FLOAT 条目调
 * param_write_u32 会把位模式当 float 存进去, 再 param_read_f32 读出
 * 垃圾。因此先校验 entry_type, 不符即 PARAM_ERR_TYPE_MISMATCH。
 *
 * @note ENUM 与 INT 共用 .i32 表示且共用 cache_update, 因此
 *       param_write_i32 / param_read_i32 对两者都放行。
 */
static int param_typed_resolve(uint32_t id, param_type_t expected)
{
    if (!g_pm.initialized)
        return PARAM_ERR_NOT_INITIALIZED;

    param_entry_t *e = param_entry_find(id);
    if (!e)
        return PARAM_ERR_INVALID_ID;

    param_type_t t = entry_type(e);
    if (t != expected && !(expected == PARAM_TYPE_INT && t == PARAM_TYPE_ENUM))
        return PARAM_ERR_TYPE_MISMATCH;

    return PARAM_OK;
}

/** @brief uint32_t 类型写入快捷函数 */
int param_write_u32(uint32_t id, uint32_t val)
{
    int ret = param_typed_resolve(id, PARAM_TYPE_UINT);
    if (ret != PARAM_OK)
        return ret;
    param_value_t v = {.u32 = val};
    return param_write(id, v);
}
/** @brief int32_t 类型写入快捷函数 (ENUM 条目同样接受) */
int param_write_i32(uint32_t id, int32_t val)
{
    int ret = param_typed_resolve(id, PARAM_TYPE_INT);
    if (ret != PARAM_OK)
        return ret;
    param_value_t v = {.i32 = val};
    return param_write(id, v);
}
/** @brief float 类型写入快捷函数 */
int param_write_f32(uint32_t id, float val)
{
    int ret = param_typed_resolve(id, PARAM_TYPE_FLOAT);
    if (ret != PARAM_OK)
        return ret;
    param_value_t v = {.f32 = val};
    return param_write(id, v);
}
/** @brief bool 类型写入快捷函数 */
int param_write_bool(uint32_t id, bool val)
{
    int ret = param_typed_resolve(id, PARAM_TYPE_BOOL);
    if (ret != PARAM_OK)
        return ret;
    param_value_t v = {.b = val};
    return param_write(id, v);
}

/** @brief uint32_t 类型读取快捷函数 */
int param_read_u32(uint32_t id, uint32_t *val)
{
    if (!val)
        return PARAM_ERR_INVALID_ID;
    int ret = param_typed_resolve(id, PARAM_TYPE_UINT);
    if (ret != PARAM_OK)
        return ret;
    param_value_t v;
    ret = param_read(id, &v);
    if (ret == PARAM_OK)
        *val = v.u32;
    return ret;
}
/** @brief int32_t 类型读取快捷函数 (ENUM 条目同样接受) */
int param_read_i32(uint32_t id, int32_t *val)
{
    if (!val)
        return PARAM_ERR_INVALID_ID;
    int ret = param_typed_resolve(id, PARAM_TYPE_INT);
    if (ret != PARAM_OK)
        return ret;
    param_value_t v;
    ret = param_read(id, &v);
    if (ret == PARAM_OK)
        *val = v.i32;
    return ret;
}
/** @brief float 类型读取快捷函数 */
int param_read_f32(uint32_t id, float *val)
{
    if (!val)
        return PARAM_ERR_INVALID_ID;
    int ret = param_typed_resolve(id, PARAM_TYPE_FLOAT);
    if (ret != PARAM_OK)
        return ret;
    param_value_t v;
    ret = param_read(id, &v);
    if (ret == PARAM_OK)
        *val = v.f32;
    return ret;
}
/** @brief bool 类型读取快捷函数 */
int param_read_bool(uint32_t id, bool *val)
{
    if (!val)
        return PARAM_ERR_INVALID_ID;
    int ret = param_typed_resolve(id, PARAM_TYPE_BOOL);
    if (ret != PARAM_OK)
        return ret;
    param_value_t v;
    ret = param_read(id, &v);
    if (ret == PARAM_OK)
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
 * @brief 带长度 typed 入口的公共尾巴 — 源已校验
 *
 * 与 param_write 的差异: 不做 "union 无长度类型" 的拒绕 —— 调用者已用
 * 专用签名给出长度并校验过源指针。value 传调用者源缓冲, 由 write_apply
 * 在 apply 接受后提交字节级拷贝 (拒绝时缓存未被污染); IP 侧
 * param_entry_write 则立即提交。
 *
 * @note 接受后 notify 收到 BLOB/STRING 的 .ptr 为缓存自身指针
 *       (生命周期覆盖整个运行期), 调用者的瞬态缓冲不参与。
 */
static int param_write_typed_tail(uint32_t param_id, param_entry_t *e, param_value_t value)
{
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

    /* 拷贝由 write_apply 在 apply 接受后提交, 本入口只校验源:
       源缓冲必须是 '\0' 终止的 C 串 (cache_update_string 有界扫描
       max_len+1 字节, 超 max_len 自动截断并补 '\0') */
    int ret = param_write_typed_tail(id, e, (param_value_t){ .ptr = (void *)str });

    /* 截断可感知: 源串在 max_len+1 窗口内无 '\0' 即超长被截断。
       返回正码 PARAM_OK_TRUNCATED: ret < 0 的失败判定不受影响,
       严格判定用 ret == PARAM_OK */
    if (ret == PARAM_OK &&
        memchr(str, '\0', (size_t)se->max_len + 1) == NULL)
        ret = PARAM_OK_TRUNCATED;
    return ret;
}

/**
 * @brief BLOB 写入统一经 param_write_raw (见其 BLOB 分支)
 *
 * len 必须等于 blob_size, 否则 PARAM_ERR_OUT_OF_RANGE (不截断/不补零);
 * apply 接受后整块拷入缓存 (先验后写, 拒绝时缓存未变)。
 */

/**
 * @brief 带策略旗标的写入入口 (高级)
 *
 * 与 param_write 相同但可携带写策略旗标 (param_write_flags_t)。
 * 未定义旗标位返回 PARAM_ERR_NOT_SUPPORTED (拒绝而非静默忽略)。
 * 当前唯一旗标 PARAM_WR_CLAMP: 超范围值裁剪到合法区间而非拒绝
 * (显式裁剪旗标, 仅 UINT/INT/FLOAT 生效)。
 */
int param_write_ex(uint32_t param_id, param_value_t value, uint32_t flags)
{
    if (flags & ~(uint32_t)PARAM_WR_CLAMP)
        return PARAM_ERR_NOT_SUPPORTED;

    param_entry_t *e = NULL;
    int ret = param_write_resolve(param_id, &e, true);
    if (ret != PARAM_OK)
        return ret;

    if (flags & PARAM_WR_CLAMP) {
        param_type_t t = entry_type(e);
        if (t == PARAM_TYPE_UINT || t == PARAM_TYPE_INT || t == PARAM_TYPE_FLOAT) {
            param_range_entry_t *re = (param_range_entry_t *)e;
            value = param_clamp_value_to_range(re, value);
        }
    }

    ret = e->vtable->write(e, value);
    if (ret == PARAM_OK)
        param_write_complete(param_id, e, true);
    return ret;
}

/**
 * @brief 带策略旗标的原始字节流写入 (高级)
 *
 * 对 BLOB/STRING/EXEC: 与 param_write_raw 完全等价 (flags 必须为 0)。
 * 对值类型: 解码字节为 param_value_t 后走 param_write_ex
 * (支持 PARAM_WR_CLAMP)。
 */
int param_write_bytes(uint32_t param_id, const uint8_t *data, uint16_t len,
                      uint32_t flags)
{
    if (!g_pm.initialized)
        return PARAM_ERR_NOT_INITIALIZED;
    if (!data || len == 0)
        return PARAM_ERR_INVALID_ID;

    param_entry_t *e = param_entry_find(param_id);
    if (!e)
        return PARAM_ERR_INVALID_ID;

    param_type_t t = entry_type(e);
    if (t == PARAM_TYPE_BLOB || t == PARAM_TYPE_STRING || t == PARAM_TYPE_EXEC) {
        if (flags != 0)
            return PARAM_ERR_NOT_SUPPORTED;
        return param_write_raw(param_id, data, len);
    }

    if (len > sizeof(param_value_t))
        return PARAM_ERR_TYPE_MISMATCH;
    param_value_t v;
    memset(&v, 0, sizeof(v));
    memcpy(&v, data, len);
    return param_write_ex(param_id, v, flags);
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
 *
 * @note 执行成功后触发 notify: new_value 携带命令负载
 *       {.ptr = user_arg}, 与 exec 回调收到的 arg 完全一致。
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
    int ret = m->vtable->exec(m, cmd_id, arg);
    if (ret == PARAM_OK && g_pm.notify_cb)
        g_pm.notify_cb(cmd_id, arg);
    return ret;
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
 * 三段式: 锁内抢占单 flusher (重入返回 PARAM_ERR_BUSY) → 分批快照模块指针、
 * 在**锁外**调 vtable->flush (慢速总线写不阻塞其他任务的参数访问) → 锁内
 * 回写统计并释放 flusher。单模块失败不中断整轮, 失败计数饱和累加。
 *
 * @note 脏位 take-and-clear: 阶段 2 期间新置的脏位留给下一轮, 不会被吞。
 *       失败重试语义与锁粒度见头文件同名声明。
 * @return PARAM_OK 或最后一个失败的错误码; PARAM_ERR_BUSY 已有 flush 在进行
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
int param_check_module_order(void)
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

    /* 全量保存前确保版本键就位 (缺则补, 已有不动)。
       @note 位置在 begin **之后**: 批量后端下这一步只改内存 (iniparser 的
             defer_dump), 由 commit 与其他条目一起落盘 —— 既避免首次保存
             写两遍文件, 也让"begin 失败即完全不写介质"成立。
       @note 必须在锁内: 后端内部状态靠 SYS_LOCK_PARAM 串行化。 */
    system_lock(SYS_LOCK_PARAM);
    param_schema_version_ensure(drv);
    system_unlock(SYS_LOCK_PARAM);

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
    const param_storage_drv_t *drv = NULL;
    int ret = param_storage_require(PARAM_CAP_SAVE, &drv);
    if (ret != PARAM_OK)
        return ret;

    param_entry_t *e = param_entry_find(param_id);
    if (!e)
        return PARAM_ERR_INVALID_ID;

    system_lock(SYS_LOCK_PARAM);
    /* 本分区的首次落盘 → 顺手补齐存储格式版本键 (缺则补, 已有则一次读就返回)。
       必须在锁内: 后端内部状态没有自己的锁, SYS_LOCK_PARAM 是唯一串行化来源。
       @note 已有键时本调用只多一次 drv->load (字典查找级), 不产生写。 */
    param_schema_version_ensure(drv);
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

    int err = drv->get_active_partition(drv->ctx, index);
    return param_status_from_backend(err);
}

int param_storage_set_active_partition(uint8_t index)
{
    const param_storage_drv_t *drv;
    int ret = param_storage_require(PARAM_CAP_SET_ACTIVE, &drv);
    if (ret != PARAM_OK)
        return ret;

    int err = drv->set_active_partition(drv->ctx, index);
    return param_status_from_backend(err);
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
 * @brief 读取指定后端的存储格式版本号 — 三态探测 (见头文件同名声明)
 *
 * @note 这是版本号读取的**唯一实现**: param_migrate_storage 阶段 1、
 *       param_schema_version_ensure、热切换前的版本探测均走本函数。
 *       三份逻辑若各写一份, 宽度 (4 字节) 或三态语义极易漂移。
 */
int param_schema_version_read(const param_storage_drv_t *drv, uint32_t *version)
{
    if (!drv || !drv->load || !version)
        return PARAM_ERR_NOT_SUPPORTED;

    uint32_t ver = 0;
    uint16_t got = 0;
    int ret = drv->load(drv->ctx, PID_SCHEMA_VER, (uint8_t *)&ver,
                        (uint16_t)sizeof(ver), &got);
    if (ret != PARAM_OK)
        return ret;  /* NOT_FOUND 原样上抛; 其余为读失败 */

    *version = ver;
    return PARAM_OK;
}

/**
 * @brief 确保指定分区已带存储格式版本键 (PID_SCHEMA_VER) — 缺则补写当前版本
 *
 * @details
 * 版本号既是“存储格式的自描述”, 也是“该分区已被本框架初始化过”的标记。
 * param_migrate_storage() 只在启动阶段对**当时激活的那个**分区处理一次;
 * 运行期热切换出来的分区 (param_get_storage_partition → 后端工厂) 不会经过它,
 * 于是那个分区会缺版本键 —— 这就是“param_user0.ini 里没有 0x00000000”的成因。
 *
 * 本函数是“补齐”的**唯一实现**, 故意放在框架层而不是后端层:
 *   - 只经 drv->load / drv->save, 后端无需认识 PID_SCHEMA_VER, 也无需
 *     引入 module_ids.h (若放后端, 三个后端要各改一份);
 *   - 与 param_migrate_storage / value_type_save 共用同一宽度 (4 字节);
 *   - 自带条目名解析 (后端 make_value 会查到 schema_ver), 名字依旧可读。
 *
 * 只在**确实读不到键**时写: 读失败 (记录损坏) 不动存储 —— 与迁移阶段 1
 * 同策略, 避免用当前版本号覆盖一个损坏但可能可修复的分区。
 *
 * @note 调用点只有两处: **两个落盘入口** param_save_all() 与 param_save_one()。
 *       切换分区 (param_set_storage) **不**补写 —— 原因有两个, 都很硬:
 *         1. 一个只有版本键的文件会被后端的 dict_is_empty() 判为非空,
 *            从而绕开 param_storage_iniparser_create() 的“空分区回退 factory”;
 *         2. 版本键的“存在性”本身就是后端/迁移判定“已初始化”的信号。
 *       同理也**不能**放进 param_init(): 那会让迁移阶段 1 误判“首次启动”。
 *
 * @note 放在两个落盘入口的效果: 切到新分区不产生任何文件;
 *       首次 save 时随那次落盘一起把键补上 (之后每次 save 只多一次
 *       drv->load, 到底层是字典查找级, 不产生写)。
 *
 * @param drv 目标后端 (需同时具备 load/save)
 */
static void param_schema_version_ensure(const param_storage_drv_t *drv)
{
    if (!drv || !drv->save)
        return;

    uint32_t ver = 0;
    if (param_schema_version_read(drv, &ver) != PARAM_ERR_NOT_FOUND)
        return;  /* 已有键 或 读失败 → 不动存储 */

    ver = PARAM_SCHEMA_VERSION;
    (void)drv->save(drv->ctx, PID_SCHEMA_VER,
                    (const uint8_t *)&ver, (uint16_t)sizeof(ver));
}

/**
 * @brief 执行参数版本迁移（固件升级时自动转换旧参数格式）
 *
 * @details
 * 迁移流程分为三个阶段:
 *
 *   **阶段 1 — 版本检查:**
 *   从存储中读取 PID_SCHEMA_VER（4 字节, 与 PARAM_SCHEMA_VERSION 同宽）:
 *     - 返回 PARAM_ERR_NOT_FOUND: 首次启动 → 写入当前版本号，跳过迁移
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
    /* 版本号按 param_value_t 的宽度 (4 字节) 存取 ——
       注册参数 schema_ver (core/param_service.c) 的 vtable->save 写的是
       sizeof(param_value_t), 两端宽度必须一致: 若此处仍用 uint8_t + len=1 读,
       后端的 hex_decode 会因记录长度 (4) 超出目标容量 (1) 返回
       PARAM_ERR_OUT_OF_RANGE, 本函数随即判为 PARAM_ERR_FLASH_FAIL,
       初始化直接失败且没有 erase 兜底。
       schema_ver 必须初始化为 0: 短记录 (旧格式 1 字节) 只会回填首字节。 */
    uint32_t schema_ver = 0;
    int ret;

    /* ---- 阶段 1: 版本检查 (三态: OK / NOT_FOUND / 读失败) ---- */
    ret = param_schema_version_read(storage, &schema_ver);
    if (ret == PARAM_ERR_NOT_FOUND) {
        /* 首次启动: 无版本键 → 写入当前版本, 跳过迁移 */
        schema_ver = PARAM_SCHEMA_VERSION;
        int err = storage->save(storage->ctx, PID_SCHEMA_VER,
                                (const uint8_t *)&schema_ver, (uint16_t)sizeof(schema_ver));
        return param_status_from_backend(err);
    }
    if (ret != PARAM_OK)
        return PARAM_ERR_FLASH_FAIL;  /**< 读失败: 无法判定版本, 不动存储 */

    if (schema_ver != PARAM_SCHEMA_VERSION) {
        /* 存储格式破坏性变更 → 全量擦除, 写版本号, 跳过迁移 */
        if (storage->erase_all) {
            int err = storage->erase_all(storage->ctx);
            if (param_status_from_backend(err) != PARAM_OK)
                return PARAM_ERR_FLASH_FAIL;  /**< 擦不掉就不能声称已重建 */
        }
        schema_ver = PARAM_SCHEMA_VERSION;
        int err = storage->save(storage->ctx, PID_SCHEMA_VER,
                                (const uint8_t *)&schema_ver, (uint16_t)sizeof(schema_ver));
        return param_status_from_backend(err);
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
                int err = storage->save(storage->ctx, new_id, buf, new_len);
                saved = (param_status_from_backend(err) == PARAM_OK);
            }
        } else {
            /* 简单改名: 数据不变，仅变更 ID */
            int err = storage->save(storage->ctx, e->new_id, buf, old_len);
            saved = (param_status_from_backend(err) == PARAM_OK);
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
 * 框架在锁**外**调用本函数 (load_all 的批量通路段不持全局锁), 故此处
 * 自行加锁包住缓存写入; param_cache_restore 本身假定调用者已持锁。
 * 每条记录的提交是原子的, 后端扫描期间其他任务可正常访问参数。
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
 * LOAD 阶段恢复缓存 (优先走后端批量通路 sink, 不完整时回退逐条 load),
 * INIT 阶段按 MODULE_INIT_ORDER 调用各模块 init —— 两者分离是为了让 init
 * 中的 param_read() 能读到已恢复的完整缓存。
 *
 * @note 恢复后的生效契约 (不逐条 apply/write, init 为唯一校验入口) 见头文件
 *       同名声明; 无存储后端时跳过 LOAD 阶段, INIT 照常执行。
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
 * @note 恢复值可能因范围收紧 (param_set_range) 而越界: 重放前先
 *       裁剪收敛 (维护语义, 用户直写路径才拒绝)。
 * @note 单条加载与写入路径同样触发 notify (携带缓存生效值)。
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

    /* apply 重新校验恢复值 (多分区兼容), 重放前对越界值裁剪收敛;
       write_complete 补上模块标脏与 notify */
    if (ret == PARAM_OK) {
        ret = param_replay_clamped(e, *entry_cache(e));
        if (ret == PARAM_OK)
            param_write_complete(param_id, e, true);
    }

    return ret;
}

/** @brief 删除单个参数的持久化数据 */
int param_delete_one(uint32_t param_id)
{
    const param_storage_drv_t *drv;
    int ret = param_storage_require(PARAM_CAP_DELETE, &drv);
    if (ret != PARAM_OK)
        return ret;

    int err = drv->delete(drv->ctx, param_id);
    return param_status_from_backend(err);
}

/** @brief 擦除全部持久化数据 */
int param_delete_all(void)
{
    const param_storage_drv_t *drv;
    int ret = param_storage_require(PARAM_CAP_ERASE_ALL, &drv);
    if (ret != PARAM_OK)
        return ret;

    int err = drv->erase_all(drv->ctx);
    return param_status_from_backend(err);
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
 * @note 默认值可能因范围收紧 (param_set_range) 而越界: 重放前先
 *       裁剪收敛 (维护语义)。
 * @note 单条重置与写入路径同样触发 notify (携带缓存生效值)。
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

    /* apply 重新校验默认值 + 恢复 entry 级 dirty; 单条重置与写入路径
       同样触发 notify (携带缓存生效值) */
    if (ret == PARAM_OK) {
        ret = param_replay_clamped(e, *entry_cache(e));
        if (ret == PARAM_OK)
            param_write_complete(param_id, e, true);
    }

    return ret;
}

/**
 * @brief 错误码转可读字符串 — 排障/日志用
 *
 * 全系统只有一个数字错误码表, 没有本函数时调用方要自己维护码表。
 * 返回静态常量串, 不可释放, 不要求参数已初始化。
 *
 * @param err param_status_t 错误码 (或等价负值)
 * @return 可读描述 (未知码返回 "unknown error")
 */
const char *param_strerror(int err)
{
    switch (err) {
    case PARAM_OK:                    return "ok";
    case PARAM_OK_TRUNCATED:          return "ok (value truncated)";
    case PARAM_ERR_INVALID_ID:        return "invalid id";
    case PARAM_ERR_OUT_OF_RANGE:      return "out of range";
    case PARAM_ERR_READONLY:          return "readonly";
    case PARAM_ERR_TYPE_MISMATCH:     return "type mismatch";
    case PARAM_ERR_NOT_FOUND:         return "not found";
    case PARAM_ERR_FLASH_FAIL:        return "flash operation failed";
    case PARAM_ERR_ALREADY_REG:       return "already registered";
    case PARAM_ERR_NO_MEMORY:         return "hash table full";
    case PARAM_ERR_TIMEOUT:           return "timeout";
    case PARAM_ERR_BUSY:              return "busy";
    case PARAM_ERR_NOT_SUPPORTED:     return "not supported by this build";
    case PARAM_ERR_NOT_INITIALIZED:   return "framework not initialized";
    case PARAM_ERR_EXEC_ONLY:         return "command-only param, use param_exec";
    default:                          return "unknown error";
    }
}

/** @brief 获取全局统计信息快照
 *  @param stats 接收快照 (NULL 视为参数错误)
 *  @return PARAM_OK 成功; PARAM_ERR_NOT_INITIALIZED 未初始化;
 *          PARAM_ERR_INVALID_ID stats 为 NULL */
int param_get_stats(param_stats_t *stats)
{
    if (!g_pm.initialized)
        return PARAM_ERR_NOT_INITIALIZED;
    if (!stats)
        return PARAM_ERR_INVALID_ID;
    system_lock(SYS_LOCK_PARAM);
    *stats = g_pm.stats;
    system_unlock(SYS_LOCK_PARAM);
    return PARAM_OK;
}

/** @brief 清零统计计数器 (dirty/error/miss)
 *  @return PARAM_OK 成功; PARAM_ERR_NOT_INITIALIZED 未初始化 */
int param_clear_stats(void)
{
    if (!g_pm.initialized)
        return PARAM_ERR_NOT_INITIALIZED;
    system_lock(SYS_LOCK_PARAM);
    g_pm.stats.dirty_count = 0;
    g_pm.stats.flush_error_count = 0;
    g_pm.stats.flush_order_miss_count = 0;
    system_unlock(SYS_LOCK_PARAM);
    return PARAM_OK;
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
 * @return PARAM_OK 完成/被回调终止; PARAM_ERR_NOT_INITIALIZED 未初始化;
 *         PARAM_ERR_INVALID_ID cb 为 NULL;
 *         PARAM_ERR_NOT_FOUND 指定模块未注册
 */
int param_foreach(uint16_t module_id, param_foreach_fn cb, void *user_data)
{
    if (!g_pm.initialized)
        return PARAM_ERR_NOT_INITIALIZED;
    if (!cb)
        return PARAM_ERR_INVALID_ID;
    if (module_id != 0 && !param_module_find(module_id))
        return PARAM_ERR_NOT_FOUND;

    foreach_ctx_t fctx = { .module_id = module_id, .cb = cb, .user_data = user_data };
    param_module_scan(foreach_accept, foreach_visit, &fctx);
    return PARAM_OK;
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
 * @brief 读取参数的上下限范围
 *
 * @param param_id 参数 ID
 * @param min_val  [out] 接收最小值 (NULL 表示忽略)
 * @param max_val  [out] 接收最大值 (NULL 表示忽略)
 * @return PARAM_OK 成功，或错误码
 */
int param_get_range(uint32_t param_id,
                    param_value_t *min_val,
                    param_value_t *max_val)
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
        *min_val = re->min;
    if (max_val)
        *max_val = re->max;

    system_unlock(SYS_LOCK_PARAM);
    return PARAM_OK;
}

/**
 * @brief 对所有已注册参数执行范围裁剪
 *
 * @note 全程持锁, **不**走 param_module_scan 的分批路径: 裁剪直接改写
 *       entry->cache, 若在锁外执行, 与并发 param_write 的
 *       "读缓存 → 裁剪 → 写回" 之间会丢失对方刚写入的值。
 *       此处只做内存内的 clamp (无 I/O), 持锁代价可忽略。
 * @return PARAM_OK 完成; PARAM_ERR_NOT_INITIALIZED 未初始化
 */
int param_validate_all(void)
{
    if (!g_pm.initialized)
        return PARAM_ERR_NOT_INITIALIZED;
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
    return PARAM_OK;
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
 * @return PARAM_OK 完成; PARAM_ERR_NOT_INITIALIZED 未初始化;
 *         PARAM_ERR_INVALID_ID cb 为 NULL
 */
int param_module_foreach(param_module_iter_fn cb, void *ctx)
{
    if (!g_pm.initialized)
        return PARAM_ERR_NOT_INITIALIZED;
    if (!cb)
        return PARAM_ERR_INVALID_ID;

    module_iter_ctx_t fctx = { .cb = cb, .ctx = ctx };
    param_module_scan(NULL, module_iter_visit, &fctx);
    return PARAM_OK;
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
