#include "test_common.h"
#include <string.h>

DEF_TEST_UINT  (ou32, TID_APPLET_UINT, PARAM_FLAG_PERSIST, 100, 0, 200);
DEF_TEST_INT   (oi32, TID_APPLET_INT,  PARAM_FLAG_PERSIST, 0, -50, 50);
PARAM_BOOL     (oboo, TID_APPLET_BOOL, PARAM_FLAG_PERSIST, true);

PARAM_IP_UINT  (oipu, TID_IP_UINT,  PARAM_FLAG_PERSIST, 500,   500,   500);
PARAM_IP_FLOAT (oipf, TID_IP_FLOAT, PARAM_FLAG_PERSIST, 2.0f,  2.0f,  2.0f);

PARAM_TABLE(applet_params, &ou32.base, &oi32.base, &oboo.base);
PARAM_TABLE(ip_params, &oipu.base, &oipf.base);

PARAM_MODULE_DEFINE(test_applet, TEST_MODULE_APPLET, "OtherApplet", NULL, NULL, NULL, mock_apply_ok, NULL, NULL);
IP_DRIVER_DEFINE(test_ip, TEST_MODULE_IP, "OtherIP", NULL, NULL, NULL, mock_ip_write_ok, NULL, NULL);

static void register_all(void)
{
    param_module_register(&test_applet_module, applet_params, PARAM_COUNT(applet_params));
    ip_driver_register(&test_ip_instance, ip_params, PARAM_COUNT(ip_params));
    test_reset_entry(&ou32.base); test_reset_entry(&oi32.base);
    test_reset_entry(&oboo.base);
    test_reset_entry(&oipu.base); test_reset_entry(&oipf.base);
}

/* ================================================================
 *  param_set_range
 * ================================================================ */
void test_set_range_narrows(void) {
    register_all();

    param_value_t min = { .u32 = 50 }, max = { .u32 = 150 };
    TEST_ASSERT_PARAM_OK(param_set_range(TID_APPLET_UINT, &min, &max));

    param_value_t v = { .u32 = 10 };
    param_write(TID_APPLET_UINT, v);
    param_read(TID_APPLET_UINT, &v);
    TEST_ASSERT_EQUAL_UINT32(50, v.u32);
}

void test_set_range_non_rangeable(void) {
    register_all();

    param_value_t min = { .b = 0 }, max = { .b = 1 };
    TEST_ASSERT_PARAM_ERR(param_set_range(TID_APPLET_BOOL, &min, &max),
                          PARAM_ERR_TYPE_MISMATCH);
}

/* ================================================================
 *  param_validate_all
 * ================================================================ */
void test_validate_all_clamps(void) {
    register_all();

    param_value_t v = { .u32 = 500 };
    param_write(TID_APPLET_UINT, v);

    param_value_t min = { .u32 = 0 }, max = { .u32 = 150 };
    param_set_range(TID_APPLET_UINT, &min, &max);

    param_validate_all();

    param_read(TID_APPLET_UINT, &v);
    TEST_ASSERT_EQUAL_UINT32(150, v.u32);
}

/* ================================================================
 *  param_foreach
 * ================================================================ */
static int g_foreach_count;

static bool test_foreach_cb(param_entry_t *entry, void *user_data)
{
    (void)entry;
    (void)user_data;
    g_foreach_count++;
    return true;
}

static bool test_foreach_stop_cb(param_entry_t *entry, void *user_data)
{
    (void)entry;
    (void)user_data;
    g_foreach_count++;
    return false;
}

void test_foreach_all_modules(void) {
    register_all();
    g_foreach_count = 0;
    param_foreach(0, test_foreach_cb, NULL);
    TEST_ASSERT_EQUAL_INT(5, g_foreach_count);
}

void test_foreach_specific_module(void) {
    register_all();
    g_foreach_count = 0;
    param_foreach(TEST_MODULE_APPLET, test_foreach_cb, NULL);
    TEST_ASSERT_EQUAL_INT(3, g_foreach_count);
}

void test_foreach_early_stop(void) {
    register_all();
    g_foreach_count = 0;
    param_foreach(0, test_foreach_stop_cb, NULL);
    TEST_ASSERT_EQUAL_INT(1, g_foreach_count);
}

/* ================================================================
 *  param_module_foreach
 * ================================================================ */
