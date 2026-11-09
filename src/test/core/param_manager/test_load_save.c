#include "test_common.h"
#include <string.h>

DEF_TEST_UINT  (lu32,  TID_APPLET_UINT,    PARAM_FLAG_PERSIST, 100, 0, 200);
DEF_TEST_FLOAT (lf32,  TID_APPLET_FLOAT,   PARAM_FLAG_PERSIST, 1.5f, 0.0f, 3.0f);
PARAM_BOOL     (lboo,  TID_APPLET_BOOL,    PARAM_FLAG_PERSIST, true);
PARAM_UINT     (lnop,  TID_APPLET_NOPERSIST, 0, 50, 0, 100);

PARAM_IP_UINT  (lipu,  TID_IP_UINT,  PARAM_FLAG_PERSIST, 500,   500,   500);
PARAM_IP_FLOAT (lipf,  TID_IP_FLOAT, PARAM_FLAG_PERSIST, 2.0f,  2.0f,  2.0f);

static uint8_t g_blob_data[TEST_BLOB_SIZE];
PARAM_BLOB     (lblb,  TID_APPLET_BLOB, PARAM_FLAG_PERSIST, g_blob_data, TEST_BLOB_SIZE);

PARAM_TABLE(applet_params, &lu32.base, &lf32.base, &lboo.base, &lnop.base, &lblb.base);
PARAM_TABLE(ip_params, &lipu.base, &lipf.base);
PARAM_MODULE_DEFINE(test_applet, TEST_MODULE_APPLET, "LSApplet", .write = mock_apply_ok);
IP_DRIVER_DEFINE(test_ip, TEST_MODULE_IP, "LSIP", .write = mock_ip_write_ok);

static void register_all(void)
{
    param_module_register(&test_applet_module, applet_params, PARAM_COUNT(applet_params));
    param_module_register(&test_ip_module, ip_params, PARAM_COUNT(ip_params));
    test_reset_entry(&lu32.base);  test_reset_entry(&lf32.base);
    test_reset_entry(&lboo.base);  test_reset_entry(&lnop.base);
    test_reset_entry(&lblb.base);
    test_reset_entry(&lipu.base);  test_reset_entry(&lipf.base);
}

/* ================================================================
 *  Load 测试
 * ================================================================ */
void test_load_one_valid(void) {
    register_all();

    param_value_t preset = { .u32 = 250 };
    mock_storage_preset(TID_APPLET_UINT, (const uint8_t *)&preset, sizeof(preset));

    TEST_ASSERT_PARAM_OK(param_load_one(TID_APPLET_UINT));

    param_value_t v;
    param_read(TID_APPLET_UINT, &v);
    TEST_ASSERT_EQUAL_UINT32(200, v.u32);
}

void test_load_one_no_persist(void) {
    register_all();

    param_value_t preset = { .u32 = 99 };
    mock_storage_preset(TID_APPLET_NOPERSIST, (const uint8_t *)&preset, sizeof(preset));

    TEST_ASSERT_PARAM_OK(param_load_one(TID_APPLET_NOPERSIST));

    param_value_t v;
    param_read(TID_APPLET_NOPERSIST, &v);
    TEST_ASSERT_EQUAL_UINT32(50, v.u32);
}

void test_load_one_not_found(void) {
    register_all();
    TEST_ASSERT_PARAM_ERR(param_load_one(0xDEADBEEFu), PARAM_ERR_INVALID_ID);
}

void test_load_float(void) {
    register_all();

    param_value_t preset = { .f32 = 2.75f };
    mock_storage_preset(TID_APPLET_FLOAT, (const uint8_t *)&preset, sizeof(preset));

    TEST_ASSERT_PARAM_OK(param_load_one(TID_APPLET_FLOAT));

    param_value_t v;
    param_read(TID_APPLET_FLOAT, &v);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 2.75f, v.f32);
}

void test_load_bool(void) {
    register_all();

    param_value_t preset = { .b = false };
    mock_storage_preset(TID_APPLET_BOOL, (const uint8_t *)&preset, sizeof(preset));

    TEST_ASSERT_PARAM_OK(param_load_one(TID_APPLET_BOOL));

    param_value_t v;
    param_read(TID_APPLET_BOOL, &v);
    TEST_ASSERT_FALSE(v.b);
}

void test_load_ip_param(void) {
    register_all();

    param_value_t preset = { .u32 = 888 };
    mock_storage_preset(TID_IP_UINT, (const uint8_t *)&preset, sizeof(preset));

    TEST_ASSERT_PARAM_OK(param_load_one(TID_IP_UINT));

    param_value_t v;
    param_read(TID_IP_UINT, &v);
    TEST_ASSERT_EQUAL_UINT32(888, v.u32);
}

