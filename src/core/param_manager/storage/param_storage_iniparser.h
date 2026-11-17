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
 * 每个参数以一行键值对存放, 键为 param_id 的 32 位 hex, 值为自描述的
 * "<参数名>:<类型标签>:<字面量>" —— 目标是人 cat 一眼可读:
 * @code
 *   [params]
 *   0x00000000 = "schema_ver:uint:1"
 *   0x00020000 = "g_boot_index:uint:3"
 *   0x00020001 = "exposure_gain:float:0.75"
 *   0x00020002 = "device_name:string:"CAM-01""
 *   0x00020003 = "calibration:blob:0A1B2C3D"
 * @endcode
 * 类型标签: uint / int / float / bool / enum / string / blob; 无法文本化的
 * 字节 (含控制字符的字符串、未知类型) 退回 hex:"<hex>"。旧的纯 hex 形态
 * ("<参数名>:<hex>") 仍可解码, 但新的保存一律写类型化文本。
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