#ifndef PARAM_STORAGE_INIPARSER_H
#define PARAM_STORAGE_INIPARSER_H

#include "param_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief INI 存储后端 — 基于 iniparser 库实现
 *
 * @details
 * 每个参数存储为 hex 编码字符串:
 * @code
 *   [params]
 *   p65536 = DEADBEEF
 *   p65537 = 01000000
 * @endcode
 *
 * 多分区支持: 每个分区对应一个 .ini 文件。
 * 每次 save() 全量重写 INI 文件 (tmp → rename 保证原子)。
 *
 * 用法:
 * @code
 *   // 启动时
 *   const param_storage_drv_t *drv = param_storage_iniparser_create("/data/params");
 *   param_init(drv, notify_cb);
 *
 *   // 运行时切换分区
 *   const param_storage_drv_t *part = param_get_storage_partition(1);
 *   param_set_storage(part);
 *   param_load_all();
 * @endcode
 */

/**
 * @brief 创建默认存储驱动（启动时调用，内部完成分区选择与初始化）
 *
 * 流程:
 *   - 读取 <base_dir>/param_boot 获取启动分区索引
 *   - 初始化对应分区的 INI 文件
 *   - 空文件或不存在 → 回退 factory
 *
 * @param base_dir 持久化目录路径 (如 "/data/params")，不能为 NULL
 * @return 已初始化的存储驱动 (不会返回 NULL)
 */
const param_storage_drv_t *param_storage_iniparser_create(const char *base_dir);

/**
 * @brief 按分区名获取存储驱动（工厂模式，按分区名创建独立实例）
 *
 * 同一分区名多次调用返回同一实例指针（幂等）。
 *
 * @param base_dir  持久化目录路径
 * @param part_name 分区名 (如 "param_user0")
 * @return 驱动句柄（静态分配），池耗尽返回 NULL
 */
const param_storage_drv_t *param_storage_iniparser_get_driver(const char *base_dir,
                                                               const char *part_name);

#ifdef __cplusplus
}
#endif

#endif /* PARAM_STORAGE_INIPARSER_H */