static int g_mod_foreach_count;

static void test_mod_foreach_cb(param_module_node_t *m, void *ctx)
{
    (void)m;
    (void)ctx;
    g_mod_foreach_count++;
}

void test_module_foreach_count(void) {
    register_all();
    g_mod_foreach_count = 0;
    param_module_foreach(test_mod_foreach_cb, NULL);
    TEST_ASSERT_EQUAL_INT(2, g_mod_foreach_count);
}

/* ================================================================
 *  param_write_immediate
 * ================================================================ */
void test_write_immediate_applet(void) {
    register_all();

    mock_apply_reset();
    param_value_t v = { .u32 = 42 };
    TEST_ASSERT_PARAM_OK(param_write_immediate(TID_APPLET_UINT, v));

    TEST_ASSERT_EQUAL_INT(1, g_apply_call_count);
    TEST_ASSERT_EQUAL_UINT32(42, g_apply_last_value.u32);

    param_read(TID_APPLET_UINT, &v);
    TEST_ASSERT_EQUAL_UINT32(42, v.u32);
    TEST_ASSERT_DIRTY(&ou32.base, 0);
}

void test_write_immediate_ip(void) {
    register_all();

    mock_ip_reset();
    param_value_t v = { .u32 = 777 };
    TEST_ASSERT_PARAM_OK(param_write_immediate(TID_IP_UINT, v));

    TEST_ASSERT_EQUAL_INT(1, g_ip_write_call_count);
    TEST_ASSERT_EQUAL_UINT32(777, g_ip_write_last_value.u32);

    param_read(TID_IP_UINT, &v);
    TEST_ASSERT_EQUAL_UINT32(777, v.u32);
    TEST_ASSERT_DIRTY(&oipu.base, 0);
}

void test_write_immediate_ip_cb_sees_new_cache(void) {
    test_ip_instance.write = mock_ip_write_readback;
    register_all();

    param_value_t v = { .u32 = 888 };
    TEST_ASSERT_PARAM_OK(param_write_immediate(TID_IP_UINT, v));
    /* 值类型缓存先行: driver write 回调执行期间缓存已是新值 */
    TEST_ASSERT_EQUAL_UINT32(888, g_ip_write_seen_cache.u32);
    test_ip_instance.write = mock_ip_write_ok;
}

void test_write_immediate_ip_fails_keeps_cache(void) {
    test_ip_instance.write = mock_ip_write_fail;
    register_all();

    param_value_t v = { .u32 = 555 };
    TEST_ASSERT_PARAM_ERR(param_write_immediate(TID_IP_UINT, v), PARAM_ERR_NOT_FOUND);
    /* 缓存先行且不回滚: 硬件写入失败后缓存保持新值, 不标记 dirty */
    param_read(TID_IP_UINT, &v);
    TEST_ASSERT_EQUAL_UINT32(555, v.u32);
    TEST_ASSERT_DIRTY(&oipu.base, 0);
    test_ip_instance.write = mock_ip_write_ok;
}

void test_write_immediate_ip_no_write_cb(void) {
    test_ip_instance.write = NULL;
    register_all();

    param_value_t v = { .u32 = 1 };
    TEST_ASSERT_PARAM_ERR(param_write_immediate(TID_IP_UINT, v), PARAM_ERR_NOT_FOUND);
}

/* ================================================================
 *  param_write_raw
 * ================================================================ */
void test_write_raw_uint(void) {
    register_all();

    uint32_t val = 42;
    TEST_ASSERT_PARAM_OK(param_write_raw(TID_APPLET_UINT, (const uint8_t *)&val, 4));

    param_value_t v;
    param_read(TID_APPLET_UINT, &v);
    TEST_ASSERT_EQUAL_UINT32(42, v.u32);
}

void test_write_raw_too_long(void) {
    register_all();

    uint8_t data[sizeof(param_value_t) + 1];
    memset(data, 0, sizeof(data));
    TEST_ASSERT_PARAM_ERR(param_write_raw(TID_APPLET_UINT, data, sizeof(data)),
                          PARAM_ERR_TYPE_MISMATCH);
}

void test_write_raw_null_data(void) {
    register_all();
    TEST_ASSERT_PARAM_ERR(param_write_raw(TID_APPLET_UINT, NULL, 4), PARAM_ERR_INVALID_ID);
}

