#ifndef PARAM_MANAGER_PORT_H
#define PARAM_MANAGER_PORT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 *  平台移植层
 *
 *  用户需根据目标平台实现以下函数:
 *    - system_lock / system_unlock  (多任务互斥)
 *    - system_malloc / system_free  (动态内存)
 *
 *  若使用 RTOS (FreeRTOS/ThreadX/RT-Thread):
 *    请使用对应 RTOS 的互斥锁 API
 *
 *  若为裸机 (无 OS):
 *    定义 PARAM_MANAGER_NO_OS 宏, lock/unlock 被编译为空操作
 *
 * ================================================================ */

/* ================================================================
 *  多任务互斥锁接口
 * ================================================================ */
void system_lock(void);
void system_unlock(void);
void system_mutex_init(void); /**< FreeRTOS 模式下初始化互斥锁 */

#if PARAM_MANAGER_NO_OS
#define LOCK()   do { } while (0)
#define UNLOCK() do { } while (0)
#else
#define LOCK()   system_lock()
#define UNLOCK() system_unlock()
#endif

/* ================================================================
 *  动态内存接口
 * ================================================================ */
void *system_malloc(size_t size);
void  system_free(void *ptr);

/* ================================================================
 *  编译期参数池容量配置 (可选)
 *
 *  若不想使用动态内存, 在 param_manager_port.c 中定义静态池:
 *    #define PARAM_PORT_STATIC_POOL_SIZE  256
 *    static param_entry_t g_param_pool[PARAM_PORT_STATIC_POOL_SIZE];
 *
 *  并在 system_malloc/free 中实现静态池分配
 *  参考 param_manager_port.c 中的 PARAM_MANAGER_NO_OS 实现
 * ================================================================ */

#ifdef __cplusplus
}
#endif

#endif /* PARAM_MANAGER_PORT_H */
