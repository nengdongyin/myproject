/**
 * @file test_ft.c
 * @brief file_transfer 组件 —— 装配契约单测 (PC / PORT_OS_LIBC)
 *
 * @par 覆盖范围
 *  - attach 参数校验与单例约束 (重复装配被拒)
 *  - 装配成功/失败回滚/detach 幂等/重新装配
 *  - 未装配时 start_* 的安全性
 *  - 状态派生值 (IDLE / 非接收态)
 *
 * @par 刻意不覆盖
 *  - 路径校验与会话流转: 需要挂载一个可写的 VFS 卷, 属于集成测试范畴。
 *  - "attach 后 parser 排在链首": protocol_chain 无公开的槽位查询 API,
 *    该顺序由 protocol_chain_init.c 的调用位置保证 (先 attach 再挂 imperx/camyu)。
 */

#include "unity.h"
#include "test_suite.h"
#include "file_transfer.h"
#include "protocol_chain.h"

#define FT_TEST_CHAIN_SLOTS 4

/* 不做任何事的字节出口, 只统计字节数 */
static uint32_t g_tx_bytes;

static void stub_tx(const uint8_t *data, uint32_t len)
{
    (void)data;
    g_tx_bytes += len;
}

static protocol_chain *g_chain;

/* ================================================================ */
/*  setUp / tearDown                                                 */
/* ================================================================ */

static void ft_setup(void)
{
    g_tx_bytes = 0;
    g_chain = NULL;
}

static void ft_teardown(void)
{
    /* 组件是单例: 无论用例如何退出都拆掉, 避免污染后续 suite */
    file_transfer_detach();
    if (g_chain)
    {
        protocol_chain_destroy(g_chain);
        g_chain = NULL;
    }
}

/* ================================================================ */
/*  attach 参数校验                                                   */
/* ================================================================ */

static void test_attach_null_cfg_returns_unsupported(void)
{
    TEST_ASSERT_EQUAL(PARAM_ERR_NOT_SUPPORTED, file_transfer_attach(NULL));
}

static void test_attach_null_chain_returns_unsupported(void)
{
    file_transfer_cfg_t cfg = { .chain = NULL, .tx_write = stub_tx };

    TEST_ASSERT_EQUAL(PARAM_ERR_NOT_SUPPORTED, file_transfer_attach(&cfg));
}

static void test_attach_null_tx_returns_unsupported(void)
{
    file_transfer_cfg_t cfg;

    g_chain = protocol_chain_create(FT_TEST_CHAIN_SLOTS);
    TEST_ASSERT_NOT_NULL(g_chain);

    cfg.chain = g_chain;
    cfg.tx_write = NULL;
    TEST_ASSERT_EQUAL(PARAM_ERR_NOT_SUPPORTED, file_transfer_attach(&cfg));
}

/* ================================================================ */
/*  未装配时的安全性                                                  */
/* ================================================================ */

static void test_state_is_idle_before_attach(void)
{
    file_transfer_detach();

    TEST_ASSERT_EQUAL(FT_STATE_IDLE, file_transfer_get_state());
    TEST_ASSERT_FALSE(file_transfer_wants_bulk_rx());
}

static void test_start_before_attach_returns_unsupported(void)
{
    file_transfer_detach();

    TEST_ASSERT_EQUAL(PARAM_ERR_NOT_SUPPORTED, file_transfer_start_recv());
    TEST_ASSERT_EQUAL(PARAM_ERR_NOT_SUPPORTED, file_transfer_start_send());
    TEST_ASSERT_EQUAL(FT_STATE_IDLE, file_transfer_get_state());
}

static void test_detach_without_attach_is_idempotent(void)
{
    file_transfer_detach();
    file_transfer_detach();

    TEST_ASSERT_EQUAL(FT_STATE_IDLE, file_transfer_get_state());
}

/* ================================================================ */
/*  装配成功 / 单例约束                                               */
/* ================================================================ */

static void test_attach_success(void)
{
    file_transfer_cfg_t cfg;

    g_chain = protocol_chain_create(FT_TEST_CHAIN_SLOTS);
    TEST_ASSERT_NOT_NULL(g_chain);

    cfg.chain = g_chain;
    cfg.tx_write = stub_tx;

    TEST_ASSERT_EQUAL(PARAM_OK, file_transfer_attach(&cfg));

    /* 刚装配好、尚未启动任何会话: 链上没有本组件的锁定 */
    TEST_ASSERT_EQUAL(FT_STATE_IDLE, file_transfer_get_state());
    TEST_ASSERT_FALSE(file_transfer_wants_bulk_rx());
}

static void test_attach_twice_rejected(void)
{
    file_transfer_cfg_t cfg;

    g_chain = protocol_chain_create(FT_TEST_CHAIN_SLOTS);
    TEST_ASSERT_NOT_NULL(g_chain);

    cfg.chain = g_chain;
    cfg.tx_write = stub_tx;

    TEST_ASSERT_EQUAL(PARAM_OK, file_transfer_attach(&cfg));
    TEST_ASSERT_EQUAL(PARAM_ERR_NOT_SUPPORTED, file_transfer_attach(&cfg));
}

static void test_attach_after_detach_succeeds(void)
{
    file_transfer_cfg_t cfg;

    g_chain = protocol_chain_create(FT_TEST_CHAIN_SLOTS);
    TEST_ASSERT_NOT_NULL(g_chain);

    cfg.chain = g_chain;
    cfg.tx_write = stub_tx;

    TEST_ASSERT_EQUAL(PARAM_OK, file_transfer_attach(&cfg));
    file_transfer_detach();
    TEST_ASSERT_EQUAL(PARAM_OK, file_transfer_attach(&cfg));
}

/* ================================================================ */
/*  链容量不足时的回滚                                                */
/* ================================================================ */

static void test_attach_chain_too_small_rolls_back(void)
{
    /* 组件需要 2 个槽位 (收 + 发), 只给 1 个必须失败且完整回滚 */
    protocol_chain *small = protocol_chain_create(1);
    file_transfer_cfg_t bad;
    file_transfer_cfg_t good;

    TEST_ASSERT_NOT_NULL(small);

    bad.chain = small;
    bad.tx_write = stub_tx;
    TEST_ASSERT_EQUAL(PARAM_ERR_NO_MEMORY, file_transfer_attach(&bad));

    /* 回滚后不应残留装配状态: 换一条正常的链应当能装上 */
    g_chain = protocol_chain_create(FT_TEST_CHAIN_SLOTS);
    TEST_ASSERT_NOT_NULL(g_chain);

    good.chain = g_chain;
    good.tx_write = stub_tx;
    TEST_ASSERT_EQUAL(PARAM_OK, file_transfer_attach(&good));

    protocol_chain_destroy(small);
}

/* ================================================================ */
/*  Runner                                                           */
/* ================================================================ */

int ft_run_all_tests(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_attach_null_cfg_returns_unsupported);
    RUN_TEST(test_attach_null_chain_returns_unsupported);
    RUN_TEST(test_attach_null_tx_returns_unsupported);

    RUN_TEST(test_state_is_idle_before_attach);
    RUN_TEST(test_start_before_attach_returns_unsupported);
    RUN_TEST(test_detach_without_attach_is_idempotent);

    RUN_TEST(test_attach_success);
    RUN_TEST(test_attach_twice_rejected);
    RUN_TEST(test_attach_after_detach_succeeds);

    RUN_TEST(test_attach_chain_too_small_rolls_back);

    return UNITY_END();
}

#include "test_suite.h"
TEST_SUITE_DEFINE(file_transfer, ft_setup, ft_teardown, ft_run_all_tests);