void test_load_storage_fails(void) {
    register_all();
    mock_storage_set_load_ret(PARAM_ERR_FLASH_FAIL);

    TEST_ASSERT_PARAM_ERR(param_load_one(TID_APPLET_UINT), PARAM_ERR_FLASH_FAIL);

    param_value_t v;
    param_read(TID_APPLET_UINT, &v);
    TEST_ASSERT_EQUAL_UINT32(100, v.u32);
}

void test_load_all_multiple(void) {
    register_all();

    param_value_t v_u32 = { .u32 = 111 };
    param_value_t v_f32 = { .f32 = 2.25f };
    param_value_t v_bool = { .b = false };
    param_value_t v_ip = { .u32 = 999 };

    mock_storage_preset(TID_APPLET_UINT,  (const uint8_t *)&v_u32, sizeof(v_u32));
    mock_storage_preset(TID_APPLET_FLOAT, (const uint8_t *)&v_f32, sizeof(v_f32));
    mock_storage_preset(TID_APPLET_BOOL,  (const uint8_t *)&v_bool, sizeof(v_bool));
    mock_storage_preset(TID_IP_UINT,       (const uint8_t *)&v_ip, sizeof(v_ip));

    param_load_all();

    param_value_t v;
    param_read(TID_APPLET_UINT, &v);
    TEST_ASSERT_EQUAL_UINT32(111, v.u32);
    param_read(TID_APPLET_FLOAT, &v);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 2.25f, v.f32);
    param_read(TID_APPLET_BOOL, &v);
    TEST_ASSERT_FALSE(v.b);
    param_read(TID_IP_UINT, &v);
    TEST_ASSERT_EQUAL_UINT32(999, v.u32);
}

void test_load_all_partial_flash(void) {
    register_all();

    param_value_t preset = { .u32 = 77 };
    mock_storage_preset(TID_APPLET_UINT, (const uint8_t *)&preset, sizeof(preset));

    param_load_all();

    param_value_t v;
    param_read(TID_APPLET_UINT, &v);
    TEST_ASSERT_EQUAL_UINT32(77, v.u32);

    param_read(TID_APPLET_BOOL, &v);
    TEST_ASSERT_TRUE(v.b);
}

/* ================================================================
 *  短记录恢复 — 后端只写实际读到的字节 (模拟真实 Flash, 不主动补零)
 *
 *  mock_storage_load 会替调用方把剩余字节清零, 盖上适配层的补零逻辑;
 *  因此这里换一个"不补零"的 load 实现, 让 value_type_load / blob_load
 *  的尾部处理真正被覆盖到。
 * ================================================================ */
static param_storage_drv_t g_nozero_drv;

static int nozero_load(void *ctx, uint32_t id, uint8_t *data, uint16_t len,
                       uint16_t *read_len)
{
    (void)ctx;
    for (uint16_t i = 0; i < g_mock_kv_count; i++) {
        if (g_mock_kv[i].id != id)
            continue;
        uint16_t n = (len < g_mock_kv[i].len) ? len : g_mock_kv[i].len;
        memcpy(data, g_mock_kv[i].data, n); /* 刻意不补零 */
        if (read_len) *read_len = n;
        return PARAM_OK;
    }
    return PARAM_ERR_NOT_FOUND;
}

/** @brief 临时把存储后端换成"不补零"版本 (save/erase 等复用 g_mock_storage) */
static void use_nozero_storage(void)
{
    g_nozero_drv = g_mock_storage;
    g_nozero_drv.load = nozero_load;
    param_set_storage(&g_nozero_drv);
}

void test_load_blob_short_record_zeros_tail(void) {
    register_all();

    /* 缓存先填 0x55, 用于区分"补零"与"残留旧值" */
    uint8_t old[TEST_BLOB_SIZE];
    memset(old, 0x55, sizeof(old));
    TEST_ASSERT_PARAM_OK(param_write_raw(TID_APPLET_BLOB, old, sizeof(old)));

    /* 存储里只有前 16 字节 (短记录: 旧固件 / 数据损坏 / 格式变更) */
    uint8_t short_rec[16];
    memset(short_rec, 0xAA, sizeof(short_rec));
    mock_storage_preset(TID_APPLET_BLOB, short_rec, sizeof(short_rec));

    use_nozero_storage();
    TEST_ASSERT_PARAM_OK(param_load_one(TID_APPLET_BLOB));

    uint8_t out[TEST_BLOB_SIZE];
    uint16_t out_len = sizeof(out);
    TEST_ASSERT_PARAM_OK(param_read_raw(TID_APPLET_BLOB, out, &out_len));

    /* 前 16 字节 = 记录内容, 其余必须补零 (若不清零尾部, 这里会残留 0x55) */
    uint8_t expect[TEST_BLOB_SIZE];
    memset(expect, 0x00, sizeof(expect));
    memset(expect, 0xAA, 16);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expect, out, TEST_BLOB_SIZE);

    param_set_storage(&g_mock_storage);
}