void test_write_raw_zero_len(void) {
    register_all();
    uint8_t d = 0;
    TEST_ASSERT_PARAM_ERR(param_write_raw(TID_APPLET_UINT, &d, 0), PARAM_ERR_INVALID_ID);
}

/* ================================================================
 *  param_exec
 * ================================================================ */
void test_exec_no_callback(void) {
    register_all();
    TEST_ASSERT_PARAM_ERR(param_exec(MAKE_PARAM_ID(TEST_MODULE_APPLET, 0), NULL), PARAM_ERR_NOT_FOUND);
}

/* ================================================================
 *  BLOB 生命周期
 * ================================================================ */
static uint8_t g_blob_buf_other[TEST_BLOB_SIZE];

void test_blob_write_read(void) {
    static uint8_t data[TEST_BLOB_SIZE];
    memset(data, 0xAB, TEST_BLOB_SIZE);

    static param_blob_entry_t test_blob = {
        .base       = { TID_APPLET_BLOB, &app_vtable },
        .type       = PARAM_TYPE_BLOB,
        .flags      = PARAM_FLAG_PERSIST,
        .dirty      = 0,
        .cache      = { .ptr = g_blob_buf_other },
        .default_val = { .ptr = g_blob_buf_other },
        .blob_size  = TEST_BLOB_SIZE,
        PARAM_DEBUG_NAME_INIT(test_blob)
    };

    static param_entry_t *entries[] = { &test_blob.base };
    PARAM_MODULE_DEFINE(test_blob_mod, (uint16_t)PARAM_MODULE_ID(TID_APPLET_BLOB),
                        "BlobMod", NULL, NULL, NULL, mock_apply_ok, NULL, NULL);
    param_module_register(&test_blob_mod_module, entries, 1);

    /* BLOB 必须走带长度的专用入口 (param_write 拒收 BLOB/STRING) */
    int ret = param_write_blob(TID_APPLET_BLOB, data, sizeof(data));
    TEST_ASSERT_PARAM_OK(ret);

    TEST_ASSERT_EQUAL_UINT8(0xAB, g_blob_buf_other[0]);
    TEST_ASSERT_EQUAL_UINT8(0xAB, g_blob_buf_other[TEST_BLOB_SIZE - 1]);
}

/* ================================================================
 *  STRING 类型
 * ================================================================ */
#define STR_TEST_MODULE 0xF5u
#define STR_TEST_ID MAKE_PARAM_ID(STR_TEST_MODULE, 0)
static char g_str_buf[32];

static param_string_entry_t g_test_str = {
    .base        = { STR_TEST_ID, &app_vtable },
    .type        = PARAM_TYPE_STRING,
    .flags       = PARAM_FLAG_PERSIST,
    .dirty       = 0,
    .cache       = { .ptr = g_str_buf },
    .default_val = { .ptr = g_str_buf },
    .max_len     = 31,
    PARAM_DEBUG_NAME_INIT(g_test_str)
};

static param_entry_t *g_str_entries[] = { &g_test_str.base };
PARAM_MODULE_DEFINE(test_str_mod, STR_TEST_MODULE, "StrMod", NULL, NULL, NULL, mock_apply_ok, NULL, NULL);

void test_string_write_read(void) {
    param_module_register(&test_str_mod_module, g_str_entries, 1);
    strncpy(g_str_buf, "hello", 31);
    g_str_buf[31] = '\0';

    param_value_t v;
    TEST_ASSERT_PARAM_OK(param_read(STR_TEST_ID, &v));
    TEST_ASSERT_EQUAL_STRING("hello", (const char *)v.ptr);
}

void test_string_truncation(void) {
    static char buf[16];
    static param_string_entry_t trunc_str = {
        .base        = { STR_TEST_ID, &app_vtable },
        .type        = PARAM_TYPE_STRING,
        .flags       = 0,
        .dirty       = 0,
        .cache       = { .ptr = buf },
        .default_val = { .ptr = buf },
        .max_len     = 5,
        PARAM_DEBUG_NAME_INIT(trunc_str)
    };
    memset(buf, 0, sizeof(buf));

    static param_entry_t *trunc_entries[] = { &trunc_str.base };
    PARAM_MODULE_DEFINE(trunc_mod, STR_TEST_MODULE, "TruncMod", NULL, NULL, NULL, mock_apply_ok, NULL, NULL);
    param_module_register(&trunc_mod_module, trunc_entries, 1);

    int ret = param_write_string(STR_TEST_ID, "HelloWorld");
    TEST_ASSERT_PARAM_OK(ret);
    TEST_ASSERT_EQUAL_STRING("Hello", buf);
}

