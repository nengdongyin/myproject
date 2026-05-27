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
#include "lwevt/lwevt.h"
#include "param_storage_flashdb.h"
#include "sensor_module.h"
#include "auto_exp_control.h"
#include "ip_param_manager.h"
#include "param_dump.h"

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

/**
 * @brief 参数管理器子系统初始化 — 系统启动时执行完整初始化序列
 *
 * @details
 * 启动流程按以下 10 步顺序执行，每步失败均有错误输出:
 *
 *   1. **存储后端创建**: param_storage_flashdb_create()
 *      智能选择目标分区（读取 param_boot → 选择用户分区/factory）
 *   2. **事件系统**: lwevt_init() 初始化轻量级事件总线
 *   3. **框架初始化**: param_init(storage, on_param_changed)
 *      注册存储驱动和参数变化通知回调
 *   4. **版本迁移**: param_migrate_storage(storage, NULL, 0)
 *      当前 V1 无迁移，保留接口用于未来固件升级
 *   5. **模块注册**: 根据 PARAM_MODULE_AUTO_REGISTER 选择:
 *      - 自动模式: param_modules_register_all() 遍历链接器段
 *      - 手动模式: sensor_module_init() + auto_exp_module_init()
 *   6. **参数加载**: param_load_all()
 *      两阶段: ① 哈希遍历恢复缓存 ② 链表遍历 init 回调下发硬件
 *   7. **完整性校验**: param_check_flush_integrity()
 *      确保所有已注册模块在 MODULE_INIT_ORDER 中有序
 *   8. **动态范围校准**: 根据传感器 FPS 计算曝光时间上限
 *      exposure_max = 990000 / (fps + 1) 微秒
 *      通过 param_set_range() 运行时调整 PID_IP_SENSOR_EXPOSURE 的 max
 *   9. **范围裁剪**: param_validate_all()
 *      遍历所有参数，将越界值裁剪到合法范围
 *  10. **硬件刷新**: param_flush()
 *      将所有 dirty 参数写入硬件寄存器
 *
 * @note 步骤 8 是应用特定的动态校准逻辑，其他项目可替换或删除。
 * @note printf 输出适合开发调试，生产环境应替换为日志框架。
 */
void param_manager_init(void)
{
    /* 步骤 1: 存储后端创建 */
    const param_storage_drv_t *storage = param_storage_flashdb_create();
    if (!storage)
        return;
    /* 步骤 2: 事件系统初始化 */
    lwevt_init();
    /* 步骤 3: 框架初始化 */
    param_init(storage, on_param_changed);

    /* 步骤 4: 存储版本迁移（模块注册和 load_all 之前执行） */
    param_migrate_storage(storage, NULL, 0);

    /* 步骤 5: 模块注册 */
#if PARAM_MODULE_AUTO_REGISTER
    param_modules_register_all();
#else
    sensor_module_init();
    auto_exp_module_init();
#endif

    /* 步骤 6: 从 Flash 加载所有参数 */
    int ret = param_load_all();
    if (ret != PARAM_OK)
        printf("[PM] load_all ret=%d\n", ret);

    /* 步骤 7: 校验 MODULE_INIT_ORDER 完整性 */
    ret = param_check_flush_integrity();
    if (ret != PARAM_OK)
        printf("[PM] WARNING: MODULE_INIT_ORDER not covered!\n");

    /* 步骤 8: 动态范围校准 — 根据 FPS 限制曝光时间 */
    param_value_t fps, exposure_max;
    param_read(PID_IP_SENSOR_FPS, &fps);
    exposure_max.u32 = (uint32_t)(990000u / (fps.u32 + 1));
    param_set_range(PID_IP_SENSOR_EXPOSURE, NULL, &exposure_max);
    printf("[PM] fps=%u -> exposure_max=%u us\n", fps.u32, exposure_max.u32);

    /* 步骤 9: 裁剪所有越界值 */
    param_validate_all();

    /* 步骤 10: 将恢复的参数刷入硬件 */
    ret = param_flush();

    printf("[PM] init done: load_all=%d flush=%d\n", ret, ret);
    printf("[PM] --- dump after init ---\n");
    param_dump(0, dump_cb, NULL);
}