void test_load_uint_short_record_zero_padded(void) {
    register_all();

    /* 旧固件按 2 字节写入, 本构建 param_value_t 更宽: 高位必须为 0 */
    uint16_t short_rec = 7;
    mock_storage_preset(TID_APPLET_UINT, (const uint8_t *)&short_rec, sizeof(short_rec));

    use_nozero_storage();
    TEST_ASSERT_PARAM_OK(param_load_one(TID_APPLET_UINT));

    param_value_t v;
    param_read_cache(TID_APPLET_UINT, &v);
    TEST_ASSERT_EQUAL_UINT32(7, v.u32); /* 高位若为残留垃圾则不等于 7 */

    param_set_storage(&g_mock_storage);
}

/* ================================================================
 *  Save 测试
 * ================================================================ */
void test_save_one_writes_storage(void) {
    register_all();

    param_value_t v = { .u32 = 42 };
    param_write(TID_APPLET_UINT, v);

    mock_storage_set_save_ret(0);
    TEST_ASSERT_PARAM_OK(param_save_one(TID_APPLET_UINT));
}

void test_save_all_skips_non_persist(void) {
    register_all();

    param_value_t v = { .u32 = 99 };
    param_write(TID_APPLET_NOPERSIST, v);

    TEST_ASSERT_PARAM_OK(param_save_all());
}

void test_save_float(void) {
    register_all();

    param_value_t v = { .f32 = 2.0f };
    param_write(TID_APPLET_FLOAT, v);

    TEST_ASSERT_PARAM_OK(param_save_one(TID_APPLET_FLOAT));
}

void test_save_bool(void) {
    register_all();

    param_value_t v = { .b = false };
    param_write(TID_APPLET_BOOL, v);

    TEST_ASSERT_PARAM_OK(param_save_one(TID_APPLET_BOOL));
}

void test_save_blob(void) {
    register_all();

    uint8_t data[TEST_BLOB_SIZE];
    memset(data, 0xAB, TEST_BLOB_SIZE);
    /* BLOB 走带长度的专用入口 param_write_raw (param_write 拒收 BLOB/STRING) */
    TEST_ASSERT_PARAM_OK(param_write_raw(TID_APPLET_BLOB, data, sizeof(data)));

    TEST_ASSERT_PARAM_OK(param_save_one(TID_APPLET_BLOB));
}

/* ================================================================
 *  Load/Save 往返
 * ================================================================ */
void test_save_load_roundtrip_uint(void) {
    register_all();

    param_value_t write_val = { .u32 = 123 };
    param_write(TID_APPLET_UINT, write_val);
    param_save_one(TID_APPLET_UINT);

    param_deinit();
    mock_callbacks_reset();
    param_init(&g_mock_storage, NULL);
    register_all();

    param_load_all();

    param_value_t read_val;
    param_read(TID_APPLET_UINT, &read_val);
    TEST_ASSERT_EQUAL_UINT32(123, read_val.u32);
}

void test_save_load_roundtrip_float(void) {
    register_all();

    param_value_t write_val = { .f32 = 2.5f };
    param_write(TID_APPLET_FLOAT, write_val);
    param_save_one(TID_APPLET_FLOAT);

    param_deinit();
    mock_callbacks_reset();
    param_init(&g_mock_storage, NULL);
    register_all();

    param_load_all();

    param_value_t read_val;
    param_read(TID_APPLET_FLOAT, &read_val);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 2.5f, read_val.f32);
}

/* ================================================================
 *  批量通路 — load_all 的 sink 端口 与 save_begin/save_commit 事务
 *
 *  这些用例把后端换成 g_mock_storage3 (实现了两个批量通路),
 *  既有用例仍用 g_mock_storage 覆盖逐条兜底路径。
 * ================================================================ */
void test_save_all_uses_batch_transaction(void) {
    register_all();
    mock_storage3_reset();
    param_set_storage(&g_mock_storage3);

    param_value_t v = { .u32 = 150 };
    TEST_ASSERT_PARAM_OK(param_write(TID_APPLET_UINT, v));

    TEST_ASSERT_PARAM_OK(param_save_all());

    /* 事务边界: begin / commit 各恰好一次, 逐条 save 由框架发起 */
    TEST_ASSERT_EQUAL_INT(1, g_mock3_begin_calls);
    TEST_ASSERT_EQUAL_INT(1, g_mock3_commit_calls);
    TEST_ASSERT_TRUE(g_mock3_save_calls >= 1);

    /* PERSIST 过滤依旧生效 (由 param_data_ops 层负责, 与批量无关) */
    TEST_ASSERT_NOT_NULL(mock_storage3_entry(TID_APPLET_UINT));
    TEST_ASSERT_NULL(mock_storage3_entry(TID_APPLET_NOPERSIST));
}

