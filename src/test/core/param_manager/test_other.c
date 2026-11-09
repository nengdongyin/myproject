#include "test_common.h"
#include <string.h>

DEF_TEST_UINT  (ou32, TID_APPLET_UINT, PARAM_FLAG_PERSIST, 100, 0, 200);
DEF_TEST_INT   (oi32, TID_APPLET_INT,  PARAM_FLAG_PERSIST, 0, -50, 50);
PARAM_BOOL     (oboo, TID_APPLET_BOOL, PARAM_FLAG_PERSIST, true);

PARAM_IP_UINT  (oipu, TID_IP_UINT,  PARAM_FLAG_PERSIST, 500,   500,   500);
PARAM_IP_FLOAT (oipf, TID_IP_FLOAT, PARAM_FLAG_PERSIST, 2.0f,  2.0f,  2.0f);

PARAM_TABLE(applet_params, &ou32.base, &oi32.base, &oboo.base);
PARAM_TABLE(ip_params, &oipu.base, &oipf.base);

PARAM_MODULE_DEFINE(test_applet, TEST_MODULE_APPLET, "OtherApplet", .write = mock_apply_ok);
IP_DRIVER_DEFINE(test_ip, TEST_MODULE_IP, "OtherIP", .write = mock_ip_write_ok);

static void register_all(void)
{
    param_module_register(&test_applet_module, applet_params, PARAM_COUNT(applet_params));
    param_module_register(&test_ip_module, ip_params, PARAM_COUNT(ip_params));
    test_reset_entry(&ou32.base); test_reset_entry(&oi32.base);
    test_reset_entry(&oboo.base);
    test_reset_entry(&oipu.base); test_reset_entry(&oipf.base);

    /* param_set_range 会永久改写静态条目的 min/max/has_range (test_reset_entry
       只恢复 cache 与 dirty), 这里复位——否则用例结果隐性依赖执行顺序
       (实测: test_set_range_narrows 把 ou32 收窄到 [50,150] 后再也不会恢复) */
    ou32.min.u32 = 0;
    ou32.max.u32 = 200;
    ou32.has_range = 1;
}

/* ================================================================
 *  param_set_range
 * ================================================================ */
