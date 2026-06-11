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
     *   - 从 FAL_BOOT_PART 裸分区读取 1 字节启动索引
     *   - 按 g_partition_names[] 映射到分区名并初始化 KVDB
     *   - 检测用户分区是否为空，空则回退 factory
     *   - 异常保护: 任意步骤失败均回退 factory
     *
     * @return 已初始化的存储驱动 (不会返回 NULL)
     */
    const param_storage_drv_t *param_storage_flashdb_create(void);

    /**
     * @brief 获取 FlashDB 持久化后端驱动（工厂模式，按分区名创建独立实例）
     *
     * 典型用法:
     * @code
     *   // 启动时
     *   const param_storage_drv_t *drv = param_storage_flashdb_create();
     *   param_init(drv, notify_cb);
     *
     *   // 运行时按分区名获取 (不经过 vtable)
     *   const param_storage_drv_t *bak = param_storage_flashdb_get_driver("param_user0");
     *
     *   // 运行时按索引获取 (经过 vtable → 推荐)
     *   const param_storage_drv_t *part = param_get_storage_partition(1);
     *   param_set_storage(part);
     *   param_load_all();
     * @endcode
     *
     * @param part_name  FAL 分区名 (如 "param_user0")
     * @return 驱动句柄 (静态分配，同一分区名返回同一实例)
     */
    const param_storage_drv_t *param_storage_flashdb_get_driver(const char *part_name);

#ifdef __cplusplus
}
#endif

#endif /* PARAM_STORAGE_FLASHDB_H */
