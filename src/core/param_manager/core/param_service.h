#ifndef PARAM_SERVICE_H
#define PARAM_SERVICE_H

/**
 * @file param_service.h
 * @brief 参数管理子系统自带的两个可选模块 — 框架元数据 + 用户集控制
 *
 * @details
 * 本文件把"任何产品都会重复写一遍"的两件事收进框架, 由**同一个编译单元**
 * 实现 (param_service.c), 但保持**两个独立模块、两个独立 init 入口**:
 *
 *   | 模块 | ID | 内容 | 注册时机 |
 *   |------|----|------|----------|
 *   | MODULE_META  | 0x00 | `PID_SCHEMA_VER` 存储格式版本号 (只读) | 早于首次写版本键 |
 *   | MODULE_SYS   | 0x02 | 用户集控制 (GenICam SFNC §16 UserSetControl) | 早于 param_load_all() |
 *
 * 为什么不把它塞进 param_manager.c: 两个模块各占 5~6 条参数 + 1 个模块节点
 * (约 248 字节 .data), 用不到 `PID_SYS_USERSET_*` 这类旋钮的
 * 产品不应无条件付出。
 * 放在**独立编译单元**里, 只要调用方不调下面的 init 函数, 链接期
 * `--gc-sections` 会把整块 (函数 + 参数表 + 条目) 一起丢掉 —— 因为本工程
 * 已开 `-ffunction-sections -fdata-sections`, 每个条目/参数表各自独占一个段。
 *
 * @par 不链接本单元的降级行为
 * 版本键的正确性由 core 保证 (param_migrate_storage / param_schema_version_ensure
 * 只经 drv->load/save, 不依赖条目), 但会失去两件**可观测性**:
 *   - ini 里记录的参数名是 `"?"` 而不是 `"schema_ver"`;
 *   - `param_read(PID_SCHEMA_VER)` 不可用 (条目未注册)。
 * 即: core 管"每个分区都有正确版本号", service 管"名字可读 + 可经指令读"。
 */

#include <stdint.h>
#include "param_manager.h" /* MAKE_PARAM_ID / PARAM_SCHEMA_VERSION / param_migrate_entry_t */
#include "module_ids.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 *  用户集控制模块 (MODULE_SYS) 的参数 ID
 *
 *  集中在此定义, 供 param_service.c (定义参数表) 与 imperx_cmd_map.c
 *  (把协议地址映射到 ID) 共同引用, 避免两处各写一份字面量。
 *  数值一旦发布不得变更 (module_ids.h: "ID 永不漂移"), 已存的 param_*.ini
 *  因此不受重命名影响。
 * ================================================================ */

/* local_id 按 SFNC §16 特征编号递增排列, 便于与标准文档逐条对照:
     16.2 Selector → 16.4 Load → 16.5 Save → 16.6 Default → 16.7 Reset
   (16.3 Description / 16.8 DefaultSelector / 16.9-10 Feature* 未实现)
   这些参数均不持久化, 因此调整 local_id 不影响已存的 param_*.ini。

   出厂分区 (set 0) 是**只读**的: Save / Reset 拒绝作用于它; 要改出厂分区
   只能走 DEFAULT_SAVE / DEFAULT_DELETE。 */

/** @brief [SFNC 16.2 UserSetSelector] 选中 SAVE/LOAD/RESET 作用的 set (枚举)
 *  @note 写入即切换落盘目标分区 (只换驱动, 不加载内容) */
#define PID_SYS_USERSET_SELECTOR        MAKE_PARAM_ID(MODULE_SYS, 0x00)
/** @brief [SFNC 16.4 UserSetLoad] 把当前选中的 set 加载为活动参数 (EXEC) */
#define PID_SYS_USERSET_LOAD            MAKE_PARAM_ID(MODULE_SYS, 0x01)
/** @brief [SFNC 16.5 UserSetSave] 把活动参数存入当前选中的 set (EXEC)
 *  @note SELECTOR == 出厂分区 时返回 PARAM_ERR_READONLY */