void test_set_range_narrows(void) {
    register_all();

    param_value_t min = { .u32 = 50 }, max = { .u32 = 150 };
    TEST_ASSERT_PARAM_OK(param_set_range(TID_APPLET_UINT, &min, &max));

    /* 超范围写入拒绝, 缓存不变 (默认 100 在新范围内) */
    param_value_t v = { .u32 = 10 };
    TEST_ASSERT_PARAM_ERR(param_write(TID_APPLET_UINT, v), PARAM_ERR_OUT_OF_RANGE);
    param_read(TID_APPLET_UINT, &v);
    TEST_ASSERT_EQUAL_UINT32(100, v.u32);
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

    /* 先写一个旧范围内合法的值 */
    param_value_t v = { .u32 = 150 };
    TEST_ASSERT_PARAM_OK(param_write(TID_APPLET_UINT, v));

    /* 运行时收紧范围 → 缓存越界 */
    param_value_t min = { .u32 = 0 }, max = { .u32 = 100 };
    TEST_ASSERT_PARAM_OK(param_set_range(TID_APPLET_UINT, &min, &max));

    /* 维护工具: 显式裁剪收敛 (裁剪只发生在显式维护操作) */
    TEST_ASSERT_PARAM_OK(param_validate_all());

    param_read(TID_APPLET_UINT, &v);
    TEST_ASSERT_EQUAL_UINT32(100, v.u32);
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

void test_foreach_unknown_module(void) {
    register_all();
    /* 指定未注册模块返回错误 (错误可见) */
    TEST_ASSERT_PARAM_ERR(param_foreach(0xDEAD, test_foreach_cb, NULL),
                          PARAM_ERR_NOT_FOUND);
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
 *  param_write_sync (App: apply+标脏; IP: 直通硬件+清脏)
 * ================================================================ */
void test_write_sync_applet(void) {
    register_all();

    mock_apply_reset();
    param_value_t v = { .u32 = 42 };
    TEST_ASSERT_PARAM_OK(param_write_sync(TID_APPLET_UINT, v));

    TEST_ASSERT_EQUAL_INT(1, g_apply_call_count);
    TEST_ASSERT_EQUAL_UINT32(42, g_apply_last_value.u32);

    param_read(TID_APPLET_UINT, &v);
    TEST_ASSERT_EQUAL_UINT32(42, v.u32);
    /* apply 校验 + **标脏**, 硬件由 flush 按 dirty 下发 */
    TEST_ASSERT_DIRTY(&ou32.base, 1);
}

void test_write_sync_ip(void) {
    register_all();

    mock_ip_reset();
    param_value_t v = { .u32 = 777 };
    TEST_ASSERT_PARAM_OK(param_write_sync(TID_IP_UINT, v));

    TEST_ASSERT_EQUAL_INT(1, g_ip_write_call_count);
    TEST_ASSERT_EQUAL_UINT32(777, g_ip_write_last_value.u32);

    param_read(TID_IP_UINT, &v);
    TEST_ASSERT_EQUAL_UINT32(777, v.u32);
    TEST_ASSERT_DIRTY(&oipu.base, 0);
}

void test_write_sync_ip_cb_sees_new_cache(void) {
    test_ip_module.write = mock_ip_write_readback;
    register_all();

    param_value_t v = { .u32 = 888 };
    TEST_ASSERT_PARAM_OK(param_write_sync(TID_IP_UINT, v));
    /* 值类型缓存先行: driver write 回调执行期间缓存已是新值 */
    TEST_ASSERT_EQUAL_UINT32(888, g_ip_write_seen_cache.u32);
    test_ip_module.write = mock_ip_write_ok;
}

void test_write_sync_ip_fails_keeps_cache(void) {
    test_ip_module.write = mock_ip_write_fail;
    register_all();

    param_value_t v = { .u32 = 555 };
    TEST_ASSERT_PARAM_ERR(param_write_sync(TID_IP_UINT, v), PARAM_ERR_NOT_FOUND);
    /* 缓存先行且不回滚 (IP flush 重试模型): 硬件写入失败后缓存保持新值 */
    param_read(TID_IP_UINT, &v);
    TEST_ASSERT_EQUAL_UINT32(555, v.u32);
    TEST_ASSERT_DIRTY(&oipu.base, 0);
    test_ip_module.write = mock_ip_write_ok;
}

/* IP 侧同样: write 是可选钩子, 缺失时"同步"仅到缓存层, 不视为错误。 */
void test_write_sync_ip_no_write_cb(void) {
    test_ip_module.write = NULL;
    register_all();

    param_value_t v = { .u32 = 1 };
    TEST_ASSERT_PARAM_OK(param_write_sync(TID_IP_UINT, v));

    param_value_t cached;
    param_read_cache(TID_IP_UINT, &cached);
    TEST_ASSERT_EQUAL_UINT32(1, cached.u32);
    TEST_ASSERT_DIRTY(&oipu.base, 0);

    test_ip_module.write = mock_ip_write_ok; /* 恢复, 不泄漏给后续用例 */
}

/* App 侧: write 只是可选的校验钩子, 缺失时退化为"仅写缓存 + 标脏" */
void test_write_sync_applet_no_write_cb(void) {
    param_write_fn saved = test_applet_module.write;
    test_applet_module.write = NULL;
    register_all();

    param_value_t v = { .u32 = 42 };
    TEST_ASSERT_PARAM_OK(param_write_sync(TID_APPLET_UINT, v));

    param_value_t cached;
    param_read_cache(TID_APPLET_UINT, &cached);
    TEST_ASSERT_EQUAL_UINT32(42, cached.u32);
    /* 标脏等 flush */
    TEST_ASSERT_DIRTY(&ou32.base, 1);

    test_applet_module.write = saved;
}

/* ================================================================
 *  notify 回调必须携带"缓存生效值"而非调用者的原始入参;
 *  超范围写入被拒绝 → 不触发 notify
 * ================================================================ */
static int g_notify_count;
static uint32_t g_notify_last_id;
static param_value_t g_notify_last_value;

static void test_notify_cb(uint32_t param_id, param_value_t new_value)
{
    g_notify_count++;
    g_notify_last_id = param_id;
    g_notify_last_value = new_value;
}

void test_notify_reports_effective_value(void) {
    param_deinit();
    param_init(&g_mock_storage, test_notify_cb);
    register_all();

    g_notify_count = 0;
    /* 超范围写入拒绝, 不产生 notify, 缓存不变 */
    param_value_t v = { .u32 = 250 };
    TEST_ASSERT_PARAM_ERR(param_write(TID_APPLET_UINT, v), PARAM_ERR_OUT_OF_RANGE);
    TEST_ASSERT_EQUAL_INT(0, g_notify_count);

    /* 范围内写入: notify 携带缓存生效值 */
    v.u32 = 150;
    TEST_ASSERT_PARAM_OK(param_write(TID_APPLET_UINT, v));
    TEST_ASSERT_EQUAL_INT(1, g_notify_count);
    TEST_ASSERT_EQUAL_UINT32(TID_APPLET_UINT, g_notify_last_id);
    TEST_ASSERT_EQUAL_UINT32(150, g_notify_last_value.u32);

    /* 维护路径收敛 (set_range 收紧触发内部重写): notify 携带裁剪后的生效值 */
    param_value_t min = { .u32 = 0 }, max = { .u32 = 100 };
    TEST_ASSERT_PARAM_OK(param_set_range(TID_APPLET_UINT, &min, &max));
    TEST_ASSERT_EQUAL_INT(2, g_notify_count);
    TEST_ASSERT_EQUAL_UINT32(100, g_notify_last_value.u32);

    param_value_t cached;
    param_read_cache(TID_APPLET_UINT, &cached);
    TEST_ASSERT_EQUAL_UINT32(100, cached.u32);
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
 *  exec / load_one / reset_one 的 notify 一致性
 * ================================================================ */
static int g_exec_got;
static param_value_t g_exec_last_arg;

static int test_exec_cb(void *ctx, uint32_t param_id, param_value_t arg)
{
    (void)ctx;
    (void)param_id;
    g_exec_got++;
    g_exec_last_arg = arg;
    return PARAM_OK;
}

void test_exec_notifies_with_payload(void) {
    param_deinit();
    param_init(&g_mock_storage, test_notify_cb);
    register_all();

    PARAM_IP_EXEC(test_cmd, MAKE_PARAM_ID(TEST_MODULE_IP2, 0xF0));
    PARAM_TABLE(cmd_params, &test_cmd.base);
    IP_DRIVER_DEFINE(cmd_ip, TEST_MODULE_IP2, "CmdIP",
                     .exec = test_exec_cb);
    param_module_register(&cmd_ip_module, cmd_params, PARAM_COUNT(cmd_params));

    g_notify_count = 0;
    g_exec_got = 0;
    uint32_t payload = 0x1234;
    TEST_ASSERT_PARAM_OK(param_exec(test_cmd.base.param_id, &payload));

    TEST_ASSERT_EQUAL_INT(1, g_exec_got);
    TEST_ASSERT_EQUAL_INT(1, g_notify_count);
    TEST_ASSERT_EQUAL_UINT32(test_cmd.base.param_id, g_notify_last_id);
    /* notify 负载与 exec 回调收到的 arg 一致 (.ptr = user_arg) */
    TEST_ASSERT_EQUAL_PTR(&payload, g_notify_last_value.ptr);
    TEST_ASSERT_EQUAL_PTR(g_exec_last_arg.ptr, g_notify_last_value.ptr);
}

void test_load_one_notifies(void) {
    param_deinit();
    param_init(&g_mock_storage, test_notify_cb);
    register_all();

    uint32_t stored = 77;
    mock_storage_preset(TID_APPLET_UINT, (const uint8_t *)&stored, sizeof(stored));

    g_notify_count = 0;
    TEST_ASSERT_PARAM_OK(param_load_one(TID_APPLET_UINT));
    TEST_ASSERT_EQUAL_INT(1, g_notify_count);
    TEST_ASSERT_EQUAL_UINT32(TID_APPLET_UINT, g_notify_last_id);
    TEST_ASSERT_EQUAL_UINT32(77, g_notify_last_value.u32);
}

void test_reset_one_notifies(void) {
    param_deinit();
    param_init(&g_mock_storage, test_notify_cb);
    register_all();

    g_notify_count = 0;
    TEST_ASSERT_PARAM_OK(param_reset_one(TID_APPLET_UINT));
    TEST_ASSERT_EQUAL_INT(1, g_notify_count);
    TEST_ASSERT_EQUAL_UINT32(TID_APPLET_UINT, g_notify_last_id);
    TEST_ASSERT_EQUAL_UINT32(100, g_notify_last_value.u32);
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
                        "BlobMod", .write = mock_apply_ok);
    param_module_register(&test_blob_mod_module, entries, 1);

    /* BLOB 走带长度的专用入口 param_write_raw (param_write 拒收 BLOB/STRING) */
    int ret = param_write_raw(TID_APPLET_BLOB, data, sizeof(data));
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
PARAM_MODULE_DEFINE(test_str_mod, STR_TEST_MODULE, "StrMod", .write = mock_apply_ok);

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
    PARAM_MODULE_DEFINE(trunc_mod, STR_TEST_MODULE, "TruncMod", .write = mock_apply_ok);
    param_module_register(&trunc_mod_module, trunc_entries, 1);

    int ret = param_write_string(STR_TEST_ID, "HelloWorld");
    /* 截断可感知: 成功但被截断返回正码 PARAM_OK_TRUNCATED */
    TEST_ASSERT_EQUAL_INT(PARAM_OK_TRUNCATED, ret);
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
                    .write = mock_apply_ok);

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
                    .write = mock_apply_ok);

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
    TEST_ASSERT_PARAM_ERR(param_write_sync(HID_STR, v), PARAM_ERR_TYPE_MISMATCH);

    /* 类型化 helper 不会把整数当指针解引用 */
    TEST_ASSERT_PARAM_ERR(param_write_u32(HID_STR, 0x1234), PARAM_ERR_TYPE_MISMATCH);
    /* write_raw 是带长度的通用入口, 对 STRING 目标是合法路径 (此处验证其可用) */
    TEST_ASSERT_PARAM_OK(param_write_raw(HID_STR, (const uint8_t *)"x", 1));
    TEST_ASSERT_PARAM_ERR(param_write_string(HID_BLOB, "x"), PARAM_ERR_TYPE_MISMATCH);

    /* 值类型路径不变 */
    TEST_ASSERT_PARAM_OK(param_write_u32(HID_U32, 42));
    param_value_t rv;
    TEST_ASSERT_PARAM_OK(param_read(HID_U32, &rv));
    TEST_ASSERT_EQUAL_UINT32(42, rv.u32);
}