void test_string_save_load(void) {
    int ret = param_module_register(&test_str_mod_module, g_str_entries, 1);
    if (ret != PARAM_ERR_ALREADY_REG) TEST_ASSERT_PARAM_OK(ret);
    strncpy(g_str_buf, "World", 31);
    g_str_buf[31] = '\0';

    TEST_ASSERT_PARAM_OK(param_write_string(STR_TEST_ID, g_str_buf));
    param_save_one(STR_TEST_ID);

    param_deinit();
    g_mock_init_called = 0;
    g_mock_deinit_called = 0;
    g_mock_erase_called = 0;
    g_mock_save_ret = 0;
    g_mock_load_ret = 0;
    g_mock_init_ret = 0;
    mock_apply_reset();
    mock_flush_reset();
    mock_init_reset();
    mock_ip_reset();
    param_init(&g_mock_storage, NULL);

    memset(g_str_buf, 0, sizeof(g_str_buf));
    param_module_register(&test_str_mod_module, g_str_entries, 1);
    param_load_one(STR_TEST_ID);

    param_value_t rv;
    param_read(STR_TEST_ID, &rv);
    TEST_ASSERT_EQUAL_STRING("World", (const char *)rv.ptr);
}

/* ================================================================
 *  P0-5: PARAM_STRING (外部缓冲)
 * ================================================================ */
#define MACRO_STR_MODULE 0xF6u
#define TID_MACRO_STR_PTR MAKE_PARAM_ID(MACRO_STR_MODULE, 1)

static char g_ms_ptr_buf[8] = "ptrdef";
PARAM_STRING(ms_ptr, TID_MACRO_STR_PTR, PARAM_FLAG_PERSIST, g_ms_ptr_buf, 7);

PARAM_TABLE(macro_str_params, &ms_ptr.base);
PARAM_MODULE_DEFINE(macro_str_mod, MACRO_STR_MODULE, "MacroStrMod",
                    NULL, NULL, NULL, mock_apply_ok, NULL, NULL);

static void register_macro_str(void)
{
    param_module_register(&macro_str_mod_module, macro_str_params,
                          PARAM_COUNT(macro_str_params));
}

