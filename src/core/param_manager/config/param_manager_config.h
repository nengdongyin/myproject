#ifndef PARAM_MANAGER_CONFIG_H
#define PARAM_MANAGER_CONFIG_H

/**
 * @file param_manager_config.h
 * @brief 集中配置 — 所有用户可调参数汇聚于此
 *
 * 使用方式:
 *   1. 直接编辑此文件
 *   2. 或通过编译器 -D 选项覆盖 (如 -DPARAM_HASH_SIZE=512)
 *
 * 所有宏均采用 #ifndef 守卫: 外部定义的值不会被覆盖。
 */

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 *  Feature 开关
 * ================================================================ */

/** @brief 启用参数调试名称 (每条参数额外存储 const char *name)
 *  关闭可省 4B/条 (32-bit)。1=开, 0=关 */
#ifndef PARAM_DEBUG_NAME_ENABLE
#define PARAM_DEBUG_NAME_ENABLE 1
#endif

/** @brief 启用参数 dump 格式化输出
 *  1=开 0=关。关闭时 param_dump()/param_dump_entry() 为空操作宏，
 *  整个 param_dump.c 不参与编译，避免拉入 stdio 库。 */
#ifndef PARAM_DUMP_ENABLE
#define PARAM_DUMP_ENABLE 1
#endif

/** @brief 启用 linker-section 自动模块注册
 *  1=开 0=关。开启后 param_modules_register_all() 遍历 .rodata.param_modules 段 */
#ifndef PARAM_MODULE_AUTO_REGISTER_ENABLE
#define PARAM_MODULE_AUTO_REGISTER_ENABLE 0
#endif

/** @name 存储后端选择 (调用层开关)
 *
 *  1=该后端可用, 0=不可用。三者**互斥**, 由启动流程的
 *  param_storage_create_selected() 按下列优先级挑一个实例化:
 *    PARAM_FLASHDB_ENABLE=1   → param_storage_flashdb_create()
 *    PARAM_INIPARSER_ENABLE=1 → param_storage_iniparser_create(PARAM_INI_BASE_DIR)
 *    PARAM_MKV_ENABLE=1       → param_storage_mkv_create()        (当前默认)
 *  三者都为 0 → 传 NULL 给 param_init(), 即"本固件不持久化"
 *  (param_load_all 的 LOAD 阶段自动跳过, INIT 阶段照常执行)。
 *
 *  @note 这些宏**不参与任何 .c 的条件编译** —— storage/*.c 已无条件提供
 *        完整实现, 是否把某个后端编进固件由构建层 (target_sources + 链接库)
 *        决定。这样避免"接口改了要同步两份实现 (实体 + stub)"的维护税。
 *  @note 被选中的后端若未编入固件, 会在链接期报 undefined reference ——
 *        这是有意的: 配置声称"选用"就不能只声明不编译。
 *  @{ */

/** @brief 选用 FlashDB 后端 (需把 param_storage_flashdb.c 与 flashdb 库编入) */
#ifndef PARAM_FLASHDB_ENABLE
#define PARAM_FLASHDB_ENABLE 0
#endif

/** @brief 选用 iniparser 后端 (需把 param_storage_iniparser.c 与 iniparser 库编入) */
#ifndef PARAM_INIPARSER_ENABLE
#define PARAM_INIPARSER_ENABLE 0
#endif

/** @brief 选用 mkv 后端 (需把 param_storage_mkv.c 与 src/lib/mkv 编入) */
#ifndef PARAM_MKV_ENABLE
#define PARAM_MKV_ENABLE 1
#endif

/** @} */

/* 存储后端互斥检查: 同时选用多个后端属于配置错误 */
#if (PARAM_FLASHDB_ENABLE + PARAM_INIPARSER_ENABLE + PARAM_MKV_ENABLE) > 1
#error "PARAM_FLASHDB_ENABLE / PARAM_INIPARSER_ENABLE / PARAM_MKV_ENABLE are mutually exclusive"
#endif

/* OS 移植选择已迁移至 port/port.h (PORT_OS_* 宏系列) */

/* ================================================================
 *  容量 / 上限
 * ================================================================ */

/** @brief 参数哈希表槽数 (必须为 2 的幂) */
#ifndef PARAM_HASH_SIZE
#define PARAM_HASH_SIZE 256
#endif

/** @brief IP 模块参数数上限
 *
 * 无独立位图限制 (条目级 dirty 标志追踪)。每模块实际上限 =
 * min(local_id 16-bit 空间, 全局哈希表 PARAM_HASH_SIZE, count 类型宽度)。
 * 全系统参数总数 (App + IP) 受 PARAM_HASH_SIZE 约束。 */

/** @brief 裸机模式静态内存池大小 (字节) */
#ifndef PARAM_PORT_POOL_SIZE
#define PARAM_PORT_POOL_SIZE 4096
#endif

/* ================================================================
 *  iniparser 存储后端
 * ================================================================ */

/** @brief INI 持久化目录 (Linux 文件系统路径) */
#ifndef PARAM_INI_BASE_DIR
#define PARAM_INI_BASE_DIR "/data/params"
#endif

/* ================================================================
 *  FlashDB 存储后端
 * ================================================================ */

/** @brief FlashDB KVDB 名称 */
#ifndef FDB_KVDB_NAME
#define FDB_KVDB_NAME "param_db"
#endif

/** @brief FlashDB 扇区大小 (字节) */
#ifndef FDB_SECTOR_SIZE
#define FDB_SECTOR_SIZE 4096
#endif

/** @brief FlashDB 最大存储实例数 */
#ifndef MAX_INSTANCES
#define MAX_INSTANCES 6
#endif

/* ================================================================
 *  参数分区
 * ================================================================ */

/** @brief 分区总数 (factory + N 个用户分区)。
 *  修改此值需同步: ① FAL 分区表 ② MAX_INSTANCES (≥COUNT)
 *  ③ g_partition_names[] 条目 ④ 启动流程 doc 中的分区数 */
#ifndef PARAM_PARTITION_COUNT
#define PARAM_PARTITION_COUNT 5
#endif

/** @brief factory 分区索引 (出厂默认，固定为 0) */
#define PARAM_PARTITION_FACTORY 0

/** @brief 用户分区索引起始 */
#define PARAM_PARTITION_USER_MIN 1

/** @brief 用户分区索引上限 */
#define PARAM_PARTITION_USER_MAX (PARAM_PARTITION_COUNT - 1)

#ifdef __cplusplus
}
#endif

#endif /* PARAM_MANAGER_CONFIG_H */
