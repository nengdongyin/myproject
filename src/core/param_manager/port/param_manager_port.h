#ifndef PARAM_MANAGER_PORT_H
#define PARAM_MANAGER_PORT_H

/**
 * @file param_manager_port.h
 * @brief 平台移植层 — 多任务互斥锁与动态内存接口定义
 *
 * @details
 * 本文件定义了 param_manager 框架所需的全部平台依赖接口。
 * 移植到新平台时，只需实现以下 5 个函数即可完成适配:
 *   - system_lock / system_unlock — 多任务互斥锁
 *   - system_mutex_init            — 互斥锁初始化（RTOS 模式）
 *   - system_malloc / system_free  — 动态内存分配
 *
 * 支持三种运行模式，通过编译宏选择:
 *   - PARAM_MANAGER_PORT_FREERTOS: FreeRTOS 模式（互斥锁 + pvPortMalloc）
 *   - PARAM_MANAGER_NO_OS:         裸机模式（关中断 + 静态内存池）
 *   - 其他（默认）:                 标准 C 库 malloc/free，空锁（PC 测试）
 *
 * 锁宏 LOCK() / UNLOCK() 由本文件根据运行模式自动展开，
 * 框架其余部分统一使用这两个宏，无需感知底层平台。
 */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 *  多任务互斥锁接口
 * ================================================================ */

/**
 * @brief 获取全局互斥锁（阻塞直至成功）
 *
 * 根据编译宏选择实现:
 *   - FreeRTOS: xSemaphoreTake(g_param_mutex, portMAX_DELAY)
 *   - 裸机:     关全局中断 (__disable_irq)，支持嵌套
 *   - 默认:     空操作
 *
 * @note 裸机模式下支持嵌套调用，通过 g_critical_nest 计数追踪。
 *       只有最外层 UNLOCK 才会重新开中断。
 */
void system_lock(void);

/**
 * @brief 释放全局互斥锁
 *
 * 与 system_lock 配对使用。裸机模式下仅在嵌套深度归零时开中断。
 */
void system_unlock(void);

/**
 * @brief 初始化互斥锁（仅在 FreeRTOS 模式下有效）
 *
 * 调用 xSemaphoreCreateMutex() 创建互斥信号量。
 * 裸机模式和默认模式下为空操作。
 *
 * @note 必须在 param_init() 之前调用，且 FreeRTOS 调度器已启动。
 */
void system_mutex_init(void);

#if PARAM_MANAGER_NO_OS
/** @brief 裸机模式: 锁为空操作，由 system_lock 内部关中断保证互斥 */
#define LOCK()   do { } while (0)
#define UNLOCK() do { } while (0)
#else
/** @brief RTOS 模式: 通过互斥信号量实现锁 */
#define LOCK()   system_lock()
#define UNLOCK() system_unlock()
#endif

/* ================================================================
 *  动态内存接口
 * ================================================================ */

/**
 * @brief 平台无关的动态内存分配
 *
 * 根据编译宏选择实现:
 *   - FreeRTOS: pvPortMalloc(size)
 *   - 裸机:     静态内存池分配 (从 g_pool 中切分，不释放)
 *   - 默认:     malloc(size)
 *
 * @param size 请求的字节数
 * @return 分配的内存指针，失败返回 NULL
 */
void *system_malloc(size_t size);

/**
 * @brief 平台无关的动态内存释放
 *
 * 裸机模式下为空操作（静态池不支持释放）。
 *
 * @param ptr 待释放的内存指针
 */
void  system_free(void *ptr);

/**
 * @note 若不想使用动态内存，可在 param_manager_port.c 中定义静态池。
 *       参考该文件 PARAM_MANAGER_NO_OS 分支的实现。
 */

#ifdef __cplusplus
}
#endif

#endif /* PARAM_MANAGER_PORT_H */
