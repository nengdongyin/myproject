#ifndef TEST_COMMON_H
#define TEST_COMMON_H

#include "unity.h"
#include "param_manager.h"
#include "app_param_manager.h"
#include "ip_param_manager.h"
#include "param_manager_port.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 *  测试专用模块 ID (高位区, 不与真实模块冲突)
 * ================================================================ */
#define TEST_MODULE_APPLET  0xF0u
#define TEST_MODULE_APPLET2 0xF1u
#define TEST_MODULE_IP      0xF2u
#define TEST_MODULE_IP2     0xF3u
#define TEST_MODULE_UNORDERED 0xEEu

/* ================================================================
 *  测试参数 ID
 * ================================================================ */
#define TID_APPLET_UINT    MAKE_PARAM_ID(TEST_MODULE_APPLET, 0)
#define TID_APPLET_INT     MAKE_PARAM_ID(TEST_MODULE_APPLET, 1)
#define TID_APPLET_FLOAT   MAKE_PARAM_ID(TEST_MODULE_APPLET, 2)
#define TID_APPLET_BOOL    MAKE_PARAM_ID(TEST_MODULE_APPLET, 3)
#define TID_APPLET_ENUM    MAKE_PARAM_ID(TEST_MODULE_APPLET, 4)
#define TID_APPLET_BLOB    MAKE_PARAM_ID(TEST_MODULE_APPLET, 5)
#define TID_APPLET_RDONLY  MAKE_PARAM_ID(TEST_MODULE_APPLET, 6)
#define TID_APPLET_NOPERSIST MAKE_PARAM_ID(TEST_MODULE_APPLET, 7)

#define TID_APPLET2_UINT   MAKE_PARAM_ID(TEST_MODULE_APPLET2, 0)

#define TID_IP_UINT        MAKE_PARAM_ID(TEST_MODULE_IP, 0)
#define TID_IP_FLOAT       MAKE_PARAM_ID(TEST_MODULE_IP, 1)
#define TID_IP_BOOL        MAKE_PARAM_ID(TEST_MODULE_IP, 2)
#define TID_IP_RDONLY      MAKE_PARAM_ID(TEST_MODULE_IP, 3)

/* ================================================================
 *  测试参数定义宏 (无范围, 可用于各类测试)
 * ================================================================ */
#define DEF_TEST_UINT(_name, _id, _flags, _def, _min, _max) \
    static param_range_entry_t _name = {                     \
        .base = { (_id), &app_vtable },                  \
        .type = PARAM_TYPE_UINT,                            \
        .flags = (_flags),                                  \
        .dirty = 0,                                         \
        .has_range = ((_min) < (_max)),                     \
        .cache       = { .u32 = (_def) },                   \
        .default_val = { .u32 = (_def) },                   \
        .min         = { .u32 = (_min) },                   \
        .max         = { .u32 = (_max) },                   \
        PARAM_DEBUG_NAME_INIT(_name)                         \
    }

#define DEF_TEST_INT(_name, _id, _flags, _def, _min, _max) \
    static param_range_entry_t _name = {                     \
        .base = { (_id), &app_vtable },                  \
        .type = PARAM_TYPE_INT,                             \
        .flags = (_flags),                                  \
        .dirty = 0,                                         \
        .has_range = ((_min) < (_max)),                     \
        .cache       = { .i32 = (_def) },                   \
        .default_val = { .i32 = (_def) },                   \
        .min         = { .i32 = (_min) },                   \
        .max         = { .i32 = (_max) },                   \
        PARAM_DEBUG_NAME_INIT(_name)                         \
    }

#define DEF_TEST_FLOAT(_name, _id, _flags, _def, _min, _max) \
    static param_range_entry_t _name = {                       \
        .base = { (_id), &app_vtable },                    \
        .type = PARAM_TYPE_FLOAT,                             \
        .flags = (_flags),                                    \
        .dirty = 0,                                           \
        .has_range = ((_min) < (_max)),                       \
        .cache       = { .f32 = (_def) },                     \
        .default_val = { .f32 = (_def) },                     \
        .min         = { .f32 = (_min) },                     \
        .max         = { .f32 = (_max) },                     \
        PARAM_DEBUG_NAME_INIT(_name)                          \
    }

