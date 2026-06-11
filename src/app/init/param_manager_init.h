/**
 * @file param_manager_init.h
 * @brief 参数管理器初始化入口 — 系统启动时调用
 *
 * @details
 * param_manager_init() 是参数管理器子系统的唯一外部入口。
 * 负责串联存储后端创建、事件系统初始化、模块注册、参数加载、
 * flush 完整性校验和运行时范围校准等全部启动流程。
 *
 * 调用时机: 在 RTOS 调度器启动之后、其他业务模块运行之前。
 */

#pragma once

/**
 * @brief 参数管理器子系统初始化
 *
 * @details
 * 启动流程按以下顺序执行:
 *   1. 通过 param_storage_flashdb_create() 创建存储驱动（含智能分区选择）
 *   2. 初始化 lwevt 事件总线
 *   3. 调用 param_init() 注册存储驱动和参数变化通知回调
 *   4. 执行参数版本迁移（当前 V1，无迁移需求）
 *   5. 注册所有模块（自动注册或手动调用 xxx_module_init）
 *   6. 调用 param_load_all() 从 Flash 恢复所有参数
 *   7. 校验 MODULE_INIT_ORDER 完整性
 *   8. 运行动态范围校准（如 FPS → 曝光时间上限）
 *   9. 调用 param_validate_all() 裁剪所有超范围值
 *  10. 调用 param_flush() 将所有恢复的参数刷入硬件
 *
 * @note 内部调用 printf 输出调试信息，适合开发阶段使用。
 *       生产环境建议替换为日志系统或条件编译关闭。
 */
void param_manager_init(void);