/* param_write_raw (BLOB 分支): NULL / 长度 / 类型校验 */
void test_write_raw_blob_validates(void) {
    register_harden();

    uint8_t data[sizeof(g_hard_blob_buf)];
    memset(data, 0x5A, sizeof(data));
    memset(g_hard_blob_buf, 0, sizeof(g_hard_blob_buf));

    TEST_ASSERT_PARAM_ERR(param_write_raw(HID_BLOB, NULL, sizeof(data)), PARAM_ERR_INVALID_ID);
    TEST_ASSERT_PARAM_ERR(param_write_raw(HID_BLOB, data, sizeof(data) - 1), PARAM_ERR_OUT_OF_RANGE);
    TEST_ASSERT_PARAM_ERR(param_write_raw(HID_BLOB, data, sizeof(data) + 1), PARAM_ERR_OUT_OF_RANGE);

    /* 被拒绝的写入不得改动缓存 */
    TEST_ASSERT_EQUAL_UINT8(0, g_hard_blob_buf[0]);

    TEST_ASSERT_PARAM_OK(param_write_raw(HID_BLOB, data, sizeof(data)));
    TEST_ASSERT_EQUAL_UINT8(0x5A, g_hard_blob_buf[0]);
    TEST_ASSERT_EQUAL_UINT8(0x5A, g_hard_blob_buf[sizeof(g_hard_blob_buf) - 1]);
    TEST_ASSERT_DIRTY(&g_hard_blob.base, 1);
}

