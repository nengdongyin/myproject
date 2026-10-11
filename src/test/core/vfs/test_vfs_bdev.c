/**
 * @file test_vfs_bdev.c
 * @brief vfs_bdev（分区切片 + 块/字节访问）单元测试
 */

#include "unity.h"
#include "test_suite.h"
#include "vfs_bdev.h"
#include "mock_storage.h"
#include <string.h>
#include <errno.h>

static vfs_mock_flash_t g_flash;

/* ================================================================
 *  setUp / tearDown
 * ================================================================ */

static void bdev_setup(void)
{
    TEST_ASSERT_EQUAL_INT(0, vfs_mock_flash_init(&g_flash, 4096, 8)); /* 32KB */
    vfs_mock_make_bdev(&g_flash, "part0", 0, 8);
}

static void bdev_teardown(void)
{
    vfs_mock_flash_free(&g_flash);
}

/* ================================================================
 *  Case: 基本字段 / 大小
 * ================================================================ */

static void test_bdev_init_fields(void)
{
    TEST_ASSERT_EQUAL_STRING("part0", g_flash.bdev.name);
    TEST_ASSERT_EQUAL_UINT32(0, g_flash.bdev.offset);
    TEST_ASSERT_EQUAL_UINT32(4096, g_flash.bdev.blk_size);
    TEST_ASSERT_EQUAL_UINT32(8, g_flash.bdev.blk_count);
}

static void test_bdev_size(void)
{
    TEST_ASSERT_EQUAL_UINT32(8 * 4096, vfs_bdev_size(&g_flash.bdev));
}

static void test_bdev_size_null(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, vfs_bdev_size(NULL));
}

/* ================================================================
 *  Case: 块级读 / 写 / 擦
 * ================================================================ */

static void test_block_prog_then_read(void)
{
    uint8_t w[4096];
    uint8_t r[4096];
    memset(w, 0x5A, sizeof(w));

    TEST_ASSERT_EQUAL_INT(0, vfs_bdev_prog(&g_flash.bdev, 1, w, 1));
    memset(r, 0, sizeof(r));
    TEST_ASSERT_EQUAL_INT(0, vfs_bdev_read(&g_flash.bdev, 1, r, 1));
    TEST_ASSERT_EQUAL_MEMORY(w, r, sizeof(w));
}

static void test_block_erase_resets_to_ff(void)
{
    uint8_t w[4096];
    uint8_t r[4096];
    memset(w, 0x00, sizeof(w));

    TEST_ASSERT_EQUAL_INT(0, vfs_bdev_prog(&g_flash.bdev, 0, w, 1));
    TEST_ASSERT_EQUAL_INT(0, vfs_bdev_erase(&g_flash.bdev, 0, 1));
    memset(r, 0xEE, sizeof(r));
    TEST_ASSERT_EQUAL_INT(0, vfs_bdev_read(&g_flash.bdev, 0, r, 1));
    for (size_t i = 0; i < sizeof(r); i++)
        TEST_ASSERT_EQUAL_HEX8(0xFF, r[i]);
}

/* prog 只能清位（按位与）：二次写入不清内存，结果 = 两次之与 */
static void test_block_prog_is_bitwise_and(void)
{
    uint8_t a[4096];
    uint8_t b[4096];
    uint8_t r[4096];
    memset(a, 0x0F, sizeof(a));
    memset(b, 0xF0, sizeof(b));

    TEST_ASSERT_EQUAL_INT(0, vfs_bdev_prog(&g_flash.bdev, 2, a, 1));
    TEST_ASSERT_EQUAL_INT(0, vfs_bdev_prog(&g_flash.bdev, 2, b, 1));
    TEST_ASSERT_EQUAL_INT(0, vfs_bdev_read(&g_flash.bdev, 2, r, 1));
    /* 0x0F & 0xF0 == 0x00，说明未擦除即写会被静默位与 */
    TEST_ASSERT_EQUAL_HEX8(0x00, r[0]);
}

static void test_block_bounds_reject(void)
{
    uint8_t buf[4096];
    /* blk == blk_count 越界 */
    TEST_ASSERT_EQUAL_INT(-EINVAL, vfs_bdev_read(&g_flash.bdev, 8, buf, 1));
    /* nblk 使范围越过末尾 */
    TEST_ASSERT_EQUAL_INT(-EINVAL, vfs_bdev_prog(&g_flash.bdev, 7, buf, 2));
    TEST_ASSERT_EQUAL_INT(-EINVAL, vfs_bdev_erase(&g_flash.bdev, 6, 3));
}

