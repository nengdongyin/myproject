/**
 * @file port_impl.c
 * @brief 平台移植实现 — 所有 OS 分支 (#if 分派)
 */

#include "port.h"

#if PORT_OS_CUSTOM

/* 用户全量自实现, 此文件不编译任何对象代码 */

#elif PORT_OS_FREERTOS

/* ================================================================
 *  FreeRTOS
 * ================================================================ */

#include "FreeRTOS.h"
#include "semphr.h"

static SemaphoreHandle_t g_mutex;

void system_lock(void)
{
    if (g_mutex)
        xSemaphoreTake(g_mutex, portMAX_DELAY);
}

void system_unlock(void)
{
    if (g_mutex)
        xSemaphoreGive(g_mutex);
}

void system_mutex_init(void)
{
    g_mutex = xSemaphoreCreateMutex();
}

void *system_malloc(size_t size)
{
    return pvPortMalloc(size);
}

void system_free(void *ptr)
{
    vPortFree(ptr);
}

#elif PORT_OS_ZEPHYR

/* ================================================================
 *  Zephyr
 * ================================================================ */

#include <zephyr/kernel.h>

static K_MUTEX_DEFINE(g_mutex);

void system_lock(void)
{
    k_mutex_lock(&g_mutex, K_FOREVER);
}

void system_unlock(void)
{
    k_mutex_unlock(&g_mutex);
}

void system_mutex_init(void)
{
    /* Zephyr K_MUTEX_DEFINE 静态初始化, 无需动态 init */
}

void *system_malloc(size_t size)
{
    return k_malloc(size);
}

void system_free(void *ptr)
{
    k_free(ptr);
}

#elif PORT_OS_RTTHREAD

/* ================================================================
 *  RT-Thread
 * ================================================================ */

#include <rtthread.h>

static rt_mutex_t g_mutex = RT_NULL;

void system_lock(void)
{
    if (g_mutex)
        rt_mutex_take(g_mutex, RT_WAITING_FOREVER);
}

void system_unlock(void)
{
    if (g_mutex)
        rt_mutex_release(g_mutex);
}

void system_mutex_init(void)
{
    g_mutex = rt_mutex_create("pm", RT_IPC_FLAG_PRIO);
}

void *system_malloc(size_t size)
{
    return rt_malloc(size);
}

void system_free(void *ptr)
{
    rt_free(ptr);
}

#elif PORT_OS_LIBC

/* ================================================================
 *  libc (裸机或 PC 测试)
 * ================================================================ */

#include <stdlib.h>

#if PORT_LOCK_IRQ

static uint32_t g_critical_nest;

void system_lock(void)
{
    if (!g_critical_nest)
        __disable_irq();
    g_critical_nest++;
}

void system_unlock(void)
{
    if (!g_critical_nest)
        return;
    if (!--g_critical_nest)
        __enable_irq();
}

#else

void system_lock(void)   { }

void system_unlock(void) { }

#endif /* PORT_LOCK_IRQ */

void system_mutex_init(void) { }

void *system_malloc(size_t size)
{
    return malloc(size);
}

void system_free(void *ptr)
{
    free(ptr);
}

#endif /* PORT_OS_* */
