/**
 * @file test_mkv.c
 * @brief mkv 极简 KV 库原生单测 — 重点回归 GC 数据丢失缺陷
 *
 * 通过 RAM-backed mock FAL 驱动 mkv 真实逻辑 (非 mock storage)。
 * 覆盖:
 *   - set/get/del 基础往返
 *   - #1 回归: >502B 的 value 在 GC 后必须完整读回
 *   - #2 回归: >256 个 distinct id 在 GC 后必须全部读回
 *   - 最小 sector (256B) 上反复 GC 的边界健壮性 (probe_erased 边界钳制)
 */

#include "unity.h"
#include "test_suite.h"
#include "mkv.h"
#include "mock_fal.h"

/* ── 每个用例独立复位 (默认 4KB×2 sector; 单用例可覆盖) ── */
void mkv_setup(void)    { mock_fal_reset(4096, 4096 * 2); }
void mkv_teardown(void) {}

/* ═════════════════════════════ 基础往返 ═══════════════ */

static void test_roundtrip_basic(void)
{
    mkv_t kv;
    TEST_ASSERT_EQUAL_INT(0, mkv_init(&kv, MOCK_PART_NAME));

    uint8_t in[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    uint8_t out[16] = {0};
    TEST_ASSERT_EQUAL_INT(0, mkv_set(&kv, 0xABCDu, in, sizeof(in)));
    TEST_ASSERT_EQUAL_INT(16, mkv_get(&kv, 0xABCDu, out, sizeof(out)));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(in, out, 16);

    /* 不存在的 id → 0 */
    TEST_ASSERT_EQUAL_INT(0, mkv_get(&kv, 0xFFFFu, out, sizeof(out)));

    /* 删除后再读 → 0 */
    TEST_ASSERT_EQUAL_INT(0, mkv_del(&kv, 0xABCDu));
    TEST_ASSERT_EQUAL_INT(0, mkv_get(&kv, 0xABCDu, out, sizeof(out)));
}

/* ═════════════════════════ #1 回归: 大 value GC 不丢 ══════════════ */

static void test_gc_preserves_large_value(void)
{
    mkv_t kv;
    TEST_ASSERT_EQUAL_INT(0, mkv_init(&kv, MOCK_PART_NAME));

    /* 600B blob —— 旧实现 GC 的 512B 缓冲会静默丢弃它 */
    uint8_t blob[600];
    for (int i = 0; i < (int)sizeof(blob); i++)
        blob[i] = (uint8_t)(i * 7 + 13);
    TEST_ASSERT_EQUAL_INT(0, mkv_set(&kv, 1, blob, (uint16_t)sizeof(blob)));

    /* 写满 sector 迫使 GC: 每条 8B value, distinct id */
    uint32_t seq0 = kv.active_seq;
    uint32_t id = 100;
    uint8_t small[8];
    while (kv.active_seq == seq0 && id < 400)
    {
        for (int j = 0; j < (int)sizeof(small); j++) small[j] = (uint8_t)(id + j);
        (void)mkv_set(&kv, id, small, (uint16_t)sizeof(small));
        id++;
    }
    TEST_ASSERT_TRUE_MESSAGE(kv.active_seq > seq0, "GC 未触发");

    /* GC 后 600B blob 必须完整读回 */
    uint8_t out[600];
    TEST_ASSERT_EQUAL_INT(600, mkv_get(&kv, 1, out, 600));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(blob, out, 600);
}

/* ═══════════════════ #2 回归: >256 个 id GC 不丢 ═══════════════ */

static void test_gc_preserves_many_ids(void)
{
    mkv_t kv;
    TEST_ASSERT_EQUAL_INT(0, mkv_init(&kv, MOCK_PART_NAME));

    /* 每条 2B value (总 12B)，写满 sector: 触发 GC 前可写 ~340 条 */
    uint32_t seq0 = kv.active_seq;
    uint32_t id = 1;
    int written = 0;
    while (kv.active_seq == seq0 && id < 500)
    {
        uint8_t v[2] = { (uint8_t)id, (uint8_t)(id >> 8) };
        if (mkv_set(&kv, id, v, 2) == 0) written++;
        id++;
    }
    TEST_ASSERT_TRUE_MESSAGE(written > 256, "未达到 >256 个 distinct id, 无法覆盖 #2");
    TEST_ASSERT_TRUE_MESSAGE(kv.active_seq > seq0, "GC 未触发");

    /* GC 后每个已写入的 id 都必须可读回 (旧实现 map=256 会丢第 257 条起) */
    for (uint32_t i = 1; i <= (uint32_t)written; i++)
    {
        uint8_t out[2];
        TEST_ASSERT_EQUAL_INT(2, mkv_get(&kv, i, out, 2));
        TEST_ASSERT_EQUAL_UINT8((uint8_t)i, out[0]);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)(i >> 8), out[1]);
    }
}

/* ═══════════════════ 最小 sector 上反复 GC 的边界健壮性 ═══════════ */

static void test_smallest_sector_gc(void)
{
    mkv_t kv;
    mock_fal_reset(256, 256 * 4); /* blk=256 (= MKV_MIN_SECTOR_SIZE), 4 sector */
    TEST_ASSERT_EQUAL_INT(0, mkv_init(&kv, MOCK_PART_NAME));

    uint32_t seq0 = kv.active_seq;
    uint32_t id = 1;
    int written = 0;
    /* 反复写触发多次 GC (跨 sector 轮转)，探测擦除区与边界 */
    while (written < 200 && kv.active_seq < seq0 + 5)
    {
        uint8_t v[4] = { (uint8_t)id, (uint8_t)(id >> 8),
                         (uint8_t)(id >> 16), (uint8_t)(id >> 24) };
        if (mkv_set(&kv, id, v, 4) == 0) written++;
        id++;
    }
    TEST_ASSERT_TRUE_MESSAGE(kv.active_seq > seq0, "最小 sector 上 GC 未触发");

    /* 已写入的 id 均应可读 (不越界、不误判擦除区) */
    for (uint32_t i = 1; i <= (uint32_t)written; i++)
    {
        uint8_t out[4];
        TEST_ASSERT_EQUAL_INT(4, mkv_get(&kv, i, out, 4));
        TEST_ASSERT_EQUAL_UINT32(i, ((uint32_t)out[0] | ((uint32_t)out[1] << 8) |
                                    ((uint32_t)out[2] << 16) | ((uint32_t)out[3] << 24)));
    }
}

/* ═════════════════════════════ Runner ═════════════════════════════ */

int mkv_run_all_tests(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_roundtrip_basic);
    RUN_TEST(test_gc_preserves_large_value);
    RUN_TEST(test_gc_preserves_many_ids);
    RUN_TEST(test_smallest_sector_gc);
    return UNITY_END();
}

TEST_SUITE_DEFINE(mkv, mkv_setup, mkv_teardown, mkv_run_all_tests);
