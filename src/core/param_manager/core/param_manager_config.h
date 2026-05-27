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
#ifndef PARAM_DEBUG_NAME
#define PARAM_DEBUG_NAME 1
#endif

/** @brief 启用 linker-section 自动模块注册
 *  1=开 0=关。开启后 param_modules_register_all() 遍历 .rodata.param_modules 段 */
#ifndef PARAM_MODULE_AUTO_REGISTER
#define PARAM_MODULE_AUTO_REGISTER 0
#endif

/** @brief 启用 FlashDB 存储后端 (需链接 flashdb 库)
 *  1=开 0=关。0 时使用空操作 stub */
#ifndef USE_FLASHDB
#define USE_FLASHDB 0
#endif

/* ================================================================
 *  OS 移植选择 (三选一，互斥，仅一个可设为 1)
 * ================================================================ */

/** @brief FreeRTOS 移植 (互斥锁 + pvPortMalloc) */
#ifndef PARAM_MANAGER_PORT_FREERTOS
#define PARAM_MANAGER_PORT_FREERTOS 0
#endif

/** @brief 裸机移植 (关中断 + 静态内存池) */
#ifndef PARAM_MANAGER_NO_OS
#define PARAM_MANAGER_NO_OS 0
#endif

/* ================================================================
 *  容量 / 上限
 * ================================================================ */

/** @brief 参数哈希表槽数 (必须为 2 的幂) */
#ifndef PARAM_HASH_SIZE
#define PARAM_HASH_SIZE 256
#endif

/** @brief IP 模块最大参数数 (dirty 位图宽度) */
#ifndef IP_DIRTY_MAP_BITS
#define IP_DIRTY_MAP_BITS 64
#endif

/** @brief 裸机模式静态内存池大小 (字节) */
#ifndef PARAM_PORT_POOL_SIZE
#define PARAM_PORT_POOL_SIZE 4096
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

#ifdef __cplusplus
}
#endif

#endif /* PARAM_MANAGER_CONFIG_H */
