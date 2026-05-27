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

static void dump_cb(const char *line, void *user_data)
{
    (void)user_data;
    printf("%s\n", line);
}

static volatile uint8_t s_notify_depth = 0;

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

/* ================================================================
 *  参数版本迁移表示例 (固件升级时使用)
 *
 *  当前 PARAM_SCHEMA_VERSION = 1，尚无迁移需求。
 *  升级到 V2.0 时，在此定义迁移表并调用 param_migrate_storage。
 *
 *  迁移条目支持三种模式:
 *    1. 改名:  {old_id, NULL,            new_id, NULL}
 *    2. 转换:  {old_id, convert_func,    0,      NULL} (回调填 new_id+new_data)
 *    3. 拆分:  回调内多次 param_write_raw 到多个新 ID
 *
 *  ===== 示例: V1.0 → V2.0 =====
 *
 *  // 改名 — BOOT_INDEX → BOOT_BANK
 *  #define OLD_BOOT_INDEX  MAKE_PARAM_ID(MODULE_SYS, 0x00)
 *  #define NEW_BOOT_BANK   MAKE_PARAM_ID(MODULE_SYS, 0x10)
 *
 *  // 类型转换 — AE_SPEED: UINT(0→255) → FLOAT(0.0→1.0)
 *  static int convert_ae_speed(const uint8_t *data, uint16_t len,
 *                              uint32_t *new_id,
 *                              uint8_t *new_data, uint16_t *new_len,
 *                              void *ctx) {
 *      (void)len; (void)ctx;
 *      float f = (float)(*(uint32_t *)data) / 255.0f;
 *      *new_id = MAKE_PARAM_ID(MODULE_AUTO_EXP, 0x01);
 *      memcpy(new_data, &f, sizeof(f));
 *      *new_len = sizeof(f);
 *      return PARAM_OK;
 *  }
 *
 *  PARAM_MIGRATE_TABLE(v2_migrations,
 *      {OLD_BOOT_INDEX, NULL,            NEW_BOOT_BANK, NULL},
 *      {OLD_AE_SPEED,   convert_ae_speed, 0,              NULL},
 *  );
 *  param_migrate_storage(storage, v2_migrations,
 *                        sizeof(v2_migrations) / sizeof(v2_migrations[0]));
 * ================================================================ */

void param_manager_init(void)
{
    const param_storage_drv_t *storage = param_storage_flashdb_create();
    if (!storage)
        return;
    lwevt_init();
    param_init(storage, on_param_changed);

    /* 存储版本迁移 — 在模块注册和 load_all 之前执行 */
    param_migrate_storage(storage, NULL, 0);

#if PARAM_MODULE_AUTO_REGISTER
    param_modules_register_all();
#else
    sensor_module_init();
    auto_exp_module_init();
#endif

    int ret = param_load_all();
    if (ret != PARAM_OK)
        printf("[PM] load_all ret=%d\n", ret);

    ret = param_check_flush_integrity();
    if (ret != PARAM_OK)
        printf("[PM] WARNING: MODULE_INIT_ORDER not covered!\n");

    param_value_t fps, exposure_max;
    param_read(PID_IP_SENSOR_FPS, &fps);
    exposure_max.u32 = (uint32_t)(990000u / (fps.u32 + 1));
    param_set_range(PID_IP_SENSOR_EXPOSURE, NULL, &exposure_max);
    printf("[PM] fps=%u -> exposure_max=%u us\n", fps.u32, exposure_max.u32);

    param_validate_all();

    ret = param_flush();

    printf("[PM] init done: load_all=%d flush=%d\n", ret, ret);
    printf("[PM] --- dump after init ---\n");
    param_dump(0, dump_cb, NULL);
}
