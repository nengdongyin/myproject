#include "test_common.h"
#include <string.h>

DEF_TEST_UINT  (ru32, TID_APPLET_UINT,   PARAM_FLAG_PERSIST, 100, 0, 200);
PARAM_BOOL     (rboo, TID_APPLET_BOOL,   PARAM_FLAG_PERSIST, true);
PARAM_BOOL     (rrdo, TID_APPLET_RDONLY, PARAM_FLAG_PERSIST|PARAM_FLAG_READONLY, false);

PARAM_IP_UINT  (ripu, TID_IP_UINT,  PARAM_FLAG_PERSIST, 500,   500,   500);
PARAM_IP_FLOAT (ripf, TID_IP_FLOAT, PARAM_FLAG_PERSIST, 2.0f,  2.0f,  2.0f);

static uint8_t g_blob_buf2[TEST_BLOB_SIZE];
PARAM_BLOB     (rblb, TID_APPLET_BLOB, PARAM_FLAG_PERSIST, g_blob_buf2, TEST_BLOB_SIZE);

PARAM_TABLE(applet_params, &ru32.base, &rboo.base, &rrdo.base, &rblb.base);
PARAM_TABLE(ip_params, &ripu.base, &ripf.base);

PARAM_MODULE_DEFINE(test_applet, TEST_MODULE_APPLET, "ResetApplet", .write = mock_apply_ok);
IP_DRIVER_DEFINE(test_ip, TEST_MODULE_IP, "ResetIP", .write = mock_ip_write_ok);

static void register_all(void)
{
    param_module_register(&test_applet_module, applet_params, PARAM_COUNT(applet_params));
    param_module_register(&test_ip_module, ip_params, PARAM_COUNT(ip_params));
    test_reset_entry(&ru32.base); test_reset_entry(&rboo.base);
    test_reset_entry(&rrdo.base); test_reset_entry(&rblb.base);
    test_reset_entry(&ripu.base); test_reset_entry(&ripf.base);
}

/* ================================================================
 *  reset_one 测试
 * ================================================================ */
void test_reset_one_uint(void) {
    register_all();

    param_value_t v = { .u32 = 999 };
    param_write(TID_APPLET_UINT, v);

    TEST_ASSERT_PARAM_OK(param_reset_one(TID_APPLET_UINT));

    param_read(TID_APPLET_UINT, &v);
    TEST_ASSERT_EQUAL_UINT32(100, v.u32);
    TEST_ASSERT_DIRTY(&ru32.base, 1);
}

void test_reset_one_bool(void) {
    register_all();

    param_value_t v = { .b = false };
    param_write(TID_APPLET_BOOL, v);

    TEST_ASSERT_PARAM_OK(param_reset_one(TID_APPLET_BOOL));

    param_read(TID_APPLET_BOOL, &v);
    TEST_ASSERT_TRUE(v.b);
}

void test_reset_one_ip(void) {
    register_all();

    param_value_t v = { .u32 = 999 };
    param_write(TID_IP_UINT, v);

    TEST_ASSERT_PARAM_OK(param_reset_one(TID_IP_UINT));

    param_read(TID_IP_UINT, &v);
    TEST_ASSERT_EQUAL_UINT32(500, v.u32);
    TEST_ASSERT_DIRTY(&ripu.base, 1);
}

void test_reset_one_float(void) {
    register_all();

    param_value_t v = { .f32 = 5.0f };
    param_write(TID_APPLET_UINT, v);

    TEST_ASSERT_PARAM_OK(param_reset_one(TID_APPLET_UINT));

    param_read(TID_APPLET_UINT, &v);
    TEST_ASSERT_EQUAL_UINT32(100, v.u32);
}

void test_reset_one_not_found(void) {
    register_all();
    TEST_ASSERT_PARAM_ERR(param_reset_one(0xDEADBEEFu), PARAM_ERR_INVALID_ID);
}

/* ================================================================
 *  reset_all 测试
 * ================================================================ */
