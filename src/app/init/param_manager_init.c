/**
 * @file param_manager_init.c
 * @brief 参数管理器子系统初始化实现 — 串联全部启动流程
 *
 * @details
 * 本文件是 param_manager 子系统与具体应用之间的胶水层。
 * 负责:
 *   - 将 lwevt 事件总线与 param_notify_fn 回调桥接
 *   - 串联存储后端、模块注册、参数加载、flush 等启动步骤
 *   - 提供运行时动态范围校准（如根据 FPS 计算曝光时间上限）
 *
 * 防重入保护: on_param_changed 通过 s_notify_depth 计数器实现
 * 简单防重入（深度 >4 时丢弃），避免参数变化通知在初始化阶段
 * 触发级联写入导致栈溢出。
 */

#include "param_manager_init.h"
#include <stdio.h>
#include <string.h>
#include "param_manager.h"
#include "param_boot.h"
#include "lwevt/lwevt.h"
#include "param_storage_flashdb.h"
#include "param_storage_iniparser.h"
#include "param_storage_mkv.h"
#include "sensor_module.h"
#include "auto_exp_control.h"
#include "ip_param_manager.h"
#include "param_dump.h"
#include "param_service.h"

/**
 * @brief dump 输出回调 — 将格式化文本行输出到控制台
 *
 * @param line      格式化后的参数行
 * @param user_data 用户上下文（本实现忽略）
 */
static void dump_cb(const char *line, void *user_data)
{
    (void)user_data;
    printf("%s\n", line);
}

/** @brief 参数变化通知的防重入深度计数器 */
static volatile uint8_t s_notify_depth = 0;

/**
 * @brief 参数变化通知回调 — 将 param_manager 事件桥接到 lwevt 事件总线
 *
 * @details
 * 防重入策略: 通过 s_notify_depth 计数器限制最大递归深度为 4。
 * 若在 notify 回调内部再次触发 param_write（例如 apply 回调中
 * 修改关联参数），深度超过 4 时静默丢弃，避免无限递归导致栈溢出。
 *
 * 这不是完美的重入保护，但足以应对实际场景中的合理级联修改（通常 ≤2 层）。
 *
 * @param param_id  发生变化的参数 ID
 * @param new_value 写入后的新值（best-effort 快照）
 */
static void on_param_changed(uint32_t param_id, param_value_t new_value)
{
    if (s_notify_depth > 4)
        return;
    s_notify_depth++;

    lwevt_t evt;
    evt.type = PARAM_EVT_CHANGED;
    evt.msg.param_changed.param_id = param_id;
    memcpy(&evt.msg.param_changed.new_value, &new_value, sizeof(new_value));
    lwevt_dispatch_ex(&evt, PARAM_EVT_CHANGED);

    s_notify_depth--;
}

/**
 * @section migration_guide 参数版本迁移指南
 *
 * 当前 PARAM_SCHEMA_VERSION = 1，尚无迁移需求。
 * 固件升级到 V2.0 时，在此定义迁移表并调用 param_migrate_storage。
 *
 * 迁移条目支持三种模式:
 *   1. 改名:  {old_id, NULL,            new_id, NULL}
 *      — 保持数据不变，仅变更参数 ID
 *   2. 转换:  {old_id, convert_func,    0,      NULL}
 *      — 回调内填充 new_id + new_data + new_len
 *   3. 拆分:  回调内多次 param_write_raw 到多个新 ID
 *      — 一个旧参数映射为多个新参数
 *
 * @section migration_example 示例: V1.0 → V2.0
 *
 * @code
 * // 改名 — BOOT_INDEX → BOOT_BANK
 * #define OLD_BOOT_INDEX  MAKE_PARAM_ID(MODULE_SYS, 0x00)
 * #define NEW_BOOT_BANK   MAKE_PARAM_ID(MODULE_SYS, 0x10)
 *
 * // 类型转换 — AE_SPEED: UINT(0→255) → FLOAT(0.0→1.0)
 * static int convert_ae_speed(const uint8_t *data, uint16_t len,
 *                             uint32_t *new_id,
 *                             uint8_t *new_data, uint16_t *new_len,
 *                             void *ctx) {
 *     (void)len; (void)ctx;
 *     float f = (float)(*(uint32_t *)data) / 255.0f;
 *     *new_id = MAKE_PARAM_ID(MODULE_AUTO_EXP, 0x01);
 *     memcpy(new_data, &f, sizeof(f));
 *     *new_len = sizeof(f);
 *     return PARAM_OK;
 * }
 *
 * PARAM_MIGRATE_TABLE(v2_migrations,
 *     {OLD_BOOT_INDEX, NULL,            NEW_BOOT_BANK, NULL},
 *     {OLD_AE_SPEED,   convert_ae_speed, 0,              NULL},
 * );
 * param_migrate_storage(storage, v2_migrations,
 *                       sizeof(v2_migrations) / sizeof(v2_migrations[0]));
 * @endcode
 */