/* ================================================================
 *  param_write_ex / param_write_bytes (CLAMP 旗标) + param_reapply
 * ================================================================ */
void test_write_ex_clamp(void) {
    register_harden();

    /* 默认 (flags=0): 超范围拒绝 (与 param_write 一致) */
    param_value_t v = { .u32 = 250 };
    TEST_ASSERT_PARAM_ERR(param_write_ex(HID_U32, v, 0), PARAM_ERR_OUT_OF_RANGE);

    /* PARAM_WR_CLAMP: 裁剪到合法区间 (显式裁剪旗标) */
    TEST_ASSERT_PARAM_OK(param_write_ex(HID_U32, v, PARAM_WR_CLAMP));
    param_read_cache(HID_U32, &v);
    TEST_ASSERT_EQUAL_UINT32(100, v.u32);
}

void test_write_ex_unknown_flag(void) {
    register_harden();
    param_value_t v = { .u32 = 1 };
    /* 未定义旗标拒绝 (而非静默忽略) */
    TEST_ASSERT_PARAM_ERR(param_write_ex(HID_U32, v, 0x80000000u),
                          PARAM_ERR_NOT_SUPPORTED);
}

void test_write_bytes_value_clamp(void) {
    register_harden();

    uint32_t raw = 250;
    TEST_ASSERT_PARAM_ERR(param_write_bytes(HID_U32, (const uint8_t *)&raw, sizeof(raw), 0),
                          PARAM_ERR_OUT_OF_RANGE);
    TEST_ASSERT_PARAM_OK(param_write_bytes(HID_U32, (const uint8_t *)&raw, sizeof(raw),
                                           PARAM_WR_CLAMP));
    param_value_t v;
    param_read_cache(HID_U32, &v);
    TEST_ASSERT_EQUAL_UINT32(100, v.u32);
}