void test_reset_all_clears_all_dirty(void) {
    register_all();

    param_value_t v = { .u32 = 1 };
    param_write(TID_APPLET_UINT, v);
    param_write(TID_IP_UINT, v);

    TEST_ASSERT_PARAM_OK(param_reset_all());

    /* reset_all 的 INIT 阶段会调用 ip_module_init, 按设计把 IP 可写
       参数重新标脏 (恢复出厂后默认值仍需下发 FPGA)。此处 dirty_count
       应为 IP 可写参数数 (ripu + ripf = 2), 而非 0。 */
    param_stats_t stats;
    param_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT16(2, stats.dirty_count);

    /* 一次 flush 把默认值下发硬件后, 脏位全部收敛 */
    TEST_ASSERT_PARAM_OK(param_flush());
    param_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT16(0, stats.dirty_count);

    param_read(TID_APPLET_UINT, &v);
    TEST_ASSERT_EQUAL_UINT32(100, v.u32);
    param_read(TID_IP_UINT, &v);
    TEST_ASSERT_EQUAL_UINT32(500, v.u32);
}

void test_reset_all_ip_dirty_cleared(void) {
    register_all();

    param_value_t v = { .f32 = 5.0f };
    param_write(TID_IP_FLOAT, v);

    TEST_ASSERT_PARAM_OK(param_reset_all());

    param_read(TID_IP_FLOAT, &v);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 2.0f, v.f32);
}

void test_reset_one_ip_marks_dirty(void) {
    register_all();

    param_value_t v = { .u32 = 999 };
    param_write(TID_IP_UINT, v);

    TEST_ASSERT_PARAM_OK(param_reset_one(TID_IP_UINT));

    param_stats_t stats;
    param_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT16(1, stats.dirty_count);
}

/* ================================================================
 *  BLOB reset
 * ================================================================ */
void test_reset_blob(void) {
    register_all();

    uint8_t new_data[TEST_BLOB_SIZE];
    memset(new_data, 0xCC, TEST_BLOB_SIZE);
    TEST_ASSERT_PARAM_OK(param_write_raw(TID_APPLET_BLOB, new_data, sizeof(new_data)));

    TEST_ASSERT_PARAM_OK(param_reset_one(TID_APPLET_BLOB));
}

/* ================================================================
 *  项目默认值覆盖 — param_set_default / param_apply_defaults
 *
 *  与"用 param_write_cache 写一遍"的区别正是本组用例要锁住的契约:
 *   1. 改的是默认值本身 → reset 回到**项目**默认值;
 *   2. 不标脏、不 notify (那不是"用户修改");
 *   3. 非法默认值被拒绝 (配置错误不静默裁剪);
 *   4. 在 LOAD 之前应用 → 有存储记录者被存储值覆盖, 无记录者用项目默认值。
 * ================================================================ */

/** @brief 计数用 notify 回调 */
static int s_def_notify_count;
static void def_notify_cb(uint32_t param_id, param_value_t new_value)
{
    (void)param_id;
    (void)new_value;
    s_def_notify_count++;
}

/**
 * @brief 把被测条目的默认值恢复成编译期默认值
 *
 * @note test_reset_entry() 只恢复 cache 与 dirty, **不恢复 default_val** ——
 *       param_set_default 会永久改写静态条目的默认值, 必须显式复位,
 *       否则用例结果会隐性依赖执行顺序 (与 test_other.c 对 param_set_range 的
 *       处理同理)。
 */
static void test_defaults_restore(void)
{
    (void)param_set_default(TID_APPLET_UINT, (param_value_t){ .u32 = 100 }); /* ru32 */
    (void)param_set_default(TID_APPLET_BOOL, (param_value_t){ .b  = true }); /* rboo */
}