/* ================================================================
 *  Case: 字节级读写（不要求块对齐）
 * ================================================================ */

static void test_byte_round_trip(void)
{
    const uint8_t w[5] = {0x11, 0x22, 0x33, 0x44, 0x55};
    uint8_t r[5] = {0};

    TEST_ASSERT_EQUAL_INT(0, vfs_bdev_prog_bytes(&g_flash.bdev, 100, w, 5));
    TEST_ASSERT_EQUAL_INT(0, vfs_bdev_read_bytes(&g_flash.bdev, 100, r, 5));
    TEST_ASSERT_EQUAL_MEMORY(w, r, 5);
}

static void test_byte_bounds_reject(void)
{
    uint8_t buf[8];
    uint32_t total = vfs_bdev_size(&g_flash.bdev);
    /* off == total 处再读 1 字节越界 */
    TEST_ASSERT_EQUAL_INT(-EINVAL, vfs_bdev_read_bytes(&g_flash.bdev, total, buf, 1));
    /* off + n 超过 total */
    TEST_ASSERT_EQUAL_INT(-EINVAL, vfs_bdev_prog_bytes(&g_flash.bdev, total - 1, buf, 2));
}

/* ================================================================
 *  Case: NULL / 非法参数
 * ================================================================ */

static void test_null_args_reject(void)
{
    uint8_t buf[8];
    TEST_ASSERT_EQUAL_INT(-EINVAL, vfs_bdev_read(NULL, 0, buf, 1));
    TEST_ASSERT_EQUAL_INT(-EINVAL, vfs_bdev_read(&g_flash.bdev, 0, NULL, 1));
    TEST_ASSERT_EQUAL_INT(-EINVAL, vfs_bdev_prog(NULL, 0, buf, 1));
    TEST_ASSERT_EQUAL_INT(-EINVAL, vfs_bdev_read_bytes(&g_flash.bdev, 0, NULL, 1));
}

static void test_bdev_init_null_silent(void)
{
    vfs_bdev_init(NULL, "x", &g_flash.storage, 0, 4096, 1); /* 不应崩溃 */
}

/* ================================================================
 *  Case: 带 offset 的分区切片
 * ================================================================ */

static void test_bdev_with_offset(void)
{
    const uint8_t w[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    uint8_t r[4] = {0};

    /* 切一段：介质内偏移 16KB，4 个块 */
    vfs_mock_make_bdev(&g_flash, "part_off", 16 * 1024, 4);
    TEST_ASSERT_EQUAL_UINT32(4, g_flash.bdev.blk_count);

    TEST_ASSERT_EQUAL_INT(0, vfs_bdev_prog_bytes(&g_flash.bdev, 0, w, 4));
    /* 数据应落在介质绝对偏移 16KB 处 */
    TEST_ASSERT_EQUAL_MEMORY(w, g_flash.mem + 16 * 1024, 4);
    TEST_ASSERT_EQUAL_INT(0, vfs_bdev_read_bytes(&g_flash.bdev, 0, r, 4));
    TEST_ASSERT_EQUAL_MEMORY(w, r, 4);
}

/* ================================================================
 *  Runner
 * ================================================================ */

int vfs_bdev_run_all_tests(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_bdev_init_fields);
    RUN_TEST(test_bdev_size);
    RUN_TEST(test_bdev_size_null);

    RUN_TEST(test_block_prog_then_read);
    RUN_TEST(test_block_erase_resets_to_ff);
    RUN_TEST(test_block_prog_is_bitwise_and);
    RUN_TEST(test_block_bounds_reject);

    RUN_TEST(test_byte_round_trip);
    RUN_TEST(test_byte_bounds_reject);

    RUN_TEST(test_null_args_reject);
    RUN_TEST(test_bdev_init_null_silent);

    RUN_TEST(test_bdev_with_offset);

    return UNITY_END();
}

TEST_SUITE_DEFINE(vfs_bdev, bdev_setup, bdev_teardown, vfs_bdev_run_all_tests);
