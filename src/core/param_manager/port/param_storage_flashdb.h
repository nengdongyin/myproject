#ifndef PARAM_STORAGE_FLASHDB_H
#define PARAM_STORAGE_FLASHDB_H

#include "param_manager.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief 创建默认存储驱动（启动时调用，内部完成分区选择与初始化）
     *
     * 内部流程:
     *   - 从 "param_boot" 裸分区读取 1 字节启动索引
     *   - 映射到目标分区名并初始化 KVDB
     *   - 检测目标分区是否为空，空则回退 param_factory
     *   - 异常保护: 任意步骤失败均回退 param_factory
     *
     * @return 已初始化的存储驱动 (不会返回 NULL)
     */
    const param_storage_drv_t *param_storage_flashdb_create(void);

    /**
     * @brief 获取 FlashDB 持久化后端驱动（工厂模式，按分区名创建独立实例）
     *
     * 典型用法:
     *   drv0 = param_storage_flashdb_get_driver("param_user0");
     *   drv1 = param_storage_flashdb_get_driver("param_bank1");
     *   param_init(drv0);
     *   param_set_storage(drv1);
     *
     * @param part_name  FAL 分区名 (如 "param_user0")
     * @return 驱动句柄 (静态分配，同一分区名返回同一实例)
     */
    const param_storage_drv_t *param_storage_flashdb_get_driver(const char *part_name);

#ifdef __cplusplus
}
#endif

#endif /* PARAM_STORAGE_FLASHDB_H */