/* 1) 立即生效 + 静默 (不脏不通知) */
void test_set_default_applies_silently(void)
{
    /* 本用例需要观察 notify, 故用带通知回调的 init 重建框架 */
    param_deinit();
    s_def_notify_count = 0;
    param_init(&g_mock_storage, def_notify_cb);
    register_all();
    test_defaults_restore();
    TEST_ASSERT_PARAM_OK(param_set_default(TID_APPLET_UINT, (param_value_t){ .u32 = 150 }));

    param_value_t v;
    TEST_ASSERT_PARAM_OK(param_read(TID_APPLET_UINT, &v));
    TEST_ASSERT_EQUAL_UINT32(150, v.u32);        /* 缓存立即生效 */

    param_stats_t st;
    TEST_ASSERT_PARAM_OK(param_get_stats(&st));
    TEST_ASSERT_EQUAL_UINT16(0, st.dirty_count); /* 不标脏 (不是用户写) */
    TEST_ASSERT_EQUAL_INT(0, s_def_notify_count);/* 不 notify */
}

/* 2) 核心语义: reset 回到项目默认值, 而不是编译期默认值 */
void test_set_default_changes_reset_target(void)
{
    register_all();
    test_defaults_restore();

    TEST_ASSERT_PARAM_OK(param_set_default(TID_APPLET_UINT, (param_value_t){ .u32 = 150 }));
    /* 用户改成 42, 再"恢复出厂" */
    TEST_ASSERT_PARAM_OK(param_write(TID_APPLET_UINT, (param_value_t){ .u32 = 42 }));
    TEST_ASSERT_PARAM_OK(param_reset_one(TID_APPLET_UINT));

    param_value_t v;
    TEST_ASSERT_PARAM_OK(param_read(TID_APPLET_UINT, &v));
    TEST_ASSERT_EQUAL_UINT32(150, v.u32); /* 150 而非编译期默认 100 */
}

/* 3) 越界拒绝, 且原默认值不受影响 */
void test_set_default_out_of_range_rejected(void)
{
    register_all();
    test_defaults_restore();

    TEST_ASSERT_PARAM_ERR(param_set_default(TID_APPLET_UINT, (param_value_t){ .u32 = 201 }),
                          PARAM_ERR_OUT_OF_RANGE); /* ru32 范围 [0,200] */

    TEST_ASSERT_PARAM_OK(param_reset_one(TID_APPLET_UINT));
    param_value_t v;
    TEST_ASSERT_PARAM_OK(param_read(TID_APPLET_UINT, &v));
    TEST_ASSERT_EQUAL_UINT32(100, v.u32); /* 仍是编译期默认值 */
}

/* 4) 不支持的形态: BLOB (单缓冲) / 未知 ID / 未初始化 */
void test_set_default_rejects_unsupported(void)
{
    register_all();
    test_defaults_restore();

    TEST_ASSERT_PARAM_ERR(param_set_default(TID_APPLET_BLOB, (param_value_t){ .ptr = NULL }),
                          PARAM_ERR_NOT_SUPPORTED);
    TEST_ASSERT_PARAM_ERR(param_set_default(0xDEADBEEFu, (param_value_t){ .u32 = 1 }),
                          PARAM_ERR_INVALID_ID);

    param_deinit();
    TEST_ASSERT_PARAM_ERR(param_set_default(TID_APPLET_UINT, (param_value_t){ .u32 = 1 }),
                          PARAM_ERR_NOT_INITIALIZED);
}

