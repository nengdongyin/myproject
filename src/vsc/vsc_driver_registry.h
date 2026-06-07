/**
 * @file    vsc_driver_registry.h
 * @brief   VSC Driver Registry — 驱动注册、查找、索引。
 */

#ifndef VSC_DRIVER_REGISTRY_H
#define VSC_DRIVER_REGISTRY_H

#include "vsc_types.h"

#ifdef __cplusplus
extern "C" {
#endif

void vsc_driver_register(const vsc_driver_t *driver);
const vsc_driver_t *vsc_driver_find(const char *name);
const vsc_driver_t *vsc_driver_by_index(int idx);

#ifdef __cplusplus
}
#endif

#endif
