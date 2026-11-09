/**
 * @file param_manager_init.h
 * @brief 参数管理器初始化入口 — 系统启动时调用
 *
 * @details
 * param_manager_init() 是参数管理器子系统的唯一外部入口。
 * 启动序列已封装为 param_boot_standard() (core/param_boot.h),
 * 本文件只注入应用专属部分: 存储后端选择、notify 桥、
 * 迁移表、动态范围校准与手动注册清单。
 *
 * 调用时机: 在 RTOS 调度器启动之后、其他业务模块运行之前。
 */

#pragma once

/**
 * @brief 参数管理器子系统初始化
 *
 * @details
 * 注入 param_boot_hooks_t 后调用 param_boot_standard(), 顺序:
 *   1. 存储后端创建 (PARAM_xxx_ENABLE 选择)
 *   2. param_init() (存储驱动 + notify 回调)
 *   3. param_service_init() (元数据 + 版本迁移 + 运维旋钮)
 *   4. 模块注册 (自动注册或手动清单 register_modules)
 *   5. param_load_all() 从存储恢复所有参数
 *   6. param_check_module_order() 注册完整性自检 (仅启动一次)
 *   7. 动态范围校准 (FPS → 曝光时间上限)
 *   8. param_validate_all() 裁剪超范围值
 *   9. param_flush() 刷入硬件
 *
 * @note 内部调用 printf 输出调试信息，适合开发阶段使用。
 *       生产环境建议替换为日志系统或条件编译关闭。
 */
void param_manager_init(void);
