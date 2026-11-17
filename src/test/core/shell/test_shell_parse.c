/**
 * @file test_shell_parse.c
 * @brief shell_parse.h（shell 整数参数解析）单元测试
 *
 * 覆盖 base=0 语义: 十进制 / 0x 十六进制 / 0 八进制、整串消费、溢出拒收、
 * 符号处理与 int 边界。两个后端分支 (SHELL_PARSE_USE_LIBC 0/1) 语义一致,
 * 同一套用例两分支都应通过。
 */

#include "unity.h"
#include "test_suite.h"
#include "shell_parse.h"

/* ================================================================
 *  u32 —— 正常解析
 * ================================================================ */

static void test_u32_decimal(void)
{
    uint32_t v = 0;
    TEST_ASSERT_TRUE(shell_arg_u32("0", &v));
    TEST_ASSERT_EQUAL_UINT32(0u, v);
    TEST_ASSERT_TRUE(shell_arg_u32("10", &v));
    TEST_ASSERT_EQUAL_UINT32(10u, v);
    TEST_ASSERT_TRUE(shell_arg_u32("4294967295", &v));
    TEST_ASSERT_EQUAL_UINT32(4294967295u, v);
}

static void test_u32_prefix(void)
{
    uint32_t v = 0;
    TEST_ASSERT_TRUE(shell_arg_u32("010", &v)); /* 前导 0 → 八进制 */
    TEST_ASSERT_EQUAL_UINT32(8u, v);
    TEST_ASSERT_TRUE(shell_arg_u32("0x1f", &v));
    TEST_ASSERT_EQUAL_UINT32(31u, v);
    TEST_ASSERT_TRUE(shell_arg_u32("0X1F", &v));
    TEST_ASSERT_EQUAL_UINT32(31u, v);
    TEST_ASSERT_TRUE(shell_arg_u32("0xFF", &v));
    TEST_ASSERT_EQUAL_UINT32(255u, v);
    TEST_ASSERT_TRUE(shell_arg_u32("0xFFFFFFFF", &v));
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFu, v);
}

static void test_u32_reject(void)
{
    uint32_t v = 0;
    TEST_ASSERT_FALSE(shell_arg_u32("4294967296", &v));  /* 溢出 */
    TEST_ASSERT_FALSE(shell_arg_u32("0x1FFFFFFFF", &v)); /* 33 bit 溢出 */
    TEST_ASSERT_FALSE(shell_arg_u32("-1", &v));          /* 符号拒收 */
    TEST_ASSERT_FALSE(shell_arg_u32("12abc", &v));       /* 整串未消费 */
    TEST_ASSERT_FALSE(shell_arg_u32("", &v));
    TEST_ASSERT_FALSE(shell_arg_u32("0x", &v));
    TEST_ASSERT_FALSE(shell_arg_u32("089", &v));         /* 八进制无 '9' */
    TEST_ASSERT_FALSE(shell_arg_u32("abc", &v));
}

/* ================================================================
 *  int —— 有符号
 * ================================================================ */

static void test_int_ok(void)
{
    int v = 0;
    TEST_ASSERT_TRUE(shell_arg_int("123", &v));
    TEST_ASSERT_EQUAL_INT(123, v);
    TEST_ASSERT_TRUE(shell_arg_int("-123", &v));
    TEST_ASSERT_EQUAL_INT(-123, v);
    TEST_ASSERT_TRUE(shell_arg_int("0x10", &v));
    TEST_ASSERT_EQUAL_INT(16, v);
    TEST_ASSERT_TRUE(shell_arg_int("+5", &v));
    TEST_ASSERT_EQUAL_INT(5, v);
    TEST_ASSERT_TRUE(shell_arg_int("2147483647", &v));
    TEST_ASSERT_EQUAL_INT(2147483647, v);
    TEST_ASSERT_TRUE(shell_arg_int("-2147483648", &v));
    TEST_ASSERT_EQUAL_INT(-2147483648, v);
}

static void test_int_reject(void)
{
    int v = 0;
    TEST_ASSERT_FALSE(shell_arg_int("2147483648", &v));   /* 超 INT_MAX */
    TEST_ASSERT_FALSE(shell_arg_int("-2147483649", &v));  /* 低于 INT_MIN */
    TEST_ASSERT_FALSE(shell_arg_int("12abc", &v));
    TEST_ASSERT_FALSE(shell_arg_int("", &v));
}

static void test_float_ok(void)
{
    float v = 0;
    TEST_ASSERT_TRUE(shell_arg_float("0", &v));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, v);
    TEST_ASSERT_TRUE(shell_arg_float("1.5", &v));
    TEST_ASSERT_EQUAL_FLOAT(1.5f, v);
    TEST_ASSERT_TRUE(shell_arg_float("-2.25", &v));
    TEST_ASSERT_EQUAL_FLOAT(-2.25f, v);
    TEST_ASSERT_TRUE(shell_arg_float("0.5", &v));
    TEST_ASSERT_EQUAL_FLOAT(0.5f, v);
    TEST_ASSERT_TRUE(shell_arg_float("-0.125", &v));
    TEST_ASSERT_EQUAL_FLOAT(-0.125f, v);
    TEST_ASSERT_TRUE(shell_arg_float("100", &v));
    TEST_ASSERT_EQUAL_FLOAT(100.0f, v);
    TEST_ASSERT_TRUE(shell_arg_float("1e2", &v));
    TEST_ASSERT_EQUAL_FLOAT(100.0f, v);
    TEST_ASSERT_TRUE(shell_arg_float("2.5e1", &v));
    TEST_ASSERT_EQUAL_FLOAT(25.0f, v);
}

static void test_float_reject(void)
{
    float v = 0;
    TEST_ASSERT_FALSE(shell_arg_float("", &v));
    TEST_ASSERT_FALSE(shell_arg_float("abc", &v));
    TEST_ASSERT_FALSE(shell_arg_float("1.5x", &v));  /* 尾随垃圾 */
    TEST_ASSERT_FALSE(shell_arg_float("1e39", &v));  /* 溢出 float */
    TEST_ASSERT_FALSE(shell_arg_float("1e400", &v)); /* 溢出 double */
    TEST_ASSERT_FALSE(shell_arg_float(".", &v));
    TEST_ASSERT_FALSE(shell_arg_float("-", &v));
}

static void test_null_out(void)
{
    TEST_ASSERT_FALSE(shell_arg_u32("1", NULL));
    TEST_ASSERT_FALSE(shell_arg_int("1", NULL));
    TEST_ASSERT_FALSE(shell_arg_float("1", NULL));
}

/* ================================================================
 *  Runner
 * ================================================================ */

int shell_parse_run_all_tests(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_u32_decimal);
    RUN_TEST(test_u32_prefix);
    RUN_TEST(test_u32_reject);

    RUN_TEST(test_int_ok);
    RUN_TEST(test_int_reject);
    RUN_TEST(test_float_ok);
    RUN_TEST(test_float_reject);
    RUN_TEST(test_null_out);

    return UNITY_END();
}

TEST_SUITE_DEFINE(shell_parse, NULL, NULL, shell_parse_run_all_tests);
