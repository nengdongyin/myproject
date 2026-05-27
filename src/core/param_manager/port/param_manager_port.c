/**
 * @file param_manager_port.c
 * @brief 平台移植层实现 — 提供 FreeRTOS / 裸机 / 默认三种平台适配
 *
 * @details
 * 本文件通过编译宏条件编译实现三种运行模式的平台适配:
 *   - PARAM_MANAGER_PORT_FREERTOS: FreeRTOS 互斥锁 + pvPortMalloc
 *   - PARAM_MANAGER_NO_OS:         关中断嵌套锁 + 静态内存池
 *   - 默认:                        空锁 + 标准 malloc/free（PC 测试用）
 *
 * 所有实现均为弱耦合: 框架不依赖特定 RTOS API，仅通过
 * system_lock / system_unlock / system_malloc / system_free 接口调用。
 * 更换 RTOS 只需修改本文件的条件编译分支。
 */

#include "param_manager_port.h"
#include "param_manager.h"

/* ================================================================
 *  FreeRTOS 移植参考实现
 *
 *  编译条件: 定义了 PARAM_MANAGER_PORT_FREERTOS
 *
 *  使用方法:
 *    1. 在工程中定义 PARAM_MANAGER_PORT_FREERTOS
 *    2. 链接此文件
 *    3. 确保 FreeRTOS 在 param_manager 之前初始化
 * ================================================================ */
#if PARAM_MANAGER_PORT_FREERTOS

#include "FreeRTOS.h"
#include "semphr.h"

static SemaphoreHandle_t g_param_mutex = NULL;

void system_lock(void)
{
    if (g_param_mutex) {
        xSemaphoreTake(g_param_mutex, portMAX_DELAY);
    }
}

void system_unlock(void)
{
    if (g_param_mutex) {
        xSemaphoreGive(g_param_mutex);
    }
}

void system_mutex_init(void)
{
    g_param_mutex = xSemaphoreCreateMutex();
    configASSERT(g_param_mutex != NULL);
}

/* ---- 动态内存 (使用 FreeRTOS heap) ---- */
void *system_malloc(size_t size)
{
    return pvPortMalloc(size);
}

void system_free(void *ptr)
{
    vPortFree(ptr);
}

/* ================================================================
 *  裸机移植参考实现
 *
 *  编译条件: 定义了 PARAM_MANAGER_NO_OS
 * ================================================================ */
#elif PARAM_MANAGER_NO_OS

static uint32_t g_critical_nest = 0;

void system_lock(void)
{
    if (g_critical_nest == 0) {
        __disable_irq();
    }
    g_critical_nest++;
}

void system_unlock(void)
{
    if (g_critical_nest == 0) return;
    g_critical_nest--;
    if (g_critical_nest == 0) {
        __enable_irq();
    }
}

/* ---- 静态内存池 (替代动态分配) ---- */

static uint8_t  g_pool[PARAM_PORT_POOL_SIZE];
static uint32_t g_pool_used;

void *system_malloc(size_t size)
{
    uint32_t n = (uint32_t)((size + 3) & ~3u);
    if (g_pool_used + n > PARAM_PORT_POOL_SIZE) return NULL;
    void *p = &g_pool[g_pool_used];
    g_pool_used += n;
    return p;
}

void system_free(void *ptr)
{
    (void)ptr;
}

/* ================================================================
 *  默认实现 (用于 PC 测试或无 RTOS 环境)
 * ================================================================ */
#else

#include <stdlib.h>

void system_lock(void)   { }
void system_unlock(void) { }

void *system_malloc(size_t size)
{
    return malloc(size);
}

void system_free(void *ptr)
{
    free(ptr);
}

#endif