void test_save_all_batch_begin_fails(void) {
    register_all();
    mock_storage3_reset();
    param_set_storage(&g_mock_storage3);
    g_mock3_begin_ret = -1; /* 事务开不起来 */

    param_value_t v = { .u32 = 150 };
    param_write(TID_APPLET_UINT, v);

    TEST_ASSERT_PARAM_ERR(param_save_all(), PARAM_ERR_FLASH_FAIL);
    TEST_ASSERT_EQUAL_INT(1, g_mock3_begin_calls);
    /* 不退化: 不开事务就不该继续逐条写, 也不该提交 */
    TEST_ASSERT_EQUAL_INT(0, g_mock3_save_calls);
    TEST_ASSERT_EQUAL_INT(0, g_mock3_commit_calls);
}

void test_save_all_batch_commit_fails(void) {
    register_all();
    mock_storage3_reset();
    param_set_storage(&g_mock_storage3);
    g_mock3_commit_ret = -1; /* 落盘失败 */

    param_value_t v = { .u32 = 150 };
    param_write(TID_APPLET_UINT, v);

    TEST_ASSERT_PARAM_ERR(param_save_all(), PARAM_ERR_FLASH_FAIL);
    TEST_ASSERT_EQUAL_INT(1, g_mock3_begin_calls);
    TEST_ASSERT_EQUAL_INT(1, g_mock3_commit_calls);
}

void test_load_all_via_sink(void) {
    register_all();
    mock_storage3_reset();
    param_set_storage(&g_mock_storage3);

    param_value_t preset = { .u32 = 173 };
    mock_storage3_preset(TID_APPLET_UINT, (const uint8_t *)&preset, sizeof(preset));

    TEST_ASSERT_PARAM_OK(param_load_all());

    /* 批量通路走通: sink 收到 1 条, 未发生逐条 load */
    TEST_ASSERT_EQUAL_UINT16(1, g_mock3_restored);
    TEST_ASSERT_EQUAL_INT(0, g_mock3_load_one_calls);

    param_value_t v;
    param_read(TID_APPLET_UINT, &v);
    TEST_ASSERT_EQUAL_UINT32(173, v.u32);
}

void test_load_all_sink_reject_falls_back(void) {
    register_all();
    mock_storage3_reset();
    param_set_storage(&g_mock_storage3);

    param_value_t preset = { .u32 = 186 };
    mock_storage3_preset(TID_APPLET_UINT, (const uint8_t *)&preset, sizeof(preset));
    g_mock3_push_short_blob = 1; /* 一条长度不符的 BLOB 记录 → restore 被拒 */

    TEST_ASSERT_PARAM_OK(param_load_all());

    /* 被拒 → 后端返回 >0 → 框架回退逐条 load (而不是静默丢弃) */
    TEST_ASSERT_TRUE(g_mock3_load_one_calls > 0);

    param_value_t v;
    param_read(TID_APPLET_UINT, &v);
    TEST_ASSERT_EQUAL_UINT32(186, v.u32);
}

void run_test_load_save(void)
{
    RUN_TEST(test_load_one_valid);
    RUN_TEST(test_load_one_no_persist);
    RUN_TEST(test_load_one_not_found);
    RUN_TEST(test_load_float);
    RUN_TEST(test_load_bool);
    RUN_TEST(test_load_ip_param);
    RUN_TEST(test_load_storage_fails);
    RUN_TEST(test_load_all_multiple);
    RUN_TEST(test_load_all_partial_flash);
    RUN_TEST(test_load_blob_short_record_zeros_tail);
    RUN_TEST(test_load_uint_short_record_zero_padded);
    RUN_TEST(test_save_one_writes_storage);
    RUN_TEST(test_save_all_skips_non_persist);
    RUN_TEST(test_save_float);
    RUN_TEST(test_save_bool);
    RUN_TEST(test_save_blob);
    RUN_TEST(test_save_load_roundtrip_uint);
    RUN_TEST(test_save_load_roundtrip_float);
    RUN_TEST(test_save_all_uses_batch_transaction);
    RUN_TEST(test_save_all_batch_begin_fails);
    RUN_TEST(test_save_all_batch_commit_fails);
    RUN_TEST(test_load_all_via_sink);
    RUN_TEST(test_load_all_sink_reject_falls_back);
}