/* ================================================================
 *  存储后端选择 — 全局唯一的"用哪个后端"决策点
 * ================================================================ */

/**
 * @brief 按配置创建持久化后端实例
 *
 * 选择顺序与 param_manager_config.h 的"存储后端选择"一致 (三者互斥):
 *   PARAM_FLASHDB_ENABLE   → param_storage_flashdb_create()
 *   PARAM_INIPARSER_ENABLE → param_storage_iniparser_create(PARAM_INI_BASE_DIR)
 *   PARAM_MKV_ENABLE       → param_storage_mkv_create()      (当前默认)
 *   三者皆 0               → NULL: 本固件不持久化
 *
 * @return 已初始化的存储驱动; NULL = 不持久化
 *         (param_load_all 只跑 INIT 阶段, save/load 返回 PARAM_ERR_NOT_FOUND)
 *
 * @note 被选中的后端 .c 必须真的编入固件, 否则此处链接失败 —— 有意如此:
 *       配置声称"选用"某后端就不能只声明不编译。
 * @note 新增后端时只需在此加一个分支 + 一个 PARAM_xxx_ENABLE 宏,
 *       无需改动 param_manager 核心。
 */
static const param_storage_drv_t *param_storage_create_selected(void)
{
#if PARAM_FLASHDB_ENABLE
    return param_storage_flashdb_create();
#elif PARAM_INIPARSER_ENABLE
    return param_storage_iniparser_create(PARAM_INI_BASE_DIR);
#elif PARAM_MKV_ENABLE
    return param_storage_mkv_create();
#else
    return NULL;
#endif
}

/**
 * @brief 参数管理器子系统初始化 — 系统启动时执行完整初始化序列
 *
 * @details
 * 启动序列已封装为 param_boot_standard (core/param_boot.h), 本文件只注入
 * 应用专属部分:
 *   1. 存储后端工厂 param_storage_create_selected()
 *   2. notify 桥 on_param_changed (lwevt 事件总线)
 *   3. 手动模块注册清单 register_modules()
 *   4. 动态范围校准 calibrate_ranges() (FPS → 曝光时间上限)
 *
 * @note printf 输出适合开发调试，生产环境应替换为日志框架。
 */

/**
 * @brief 手动模块注册清单 (PARAM_MODULE_AUTO_REGISTER_ENABLE=0 时由
 *        param_boot_standard 调用; 开启自动注册时由链接段注册, 本函数不参与)
 *
 * 模块 ID 与顺序在 module_ids.h 的单一清单中登记;
 * 新增模块时在此补一行注册调用。
 */
static void register_modules(void)
{
    sensor_module_init();
    auto_exp_module_init();
}

/**
 * @brief 动态范围校准 — 根据传感器 FPS 限制曝光时间
 *
 * 应用特定的校准逻辑, 由 param_boot_standard 在 load_all 之后、
 * param_validate_all 之前调用; 其他项目可替换或删除。
 */
static void calibrate_ranges(void)
{
    param_value_t fps, exposure_max;
    if (param_read(PID_IP_SENSOR_FPS, &fps) != PARAM_OK)
        return;
    exposure_max.u32 = (uint32_t)(990000u / (fps.u32 + 1));
    param_set_range(PID_IP_SENSOR_EXPOSURE, NULL, &exposure_max);
    printf("[PM] fps=%u -> exposure_max=%u us\n", fps.u32, exposure_max.u32);
}

void param_manager_init(void)
{
    lwevt_init();

    param_boot_hooks_t hooks = {
        .storage_factory  = param_storage_create_selected,
        .notify           = on_param_changed,
        .migrate_table    = NULL,
        .migrate_count    = 0,
        .calibrate        = calibrate_ranges,
        .register_modules = register_modules,
    };

    int ret = param_boot_standard(&hooks);
    printf("[PM] boot ret=%d (%s)\n", ret, param_strerror(ret));

    printf("[PM] --- dump after init ---\n");
    param_dump(0, dump_cb, NULL);
}
