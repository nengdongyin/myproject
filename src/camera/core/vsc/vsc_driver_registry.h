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

/**
 * @brief 向全局注册表注册一个驱动
 * @param[in] driver 指向驱动描述符的静态指针（不被拷贝）
 * @note 通常在 vsc_system_init() 的生成代码中调用，
 *       将编译期链接的驱动注册到运行时查找表。
 */
void vsc_driver_register(const vsc_driver_t *driver);

/**
 * @brief 按名称查找已注册驱动
 * @param[in] name 驱动名称（如 "crop"、"sensor_imx477"）
 * @return 找到的驱动描述符指针；未找到返回 NULL
 * @details 先查找运行时注册表（g_registered），
 *          再回退到编译期链接的 _vsc_drivers[] 静态数组。
 */
const vsc_driver_t *vsc_driver_find(const char *name);

/**
 * @brief 按索引顺序访问所有已注册驱动
 * @param[in] idx 从 0 开始的驱动索引
 * @return 驱动描述符指针；索引越界返回 NULL
 * @details 先遍历运行时注册表，再遍历编译期静态数组。
 *          常用于遍历所有驱动（如 vsc_feature_derive()）。
 */
const vsc_driver_t *vsc_driver_by_index(int idx);

#ifdef __cplusplus
}
#endif

#endif
