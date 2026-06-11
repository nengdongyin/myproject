#ifndef PARAM_MANAGER_PORT_H
#define PARAM_MANAGER_PORT_H

/**
 * @file port.h
 * @brief 平台移植接口 — OS 选择 + 6 函数声明
 *
 * 使用方式: 编译参数定义 PORT_OS_* 之一 (0/1 布尔宏)
 *   PORT_OS_FREERTOS=1  → FreeRTOS (互斥锁 + pvPortMalloc)
 *   PORT_OS_ZEPHYR=1    → Zephyr (互斥锁 + k_malloc)
 *   PORT_OS_RTTHREAD=1  → RT-Thread (互斥锁 + rt_malloc)
 *   PORT_OS_LIBC=1      → libc malloc/free (+ PORT_LOCK_IRQ=1 关中断)
 *   PORT_OS_CUSTOM=1    → 用户全量自实现
 * 默认: PORT_OS_LIBC (空锁 + malloc/free, PC 测试)
 */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 *  OS 选择宏
 * ================================================================ */
#if !defined(PORT_OS_FREERTOS) && !defined(PORT_OS_ZEPHYR) && \
    !defined(PORT_OS_RTTHREAD) && !defined(PORT_OS_LIBC) && \
    !defined(PORT_OS_CUSTOM)

#ifndef PORT_OS_FREERTOS
#define PORT_OS_FREERTOS  0
#endif
#ifndef PORT_OS_ZEPHYR
#define PORT_OS_ZEPHYR    1
#endif
#ifndef PORT_OS_RTTHREAD
#define PORT_OS_RTTHREAD  0
#endif
#ifndef PORT_OS_LIBC
#define PORT_OS_LIBC      0
#endif
#ifndef PORT_OS_CUSTOM
#define PORT_OS_CUSTOM    0
#endif

#endif

/* 互斥检查 */
#if (PORT_OS_FREERTOS + PORT_OS_ZEPHYR + PORT_OS_RTTHREAD + PORT_OS_LIBC + PORT_OS_CUSTOM) > 1
#error "Only one PORT_OS_* may be set to 1"
#endif

/* 全 0 默认 → LIBC (空锁, PC 测试) */
#if (PORT_OS_FREERTOS + PORT_OS_ZEPHYR + PORT_OS_RTTHREAD + PORT_OS_LIBC + PORT_OS_CUSTOM) == 0
#undef  PORT_OS_LIBC
#define PORT_OS_LIBC 1
#endif

/* LIBC 子开关: 锁方式 */
#ifndef PORT_LOCK_IRQ
#define PORT_LOCK_IRQ 0         /* 1=关中断, 0=空锁 */
#endif

/* ================================================================
 *  统一接口 (全部为函数声明, 无宏)
 * ================================================================ */

void     system_lock(void);
void     system_unlock(void);
void     system_mutex_init(void);
void    *system_malloc(size_t size);
void     system_free(void *ptr);
uint32_t system_get_time_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* PARAM_MANAGER_PORT_H */
