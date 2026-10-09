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

/* ---- 句柄式互斥 ---- */
struct sys_mutex
{
    SemaphoreHandle_t h;
    uint8_t used;
};
static struct sys_mutex g_sys_mutex_pool[SYS_MUTEX_POOL_SIZE];

sys_mutex_t *sys_mutex_create(void)
{
    for (int i = 0; i < SYS_MUTEX_POOL_SIZE; i++)
    {
        if (!g_sys_mutex_pool[i].used)
        {
            SemaphoreHandle_t h = xSemaphoreCreateMutex();
            if (!h)
                return NULL;
            g_sys_mutex_pool[i].h = h;
            g_sys_mutex_pool[i].used = 1;
            return &g_sys_mutex_pool[i];
        }
    }
    return NULL;
}

void sys_mutex_destroy(sys_mutex_t *m)
{
    if (m && m->used)
    {
        vSemaphoreDelete(m->h);
        m->h = NULL;
        m->used = 0;
    }
}

void sys_mutex_lock(sys_mutex_t *m)
{
    if (m && m->h)
        xSemaphoreTake(m->h, portMAX_DELAY);
}

void sys_mutex_unlock(sys_mutex_t *m)
{
    if (m && m->h)
        xSemaphoreGive(m->h);
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

/* ---- 句柄式互斥 ---- */
struct sys_mutex
{
    struct k_mutex m;
    uint8_t used;
};
static struct sys_mutex g_sys_mutex_pool[SYS_MUTEX_POOL_SIZE];

sys_mutex_t *sys_mutex_create(void)
{
    for (int i = 0; i < SYS_MUTEX_POOL_SIZE; i++)
    {
        if (!g_sys_mutex_pool[i].used)
        {
            k_mutex_init(&g_sys_mutex_pool[i].m);
            g_sys_mutex_pool[i].used = 1;
            return &g_sys_mutex_pool[i];
        }
    }
    return NULL;
}

void sys_mutex_destroy(sys_mutex_t *m)
{
    if (m)
        m->used = 0;
}

void sys_mutex_lock(sys_mutex_t *m)
{
    if (m && m->used)
        k_mutex_lock(&m->m, K_FOREVER);
}

void sys_mutex_unlock(sys_mutex_t *m)
{
    if (m && m->used)
        k_mutex_unlock(&m->m);
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

/* ---- 句柄式互斥 ---- */
struct sys_mutex
{
    rt_mutex_t h;
    uint8_t used;
};
static struct sys_mutex g_sys_mutex_pool[SYS_MUTEX_POOL_SIZE];

sys_mutex_t *sys_mutex_create(void)
{
    for (int i = 0; i < SYS_MUTEX_POOL_SIZE; i++)
    {
        if (!g_sys_mutex_pool[i].used)
        {
            rt_mutex_t h = rt_mutex_create("vfsm", RT_IPC_FLAG_PRIO);
            if (!h)
                return NULL;
            g_sys_mutex_pool[i].h = h;
            g_sys_mutex_pool[i].used = 1;
            return &g_sys_mutex_pool[i];
        }
    }
    return NULL;
}

void sys_mutex_destroy(sys_mutex_t *m)
{
    if (m && m->used)
    {
        rt_mutex_delete(m->h);
        m->h = NULL;
        m->used = 0;
    }
}

void sys_mutex_lock(sys_mutex_t *m)
{
    if (m && m->h)
        rt_mutex_take(m->h, RT_WAITING_FOREVER);
}

void sys_mutex_unlock(sys_mutex_t *m)
{
    if (m && m->h)
        rt_mutex_release(m->h);
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

/* ---- 句柄式互斥（单线程，空实现） ---- */
struct sys_mutex
{
    uint8_t used;
};
static struct sys_mutex g_sys_mutex_pool[SYS_MUTEX_POOL_SIZE];

sys_mutex_t *sys_mutex_create(void)
{
    for (int i = 0; i < SYS_MUTEX_POOL_SIZE; i++)
    {
        if (!g_sys_mutex_pool[i].used)
        {
            g_sys_mutex_pool[i].used = 1;
            return &g_sys_mutex_pool[i];
        }
    }
    return NULL;
}

void sys_mutex_destroy(sys_mutex_t *m) { if (m) m->used = 0; }
void sys_mutex_lock(sys_mutex_t *m)    { (void)m; }
void sys_mutex_unlock(sys_mutex_t *m)  { (void)m; }

#endif /* PORT_OS_* */
