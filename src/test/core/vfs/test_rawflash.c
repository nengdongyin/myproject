/**
 * @file test_rawflash.c
 * @brief rawflash 通用块区存储核心单元测试（掉电安全 / 写游标 / RDWR / append）
 *
 * 介质为 flash 语义的内存 mock（prog 按位与），因此这些用例真实检验了核心的
 * 「先擦后写」「close 才提交 footer」「0xFF 脏尾检测」等掉电安全纪律。
 */

#include "unity.h"
#include "test_suite.h"
#include "rawflash.h"
#include "mock_storage.h"
#include <string.h>
#include <errno.h>

#define MOCK_BLK_SZ   4096u
#define MOCK_BLK_CNT  4u                    /* 3 数据块 + 1 元数据块 */
#define DATA_CAP      (3u * MOCK_BLK_SZ)    /* = 12288 */

static vfs_mock_flash_t g_flash;
static rawflash_ctx_t   g_ctx;

/** @brief 数据区 [0,cap) 内、距分区起点 off 处的字节直接篡改（绕过编程语义） */
static void poke(uint32_t off, uint8_t val)
{
    g_flash.mem[off] = val;
}

/** @brief 新会话：重新 init 上下文并打开 */
static int reopen(int mode, int append, int truncate)
{
    rawflash_ctx_init(&g_ctx, &g_flash.bdev, DATA_CAP);
    return rawflash_open(&g_ctx, mode, append, truncate);
}

/** @brief 便利：写一段数据并 close（一次完整提交） */
static int write_all_and_close(const void *buf, size_t n)
{
    if (reopen(RAWFLASH_MODE_WRITE, 0, 0) != 0)
        return -1;
    if (rawflash_write(&g_ctx, buf, n) != (ssize_t)n)
        return -1;
    return rawflash_close(&g_ctx);
}

static void rf_setup(void)
{
    TEST_ASSERT_EQUAL_INT(0, vfs_mock_flash_init(&g_flash, MOCK_BLK_SZ, MOCK_BLK_CNT));
    vfs_mock_make_bdev(&g_flash, "raw", 0, MOCK_BLK_CNT);
}

static void rf_teardown(void)
{
    vfs_mock_flash_free(&g_flash);
}

/* ================================================================
 *  Case: 基本写→close→读回
 * ================================================================ */

