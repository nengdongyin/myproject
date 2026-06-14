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

static SemaphoreHandle_t g_mutex[SYS_LOCK_COUNT];

void system_lock(sys_lock_id_t id)
{
    if (id < SYS_LOCK_COUNT && g_mutex[id])
        xSemaphoreTake(g_mutex[id], portMAX_DELAY);
}

void system_unlock(sys_lock_id_t id)
{
    if (id < SYS_LOCK_COUNT && g_mutex[id])
        xSemaphoreGive(g_mutex[id]);
}

void system_mutex_init(void)
{
    for (int i = 0; i < SYS_LOCK_COUNT; i++)
        g_mutex[i] = xSemaphoreCreateMutex();
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

K_MUTEX_DEFINE(g_mutex_isc);
K_MUTEX_DEFINE(g_mutex_param);
K_MUTEX_DEFINE(g_mutex_file);

static struct k_mutex *g_mutex[SYS_LOCK_COUNT];

void system_lock(sys_lock_id_t id)
{
    if (id < SYS_LOCK_COUNT && g_mutex[id])
        k_mutex_lock(g_mutex[id], K_FOREVER);
}

void system_unlock(sys_lock_id_t id)
{
    if (id < SYS_LOCK_COUNT && g_mutex[id])
        k_mutex_unlock(g_mutex[id]);
}

void system_mutex_init(void)
{
    g_mutex[SYS_LOCK_PARAM] = &g_mutex_param;
    g_mutex[SYS_LOCK_ISC]   = &g_mutex_isc;
    g_mutex[SYS_LOCK_FILE]  = &g_mutex_file;
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

static rt_mutex_t g_mutex[SYS_LOCK_COUNT];

void system_lock(sys_lock_id_t id)
{
    if (id < SYS_LOCK_COUNT && g_mutex[id])
        rt_mutex_take(g_mutex[id], RT_WAITING_FOREVER);
}

void system_unlock(sys_lock_id_t id)
{
    if (id < SYS_LOCK_COUNT && g_mutex[id])
        rt_mutex_release(g_mutex[id]);
}

void system_mutex_init(void)
{
    g_mutex[SYS_LOCK_PARAM] = rt_mutex_create("pm", RT_IPC_FLAG_PRIO);
    g_mutex[SYS_LOCK_ISC]   = rt_mutex_create("isc", RT_IPC_FLAG_PRIO);
    g_mutex[SYS_LOCK_FILE]  = rt_mutex_create("file", RT_IPC_FLAG_PRIO);
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

void system_lock(sys_lock_id_t id)
{
    (void)id;
    if (!g_critical_nest)
        __disable_irq();
    g_critical_nest++;
}

void system_unlock(sys_lock_id_t id)
{
    (void)id;
    if (!g_critical_nest)
        return;
    if (!--g_critical_nest)
        __enable_irq();
}

#else

void system_lock(sys_lock_id_t id)   { (void)id; }

void system_unlock(sys_lock_id_t id) { (void)id; }

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
