#ifndef PARAM_BOOT_H
#define PARAM_BOOT_H

#include "param_manager.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @file param_boot.h
     * @brief 标准启动序列封装 — 启动 8 步收敛为一次调用
     *
     * 应用层无需手工串联"存储后端 → 框架初始化 → 元数据/迁移 →
     * 模块注册 → 加载 → 自检 → 裁剪 → flush", 顺序类错误
     * (如迁移早于注册、忘记自检) 从根上消失。
     * 应用专属部分 (后端选择、notify 桥、迁移表、手动注册清单) 经
     * param_boot_hooks_t 注入。
     */

    /**
     * @brief 启动钩子 — 应用专属部分由调用方提供
     */
    typedef struct
    {
        /** 存储后端工厂 (可 NULL = 本固件不持久化, 只跑 INIT 阶段) */
        const param_storage_drv_t *(*storage_factory)(void);
        /** 参数变化通知回调 (可 NULL) */
        param_notify_fn notify;
        /** 版本迁移表 (可 NULL, 透传给 param_service_init) */
        const param_migrate_entry_t *migrate_table;
        /** 版本迁移表长度 */
        uint16_t migrate_count;
        /** 模块注册 (PARAM_MODULE_AUTO_REGISTER_ENABLE=0 时必须提供;
         *  开启自动注册时被忽略) */
        void (*register_modules)(void);
    } param_boot_hooks_t;

    /**
     * @brief 标准启动序列 (一次调用)
     *
     * 顺序: 存储后端 → param_init → param_service_init (元数据+迁移+旋钮)
     *      → 模块注册 (自动或手动) → param_load_all → 注册完整性自检
     *      → param_validate_all → param_flush
     *
     * @note 收集首个失败错误码 (不中断, 与各步骤"继续处理"的语义一致);
     *       自检只在启动执行一次 —— param_load_all 保持纯恢复语义
     *       (分区热切换也会调用它)。
     *
     * @param hooks 启动钩子 (非 NULL)
     * @return PARAM_OK 全部成功; 否则为首个失败的错误码
     */
    int param_boot_standard(const param_boot_hooks_t *hooks);

#ifdef __cplusplus
}
#endif

#endif /* PARAM_BOOT_H */
