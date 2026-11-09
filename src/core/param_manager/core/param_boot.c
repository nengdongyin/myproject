/**
 * @file param_boot.c
 * @brief 标准启动序列实现 — 见 param_boot.h
 *
 * 本文件是框架侧提供的启动模板: 串联固定的步骤顺序, 应用专属部分
 * (后端选择 / notify 桥 / 迁移表 / 动态校准 / 手动注册清单) 由
 * param_boot_hooks_t 注入, 框架不反向依赖任何应用符号。
 */

#include "param_boot.h"
#include "param_manager_internal.h"
#include "param_service.h"
#include "app_param_manager.h"

int param_boot_standard(const param_boot_hooks_t *hooks)
{
    if (!hooks)
        return PARAM_ERR_INVALID_ID;

    /* 前置校验: 手动注册模式下注册清单必须提供 —— 提前拦截配置错误,
       避免框架已部分初始化后才报错 */
#if !PARAM_MODULE_AUTO_REGISTER_ENABLE
    if (!hooks->register_modules)
        return PARAM_ERR_INVALID_ID;
#endif

    int first_err = PARAM_OK;
#define TRACK_ERR(ret)                                                     \
    do {                                                                   \
        int _r = (ret);                                                    \
        if (_r != PARAM_OK && first_err == PARAM_OK)                       \
            first_err = _r;                                                \
    } while (0)

    /* 步骤 1: 存储后端 (工厂可返回 NULL = 本固件不持久化) */
    const param_storage_drv_t *storage =
        hooks->storage_factory ? hooks->storage_factory() : NULL;

    /* 步骤 2: 框架初始化 */
    TRACK_ERR(param_init(storage, hooks->notify));

    /* 步骤 3: 框架元数据 + 存储版本迁移 + 运维服务旋钮 (三合一入口) */
    TRACK_ERR(param_service_init(hooks->migrate_table, hooks->migrate_count));

    /* 步骤 4: 模块注册 (自动: linker 段; 手动: hooks 清单) */
#if PARAM_MODULE_AUTO_REGISTER_ENABLE
    TRACK_ERR(param_modules_register_all());
#else
    hooks->register_modules();
#endif

    /* 步骤 5: 从存储加载所有参数 (LOAD + INIT 两阶段) */
    TRACK_ERR(param_load_all());

    /* 步骤 6: 注册完整性自检 — 仅启动执行一次
       (不放进 load_all: 分区热切换会反复调用 load_all) */
    TRACK_ERR(param_check_module_order());

    /* 步骤 7: 动态范围校准 (应用专属, 可省略) */
    if (hooks->calibrate)
        hooks->calibrate();

    /* 步骤 8: 裁剪所有越界值 (维护语义) */
    TRACK_ERR(param_validate_all());

    /* 步骤 9: 将恢复的参数刷入硬件 */
    TRACK_ERR(param_flush());

#undef TRACK_ERR
    return first_err;
}