/* 5) 表形式: 逐条应用; 空表 = 空操作; 单条坏配置不连带其它条目 */
void test_apply_defaults_table(void)
{
    register_all();
    test_defaults_restore();

    const param_default_t tbl[] = {
        { TID_APPLET_UINT, { .u32 = 180 } },
        { TID_IP_UINT,     { .u32 = 500 } }, /* 固定 [500,500], 合法 */
    };
    TEST_ASSERT_PARAM_OK(param_apply_defaults(tbl, 2));

    param_value_t v;
    TEST_ASSERT_PARAM_OK(param_read(TID_APPLET_UINT, &v));
    TEST_ASSERT_EQUAL_UINT32(180, v.u32);

    /* 空表与 NULL 表都是空操作 (本产品不做覆盖的场景) */
    TEST_ASSERT_PARAM_OK(param_apply_defaults(NULL, 0));
    TEST_ASSERT_PARAM_OK(param_apply_defaults(tbl, 0));

    /* 含一条非法项: 返回首个错误, 但其后合法条目照常应用 */
    const param_default_t mixed[] = {
        { TID_APPLET_UINT, { .u32 = 999 } },  /* 越界 → 失败 */
        { TID_APPLET_BOOL, { .b = false } },  /* 合法 → 生效 */
    };
    TEST_ASSERT_PARAM_ERR(param_apply_defaults(mixed, 2), PARAM_ERR_OUT_OF_RANGE);

    TEST_ASSERT_PARAM_OK(param_read(TID_APPLET_UINT, &v));
    TEST_ASSERT_EQUAL_UINT32(180, v.u32);      /* 失败条目未改动 */
    TEST_ASSERT_PARAM_OK(param_read(TID_APPLET_BOOL, &v));
    TEST_ASSERT_FALSE(v.b);                    /* 后续条目仍被应用 */
}

/* 6) 启动顺序语义: LOAD 之前覆盖 → 有存储记录者被存储值覆盖
 *
 *  这条是整套机制的关键: 放在 param_load_all 之前就不需要判断
 *  "存储里有没有这条记录", LOAD 天然区分。 */
void test_apply_defaults_before_load_stored_wins(void)
{
    /* 预置一条存储记录: TID_APPLET_UINT = 42 (模拟用户现场改过) */
    uint32_t stored = 42;
    mock_storage_preset(TID_APPLET_UINT, (const uint8_t *)&stored, sizeof(stored));

    register_all();
    test_defaults_restore();

    const param_default_t tbl[] = {
        { TID_APPLET_UINT, { .u32 = 150 } }, /* 有存储记录 → 应被 42 覆盖 */
        { TID_APPLET_BOOL, { .b  = false } }, /* 无记录 → 应保留项目默认值 */
    };
    TEST_ASSERT_PARAM_OK(param_apply_defaults(tbl, 2));

    /* 模拟启动序列中的 LOAD 阶段 */
    TEST_ASSERT_PARAM_OK(param_load_all());

    param_value_t v;
    TEST_ASSERT_PARAM_OK(param_read(TID_APPLET_UINT, &v));
    TEST_ASSERT_EQUAL_UINT32(42, v.u32);      /* 存储值胜出 (用户值不被冲掉) */
    TEST_ASSERT_PARAM_OK(param_read(TID_APPLET_BOOL, &v));
    TEST_ASSERT_FALSE(v.b);                   /* 无记录 → 项目默认值生效 */

    /* 但"恢复出厂"仍应回到项目默认值 */
    TEST_ASSERT_PARAM_OK(param_reset_one(TID_APPLET_UINT));
    TEST_ASSERT_PARAM_OK(param_read(TID_APPLET_UINT, &v));
    TEST_ASSERT_EQUAL_UINT32(150, v.u32);

    /* 用例间隔离: 默认值会永久改写静态条目, 恢复成编译期默认值 */
    test_defaults_restore();
}

void run_test_reset(void)
{
    RUN_TEST(test_reset_one_uint);
    RUN_TEST(test_reset_one_bool);
    RUN_TEST(test_reset_one_ip);
    RUN_TEST(test_reset_one_float);
    RUN_TEST(test_reset_one_not_found);
    RUN_TEST(test_reset_all_clears_all_dirty);
    RUN_TEST(test_reset_all_ip_dirty_cleared);
    RUN_TEST(test_reset_one_ip_marks_dirty);
    RUN_TEST(test_reset_blob);
    RUN_TEST(test_set_default_applies_silently);
    RUN_TEST(test_set_default_changes_reset_target);
    RUN_TEST(test_set_default_out_of_range_rejected);
    RUN_TEST(test_set_default_rejects_unsupported);
    RUN_TEST(test_apply_defaults_table);
    RUN_TEST(test_apply_defaults_before_load_stored_wins);
}