/* ================================================================
 *  BLOB 缓冲区
 * ================================================================ */
#define TEST_BLOB_SIZE 64
extern uint8_t g_blob_buf[TEST_BLOB_SIZE];
extern uint8_t g_blob_default[TEST_BLOB_SIZE];

/* ================================================================
 *  Mock Storage — 模拟 Flash 持久化后端
 * ================================================================ */
#define MOCK_STORAGE_MAX 64

typedef struct {
    uint32_t id;
    uint16_t len;
    uint8_t  data[64];
} mock_kv_t;

extern mock_kv_t       g_mock_kv[MOCK_STORAGE_MAX];
extern uint16_t        g_mock_kv_count;
extern int             g_mock_init_called;
extern int             g_mock_deinit_called;
extern int             g_mock_erase_called;
extern int             g_mock_save_ret;
extern int             g_mock_load_ret;
extern int             g_mock_init_ret;

void mock_storage_reset(void);
void mock_storage_set_load_ret(int ret);
void mock_storage_set_save_ret(int ret);
void mock_storage_set_init_ret(int ret);
void mock_storage_preset(uint32_t id, const uint8_t *data, uint16_t len);

extern param_storage_drv_t g_mock_storage;

/* 第二个独立后端, 多分区切换测试 */
extern mock_kv_t        g_mock_kv2[MOCK_STORAGE_MAX];
extern uint16_t         g_mock_kv2_count;
extern int              g_mock_init_called2;
extern int              g_mock_save_ret2;
extern param_storage_drv_t g_mock_storage2;

void mock_storage2_reset(void);
void mock_storage2_set_save_ret(int ret);

/* ================================================================
 *  Mock Apply / Flush 回调追踪
 * ================================================================ */
extern int   g_apply_call_count;
extern int   g_apply_last_ret;
extern uint32_t g_apply_last_id;
extern param_value_t g_apply_last_value;

extern int   g_flush_call_count;
extern int   g_flush_last_ret;
extern void *g_flush_last_ctx;

extern int   g_init_call_count;
extern int   g_init_last_ret;

void mock_apply_reset(void);
void mock_flush_reset(void);
void mock_init_reset(void);
void mock_callbacks_reset(void);

int mock_apply_ok(uint32_t param_id, param_value_t value);
int mock_apply_fail(uint32_t param_id, param_value_t value);
int mock_flush_ok(void *ctx);
int mock_flush_fail(void *ctx);
int mock_init_ok(void *ctx);

/* ================================================================
 *  Mock IP Driver 回调
 * ================================================================ */
extern int g_ip_read_call_count;
extern int g_ip_write_call_count;
extern int g_ip_write_last_ret;
extern param_value_t g_ip_write_last_value;

void mock_ip_reset(void);

int mock_ip_read(void *drv, uint32_t param_id, param_value_t *value);
int mock_ip_write_ok(void *drv, uint32_t param_id, param_value_t value);
int mock_ip_write_fail(void *drv, uint32_t param_id, param_value_t value);
int mock_ip_init_ok(void *drv);

/* ================================================================
 *  参数条目重置 — 条目是 static，跨测试需要手动恢复 cache
 * ================================================================ */
void test_reset_entry(param_entry_t *e);

/* ================================================================
 *  辅助断言
 * ================================================================ */
#define TEST_ASSERT_PARAM_OK(ret)     TEST_ASSERT_EQUAL_INT(PARAM_OK, (ret))
#define TEST_ASSERT_PARAM_ERR(ret, e) TEST_ASSERT_EQUAL_INT((e), (ret))
#define TEST_ASSERT_DIRTY(e, val)     TEST_ASSERT_EQUAL_UINT8((val), entry_dirty((param_entry_t*)(e)))

#ifdef __cplusplus
}
#endif

#endif