#define PID_SYS_USERSET_SAVE            MAKE_PARAM_ID(MODULE_SYS, 0x02)
/** @brief [SFNC 16.6 UserSetDefault] 上电时加载哪个 set (枚举) */
#define PID_SYS_USERSET_DEFAULT         MAKE_PARAM_ID(MODULE_SYS, 0x03)
/** @brief [SFNC 16.7 UserSetReset] 清空当前选中的 set (EXEC)
 *  @note SELECTOR == 出厂分区 时返回 PARAM_ERR_READONLY */
#define PID_SYS_USERSET_RESET           MAKE_PARAM_ID(MODULE_SYS, 0x04)
/** @brief [厂商扩展] 出厂分区写入: 把活动参数存入出厂分区 (不受 SELECTOR 限制) (EXEC)
 *  @note SFNC 不允许操作出厂集 (Default 仅用作"上电加载哪个"), 但产线要烧写出厂
 *        默认、售后要重建出厂集, 所以以厂商扩展补上。不是 SFNC 特征。
 *  @note **仅供自用** (产线 / 售后), **不经 GenICam XML 对外暴露** —— 第三方工具
 *        看不到也不应依赖这两条。 */
#define PID_SYS_USERSET_DEFAULT_SAVE    MAKE_PARAM_ID(MODULE_SYS, 0x05)
/** @brief [厂商扩展] 出厂分区清除: 清空出厂分区存储 (不受 SELECTOR 限制) (EXEC)
 *  @note 同上 —— SFNC 无对应特征, 仅供自用。 */
#define PID_SYS_USERSET_DEFAULT_DELETE  MAKE_PARAM_ID(MODULE_SYS, 0x06)

/* ================================================================
 *  注册入口
 * ================================================================ */

/**
 * @brief 推荐入口 — 依次完成元数据、版本迁移、用户集控制三件事
 *
 * @details
 * 内部按固定顺序执行, 调用方不必记忆三条隐性契约:
 *   1. `param_meta_module_init()`      — 注册 schema_ver。早于首次写版本键,
 *      这样 ini 里记录的是可读名而不是 `"?"` (字典里的名字在写入时固化,
 *      之后别的键落盘只会原样重写, 不会自愈)。
 *   2. `param_migrate_storage(...)`    — 版本校验 / 迁移表 (core 的通用 API)。
 *   3. `param_service_module_init()`   — UserSetSelector / Default / Save / Load / Reset 旋钮。
 *
 * @param table 迁移表 (可为 NULL, 与 count==0 一起表示"无迁移")
 * @param count 迁移条目数
 * @return PARAM_OK 成功; PARAM_ERR_NOT_SUPPORTED 无存储后端 (param_init 未调用
 *         或后端缺 load/save); 其他为迁移失败码
 *
 * @note **注册与迁移结果解耦**: 即使迁移返回失败, 两个模块仍会全部注册完成。
 *       迁移失败时恰恰更需要 `PID_SYS_USERSET_SAVE` / `..._LOAD` 等旋钮做远程
 *       诊断与重试, 所以返回值只用于报告, 不改变注册行为。
 *       调用方可以记日志后继续走 param_load_all()。
 * @note 必须在 `param_init()` **之后**、`param_load_all()` **之前**调用。
 * @note 不接收 `storage` 实参: 内部用 `param_get_storage()` 取当前后端, 避免
 *       与 param_init 时的指针出现第二个不一致来源。
 */
int param_service_init(const param_migrate_entry_t *table, uint16_t count);

/**
 * @brief 注册框架元数据模块 (MODULE_META) 及其参数表
 *
 * @note 须在 `param_init()` 之后 (注册要求 g_pm.initialized), 且早于
 *       **第一次写版本键** (即 param_migrate_storage) 才能在 ini 里留下可读名。
 *       已经调 `param_service_init()` 时不需要再单独调本函数。
 */
void param_meta_module_init(void);

/**
 * @brief 注册用户集控制模块 (MODULE_SYS) 及其参数表
 *
 * @note 须早于 `param_load_all()`: 晚注册的模块不会收到 init 回调。
 *       已经调 `param_service_init()` 时不需要再单独调本函数。
 */
void param_service_module_init(void);

#ifdef __cplusplus
}
#endif

#endif /* PARAM_SERVICE_H */
