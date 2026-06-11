/**
 * @file    vsc_driver_registry.c
 * @brief   VSC Driver Registry — 全局驱动注册、查找、索引。
 *
 * 从 vsc_feature.c 提取，供所有子系统共享。
 */

#include "vsc_driver_registry.h"
#include <string.h>

#define VSC_REGISTERED_MAX 16
static const vsc_driver_t *g_registered[VSC_REGISTERED_MAX];
static uint8_t g_registered_count;

extern const vsc_driver_t _vsc_drivers[];

/**
 * @brief 向全局注册表注册一个驱动（详细文档见 vsc_driver_registry.h）
 */
void vsc_driver_register(const vsc_driver_t *driver)
{
    if (g_registered_count < VSC_REGISTERED_MAX)
        g_registered[g_registered_count++] = driver;
}

/**
 * @brief 按名称查找驱动（详细文档见 vsc_driver_registry.h）
 * @details 两阶段查找：先查运行时注册表 g_registered，
 *          未命中则回退到编译期静态数组 _vsc_drivers[]。
 */
const vsc_driver_t *vsc_driver_find(const char *name)
{
    for (uint8_t i = 0; i < g_registered_count; i++)
        if (strcmp(g_registered[i]->name, name) == 0)
            return g_registered[i];

    for (int i = 0; _vsc_drivers[i].name != NULL; i++)
        if (strcmp(_vsc_drivers[i].name, name) == 0)
            return &_vsc_drivers[i];

    return NULL;
}

/**
 * @brief 按索引访问驱动（详细文档见 vsc_driver_registry.h）
 */
const vsc_driver_t *vsc_driver_by_index(int idx)
{
    if (idx < 0) return NULL;
    if (idx < g_registered_count)
        return g_registered[idx];
    idx -= g_registered_count;
    if (_vsc_drivers[idx].name == NULL) return NULL;
    return &_vsc_drivers[idx];
}