void test_reapply_revalidates(void) {
    register_all();

    /* 先写合法值, 再运行时收紧范围, reapply 重放完整写入路径 */
    param_value_t v = { .u32 = 150 };
    TEST_ASSERT_PARAM_OK(param_write(TID_APPLET_UINT, v));

    param_value_t min = { .u32 = 0 }, max = { .u32 = 100 };
    TEST_ASSERT_PARAM_OK(param_set_range(TID_APPLET_UINT, &min, &max));

    mock_apply_reset();
    TEST_ASSERT_PARAM_OK(param_reapply(TID_APPLET_UINT));
    TEST_ASSERT_EQUAL_INT(1, g_apply_call_count); /* 重放触发 apply */
    param_read_cache(TID_APPLET_UINT, &v);
    TEST_ASSERT_EQUAL_UINT32(100, v.u32);
}

/* write_raw 的 STRING 分支按 len 定界 (不得越过 len 扫 '\0') */
void test_write_raw_string_bounded_by_len(void) {
    register_harden();

    /* 4 个有效字符后紧跟非 '\0' 填充 —— 若按 strncpy(max_len) 处理, 填充也会被拷入 */
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
    TEST_ASSERT_PARAM_OK(param_write_raw(HID_BLOB, good, sizeof(good)));

    /* BLOB: 短记录 → 拒绝, 缓存保持不变 (若按 blob_size 读会越过记录长度) */
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
    RUN_TEST(test_foreach_unknown_module);
    RUN_TEST(test_foreach_early_stop);
    RUN_TEST(test_module_foreach_count);
    RUN_TEST(test_write_sync_applet);
    RUN_TEST(test_write_sync_ip);
    RUN_TEST(test_write_sync_ip_cb_sees_new_cache);
    RUN_TEST(test_write_sync_ip_fails_keeps_cache);
    RUN_TEST(test_write_sync_ip_no_write_cb);
    RUN_TEST(test_write_sync_applet_no_write_cb);
    RUN_TEST(test_notify_reports_effective_value);
    RUN_TEST(test_write_raw_uint);
    RUN_TEST(test_write_raw_too_long);
    RUN_TEST(test_write_raw_null_data);
    RUN_TEST(test_write_raw_zero_len);
    RUN_TEST(test_exec_no_callback);
    RUN_TEST(test_exec_notifies_with_payload);
    RUN_TEST(test_load_one_notifies);
    RUN_TEST(test_reset_one_notifies);
    RUN_TEST(test_blob_write_read);
    RUN_TEST(test_string_write_read);
    RUN_TEST(test_string_truncation);
    RUN_TEST(test_string_save_load);
    RUN_TEST(test_string_macro_ptr_buffer_reset_no_ub);
    RUN_TEST(test_write_rejects_blob_and_string);
    RUN_TEST(test_write_raw_blob_validates);
    RUN_TEST(test_write_ex_clamp);
    RUN_TEST(test_write_ex_unknown_flag);
    RUN_TEST(test_write_bytes_value_clamp);
    RUN_TEST(test_reapply_revalidates);
    RUN_TEST(test_write_raw_string_bounded_by_len);
    RUN_TEST(test_cache_restore_checks_record_len);
}