void test_string_macro_ptr_buffer_reset_no_ub(void) {
    register_macro_str();

    TEST_ASSERT_PARAM_OK(param_write_string(TID_MACRO_STR_PTR, "newval"));

    char buf[16];
    TEST_ASSERT_PARAM_OK(param_read_string(TID_MACRO_STR_PTR, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("newval", buf);

    /* 单缓冲配置: reset 跳过自拷贝 (无 UB), 内容保持现状 */
    TEST_ASSERT_PARAM_OK(param_reset_one(TID_MACRO_STR_PTR));
    TEST_ASSERT_PARAM_OK(param_read_string(TID_MACRO_STR_PTR, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("newval", buf);
}

/* ================================================================
 *  写路径硬化 (P1)
 *    - union 入口 (param_write/_cache/_immediate) 拒收 BLOB/STRING
 *    - BLOB 专用入口的长度 / NULL 校验
 *    - write_raw 与 cache_restore 的 STRING 分支按 len 定界
 * ================================================================ */
#define HARDEN_MODULE 0xF7u
#define HID_U32  MAKE_PARAM_ID(HARDEN_MODULE, 0)
#define HID_BLOB MAKE_PARAM_ID(HARDEN_MODULE, 1)
#define HID_STR  MAKE_PARAM_ID(HARDEN_MODULE, 2)

DEF_TEST_UINT(hard_u32, HID_U32, PARAM_FLAG_PERSIST, 7, 0, 100);

static uint8_t g_hard_blob_buf[16];
static char    g_hard_str_buf[8];

static param_blob_entry_t g_hard_blob = {
    .base        = { HID_BLOB, &app_vtable },
    .type        = PARAM_TYPE_BLOB,
    .flags       = PARAM_FLAG_PERSIST,
    .dirty       = 0,
    .cache       = { .ptr = g_hard_blob_buf },
    .default_val = { .ptr = g_hard_blob_buf },
    .blob_size   = sizeof(g_hard_blob_buf),
    PARAM_DEBUG_NAME_INIT(g_hard_blob)
};

static param_string_entry_t g_hard_str = {
    .base        = { HID_STR, &app_vtable },
    .type        = PARAM_TYPE_STRING,
    .flags       = PARAM_FLAG_PERSIST,
    .dirty       = 0,
    .cache       = { .ptr = g_hard_str_buf },
    .default_val = { .ptr = g_hard_str_buf },
    .max_len     = sizeof(g_hard_str_buf) - 1,
    PARAM_DEBUG_NAME_INIT(g_hard_str)
};

static param_entry_t *g_hard_entries[] = {
    &hard_u32.base, &g_hard_blob.base, &g_hard_str.base
};

PARAM_MODULE_DEFINE(hard_mod, HARDEN_MODULE, "HardenMod",
                    NULL, NULL, NULL, mock_apply_ok, NULL, NULL);

static void register_harden(void)
{
    int ret = param_module_register(&hard_mod_module, g_hard_entries,
                                    PARAM_COUNT(g_hard_entries));
    if (ret != PARAM_ERR_ALREADY_REG)
        TEST_ASSERT_PARAM_OK(ret);
    test_reset_entry(&hard_u32.base);
    test_reset_entry(&g_hard_blob.base);
    test_reset_entry(&g_hard_str.base);
}

/* union 入口必须拒收 BLOB/STRING, 值类型不受影响 */
void test_write_rejects_blob_and_string(void) {
    register_harden();

    param_value_t v = { .u32 = 0x11223344 };
    TEST_ASSERT_PARAM_ERR(param_write(HID_BLOB, v), PARAM_ERR_TYPE_MISMATCH);
    TEST_ASSERT_PARAM_ERR(param_write(HID_STR, v), PARAM_ERR_TYPE_MISMATCH);
    TEST_ASSERT_PARAM_ERR(param_write_cache(HID_BLOB, v), PARAM_ERR_TYPE_MISMATCH);
    TEST_ASSERT_PARAM_ERR(param_write_immediate(HID_STR, v), PARAM_ERR_TYPE_MISMATCH);

    /* 类型化 helper 不再能把整数当指针解引用 */
    TEST_ASSERT_PARAM_ERR(param_write_u32(HID_STR, 0x1234), PARAM_ERR_TYPE_MISMATCH);
    TEST_ASSERT_PARAM_ERR(param_write_blob(HID_STR, "x", 1), PARAM_ERR_TYPE_MISMATCH);
    TEST_ASSERT_PARAM_ERR(param_write_string(HID_BLOB, "x"), PARAM_ERR_TYPE_MISMATCH);

    /* 值类型路径不变 */
    TEST_ASSERT_PARAM_OK(param_write_u32(HID_U32, 42));
    param_value_t rv;
    TEST_ASSERT_PARAM_OK(param_read(HID_U32, &rv));
    TEST_ASSERT_EQUAL_UINT32(42, rv.u32);
}

/* param_write_blob: NULL / 长度 / 类型校验 */
void test_write_blob_validates(void) {
    register_harden();

    uint8_t data[sizeof(g_hard_blob_buf)];
    memset(data, 0x5A, sizeof(data));
    memset(g_hard_blob_buf, 0, sizeof(g_hard_blob_buf));

    TEST_ASSERT_PARAM_ERR(param_write_blob(HID_BLOB, NULL, sizeof(data)), PARAM_ERR_INVALID_ID);
    TEST_ASSERT_PARAM_ERR(param_write_blob(HID_BLOB, data, sizeof(data) - 1), PARAM_ERR_OUT_OF_RANGE);
    TEST_ASSERT_PARAM_ERR(param_write_blob(HID_BLOB, data, sizeof(data) + 1), PARAM_ERR_OUT_OF_RANGE);

    /* 被拒绝的写入不得改动缓存 */
    TEST_ASSERT_EQUAL_UINT8(0, g_hard_blob_buf[0]);

    TEST_ASSERT_PARAM_OK(param_write_blob(HID_BLOB, data, sizeof(data)));
    TEST_ASSERT_EQUAL_UINT8(0x5A, g_hard_blob_buf[0]);
    TEST_ASSERT_EQUAL_UINT8(0x5A, g_hard_blob_buf[sizeof(g_hard_blob_buf) - 1]);
    TEST_ASSERT_DIRTY(&g_hard_blob.base, 1);
}

/* write_raw 的 STRING 分支按 len 定界 (不得越过 len 扫 '\0') */
void test_write_raw_string_bounded_by_len(void) {
    register_harden();

    /* 4 个有效字符后紧跟非 '\0' 填充 — 旧实现 strncpy(max_len) 会把填充也拷进来 */
    char src[12];
    memset(src, 'X', sizeof(src));
    memcpy(src, "abcd", 4);
    src[sizeof(src) - 1] = '\0';

    TEST_ASSERT_PARAM_OK(param_write_raw(HID_STR, (const uint8_t *)src, 4));
    TEST_ASSERT_EQUAL_STRING("abcd", g_hard_str_buf);
}

/* param_cache_restore 按记录 len 校验/定界 */
void test_cache_restore_checks_record_len(void) {
    register_harden();

    uint8_t good[sizeof(g_hard_blob_buf)];
    memset(good, 0x33, sizeof(good));
    TEST_ASSERT_PARAM_OK(param_write_blob(HID_BLOB, good, sizeof(good)));

    /* BLOB: 短记录 → 拒绝, 缓存保持不变 (旧实现会按 blob_size 越界读) */
    uint8_t short_rec[4];
    memset(short_rec, 0x77, sizeof(short_rec));
    TEST_ASSERT_PARAM_ERR(param_cache_restore(HID_BLOB, short_rec, sizeof(short_rec)),
                          PARAM_ERR_OUT_OF_RANGE);
    TEST_ASSERT_EQUAL_UINT8(0x33, g_hard_blob_buf[0]);
    TEST_ASSERT_EQUAL_UINT8(0x33, g_hard_blob_buf[sizeof(g_hard_blob_buf) - 1]);

    /* STRING: 无 '\0' 的短记录按 len 定界拷贝并补 '\0' */
    TEST_ASSERT_PARAM_OK(param_cache_restore(HID_STR, (const uint8_t *)"xyz", 3));
    TEST_ASSERT_EQUAL_STRING("xyz", g_hard_str_buf);

    /* 值类型: 短记录补 0, 不引入未初始化读 */
    uint8_t one = 9;
    TEST_ASSERT_PARAM_OK(param_cache_restore(HID_U32, &one, sizeof(one)));
    param_value_t rv;
    TEST_ASSERT_PARAM_OK(param_read_cache(HID_U32, &rv));
    TEST_ASSERT_EQUAL_UINT32(9, rv.u32);
}

void run_test_other(void)
{
    RUN_TEST(test_set_range_narrows);
    RUN_TEST(test_set_range_non_rangeable);
    RUN_TEST(test_validate_all_clamps);
    RUN_TEST(test_foreach_all_modules);
    RUN_TEST(test_foreach_specific_module);
    RUN_TEST(test_foreach_early_stop);
    RUN_TEST(test_module_foreach_count);
    RUN_TEST(test_write_immediate_applet);
    RUN_TEST(test_write_immediate_ip);
    RUN_TEST(test_write_immediate_ip_cb_sees_new_cache);
    RUN_TEST(test_write_immediate_ip_fails_keeps_cache);
    RUN_TEST(test_write_immediate_ip_no_write_cb);
    RUN_TEST(test_write_raw_uint);
    RUN_TEST(test_write_raw_too_long);
    RUN_TEST(test_write_raw_null_data);
    RUN_TEST(test_write_raw_zero_len);
    RUN_TEST(test_exec_no_callback);
    RUN_TEST(test_blob_write_read);
    RUN_TEST(test_string_write_read);
    RUN_TEST(test_string_truncation);
    RUN_TEST(test_string_save_load);
    RUN_TEST(test_string_macro_ptr_buffer_reset_no_ub);
    RUN_TEST(test_write_rejects_blob_and_string);
    RUN_TEST(test_write_blob_validates);
    RUN_TEST(test_write_raw_string_bounded_by_len);
    RUN_TEST(test_cache_restore_checks_record_len);
}