static void test_write_close_then_read(void)
{
    TEST_ASSERT_EQUAL_INT(0, write_all_and_close("hello", 5));
    TEST_ASSERT_EQUAL_UINT32(5, rawflash_visible_len(&g_flash.bdev, DATA_CAP));

    TEST_ASSERT_EQUAL_INT(0, reopen(RAWFLASH_MODE_READ, 0, 0));
    uint8_t buf[16] = {0};
    TEST_ASSERT_EQUAL_INT(5, (int)rawflash_read(&g_ctx, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_MEMORY("hello", buf, 5);
    /* 读到末尾后再读返回 0 */
    TEST_ASSERT_EQUAL_INT(0, (int)rawflash_read(&g_ctx, buf, sizeof(buf)));
}

/* ================================================================
 *  Case: 未 close（掉电）→ footer 无效 → 视为空文件
 * ================================================================ */

static void test_uncommitted_write_reads_as_empty(void)
{
    TEST_ASSERT_EQUAL_INT(0, reopen(RAWFLASH_MODE_WRITE, 0, 0));
    TEST_ASSERT_EQUAL_INT(7, (int)rawflash_write(&g_ctx, "partial", 7));
    /* 故意不 close，模拟掉电：数据已在介质，但 footer 未提交 */

    TEST_ASSERT_EQUAL_UINT32(0, rawflash_visible_len(&g_flash.bdev, DATA_CAP));
    TEST_ASSERT_EQUAL_INT(0, reopen(RAWFLASH_MODE_READ, 0, 0));
    uint8_t buf[8];
    TEST_ASSERT_EQUAL_INT(0, (int)rawflash_read(&g_ctx, buf, sizeof(buf)));
}

/* ================================================================
 *  Case: 截断到 0
 * ================================================================ */

static void test_truncate_to_zero(void)
{
    TEST_ASSERT_EQUAL_INT(0, write_all_and_close("abcdef", 6));
    TEST_ASSERT_EQUAL_UINT32(6, rawflash_visible_len(&g_flash.bdev, DATA_CAP));

    TEST_ASSERT_EQUAL_INT(0, reopen(RAWFLASH_MODE_READ, 0, 0));
    TEST_ASSERT_EQUAL_INT(0, rawflash_truncate(&g_ctx));
    TEST_ASSERT_EQUAL_INT(0, rawflash_close(&g_ctx)); /* WRITE 模式落 footer=0 */
    TEST_ASSERT_EQUAL_UINT32(0, rawflash_visible_len(&g_flash.bdev, DATA_CAP));
}

/* ================================================================
 *  Case: 容量上界 → -ENOSPC
 * ================================================================ */

static void test_capacity_enospc(void)
{
    static uint8_t big[DATA_CAP + 1];
    memset(big, 0x5A, sizeof(big));

    TEST_ASSERT_EQUAL_INT(0, reopen(RAWFLASH_MODE_WRITE, 0, 0));
    TEST_ASSERT_EQUAL_INT((int)DATA_CAP, (int)rawflash_write(&g_ctx, big, DATA_CAP));
    /* 已满，再写 1 字节越界 */
    TEST_ASSERT_EQUAL_INT(-ENOSPC, (int)rawflash_write(&g_ctx, big, 1));
    TEST_ASSERT_EQUAL_INT(0, rawflash_close(&g_ctx));
}

/* ================================================================
 *  Case: lseek + 定位读
 * ================================================================ */

static void test_lseek_and_read(void)
{
    TEST_ASSERT_EQUAL_INT(0, write_all_and_close("ABCDEFGH", 8));
    TEST_ASSERT_EQUAL_INT(0, reopen(RAWFLASH_MODE_READ, 0, 0));

    TEST_ASSERT_EQUAL_INT(3, (int)rawflash_lseek(&g_ctx, 3, RAWFLASH_SEEK_SET));
    uint8_t buf[8] = {0};
    TEST_ASSERT_EQUAL_INT(4, (int)rawflash_read(&g_ctx, buf, 4));
    TEST_ASSERT_EQUAL_MEMORY("DEFG", buf, 4);

    TEST_ASSERT_EQUAL_INT(6, (int)rawflash_lseek(&g_ctx, -2, RAWFLASH_SEEK_END));
    TEST_ASSERT_EQUAL_INT(2, (int)rawflash_read(&g_ctx, buf, 2));
    TEST_ASSERT_EQUAL_MEMORY("GH", buf, 2);

    /* 越界 seek → -EINVAL */
    TEST_ASSERT_EQUAL_INT(-EINVAL, (int)rawflash_lseek(&g_ctx, 100, RAWFLASH_SEEK_SET));
}

/* ================================================================
 *  Case: O_RDWR 读-改-写
 * ================================================================ */

static void test_rdwr_read_then_write(void)
{
    TEST_ASSERT_EQUAL_INT(0, write_all_and_close("AAAA", 4));

    /* 打开不擦，可读旧内容 */
    TEST_ASSERT_EQUAL_INT(0, reopen(RAWFLASH_MODE_RDWR, 0, 0));
    uint8_t buf[8] = {0};
    TEST_ASSERT_EQUAL_INT(4, (int)rawflash_read(&g_ctx, buf, 4));
    TEST_ASSERT_EQUAL_MEMORY("AAAA", buf, 4);

    /* 首次 write 才整区擦除并重写 */
    TEST_ASSERT_EQUAL_INT(2, (int)rawflash_write(&g_ctx, "BB", 2));
    TEST_ASSERT_EQUAL_INT(0, rawflash_close(&g_ctx));
    TEST_ASSERT_EQUAL_UINT32(2, rawflash_visible_len(&g_flash.bdev, DATA_CAP));
}

static void test_rdwr_without_write_keeps_content(void)
{
    TEST_ASSERT_EQUAL_INT(0, write_all_and_close("KEEP", 4));

    /* 打开 RDWR 后不写就 close：不应改动分区 */
    TEST_ASSERT_EQUAL_INT(0, reopen(RAWFLASH_MODE_RDWR, 0, 0));
    TEST_ASSERT_EQUAL_INT(0, rawflash_close(&g_ctx));
    TEST_ASSERT_EQUAL_UINT32(4, rawflash_visible_len(&g_flash.bdev, DATA_CAP));
}

/* ================================================================
 *  Case: append 尾部续写 + 脏尾检测
 * ================================================================ */

static void test_append_extends(void)
{
    TEST_ASSERT_EQUAL_INT(0, write_all_and_close("AB", 2));

    TEST_ASSERT_EQUAL_INT(0, reopen(RAWFLASH_MODE_WRITE, 1 /*append*/, 0));
    TEST_ASSERT_EQUAL_INT(2, (int)rawflash_write(&g_ctx, "CD", 2));
    TEST_ASSERT_EQUAL_INT(0, rawflash_close(&g_ctx));

    TEST_ASSERT_EQUAL_UINT32(4, rawflash_visible_len(&g_flash.bdev, DATA_CAP));
    TEST_ASSERT_EQUAL_INT(0, reopen(RAWFLASH_MODE_READ, 0, 0));
    uint8_t buf[8] = {0};
    TEST_ASSERT_EQUAL_INT(4, (int)rawflash_read(&g_ctx, buf, 4));
    TEST_ASSERT_EQUAL_MEMORY("ABCD", buf, 4);
}

static void test_append_rejects_dirty_tail(void)
{
    TEST_ASSERT_EQUAL_INT(0, write_all_and_close("AB", 2));

    /* 在已提交长度之后直接污染当前块尾部（模拟上次掉电残留） */
    poke(2, 0x00);

    /* append 打开应检测到非 0xFF 脏尾并返回 -EIO */
    TEST_ASSERT_EQUAL_INT(-EIO, reopen(RAWFLASH_MODE_WRITE, 1, 0));
}

/* ================================================================
 *  Case: footer 双份冗余
 * ================================================================ */

static void test_footer_redundant_survives_one_copy(void)
{
    TEST_ASSERT_EQUAL_INT(0, write_all_and_close("hello", 5));

    uint32_t meta_byte = (MOCK_BLK_CNT - 1) * MOCK_BLK_SZ;
    /* 破坏主份（块首）首字节 */
    poke(meta_byte, (uint8_t)(g_flash.mem[meta_byte] ^ 0xFF));
    /* 冗余份（块中途）仍在 → 长度仍可读回 */
    TEST_ASSERT_EQUAL_UINT32(5, rawflash_visible_len(&g_flash.bdev, DATA_CAP));
}

static void test_footer_both_copies_bad_reads_empty(void)
{
    TEST_ASSERT_EQUAL_INT(0, write_all_and_close("hello", 5));

    uint32_t meta_byte = (MOCK_BLK_CNT - 1) * MOCK_BLK_SZ;
    uint32_t red = MOCK_BLK_SZ / 2;
    /* 破坏两份 footer */
    poke(meta_byte, (uint8_t)(g_flash.mem[meta_byte] ^ 0xFF));
    poke(meta_byte + red, (uint8_t)(g_flash.mem[meta_byte + red] ^ 0xFF));
    TEST_ASSERT_EQUAL_UINT32(0, rawflash_visible_len(&g_flash.bdev, DATA_CAP));
}

/* ================================================================
 *  Case: 可见长度夹取到 cap
 * ================================================================ */

static void test_visible_len_clamped(void)
{
    TEST_ASSERT_EQUAL_INT(0, write_all_and_close("0123456789", 10));
    TEST_ASSERT_EQUAL_UINT32(10, rawflash_visible_len(&g_flash.bdev, DATA_CAP));
    /* cap 小于实际长度时夹取到 cap */
    TEST_ASSERT_EQUAL_UINT32(4, rawflash_visible_len(&g_flash.bdev, 4));
}

/* ================================================================
 *  Runner
 * ================================================================ */

int rawflash_run_all_tests(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_write_close_then_read);
    RUN_TEST(test_uncommitted_write_reads_as_empty);
    RUN_TEST(test_truncate_to_zero);
    RUN_TEST(test_capacity_enospc);
    RUN_TEST(test_lseek_and_read);
    RUN_TEST(test_rdwr_read_then_write);
    RUN_TEST(test_rdwr_without_write_keeps_content);
    RUN_TEST(test_append_extends);
    RUN_TEST(test_append_rejects_dirty_tail);
    RUN_TEST(test_footer_redundant_survives_one_copy);
    RUN_TEST(test_footer_both_copies_bad_reads_empty);
    RUN_TEST(test_visible_len_clamped);

    return UNITY_END();
}

TEST_SUITE_DEFINE(rawflash, rf_setup, rf_teardown, rawflash_run_all_tests);
