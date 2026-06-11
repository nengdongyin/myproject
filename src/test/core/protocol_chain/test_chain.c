#include "unity.h"
#include "test_suite.h"
#include "protocol_chain.h"
#include "protocol_parser.h"
#include <string.h>

extern uint32_t mock_time_ms;

/* ================================================================ */
/*  Mock parsers                                                     */
/* ================================================================ */

static int mock_parse_ok(protocol_parser_t *p, const uint8_t *d, uint32_t len)
{
    (void)p; (void)d; (void)len;
    return PARSER_ERR_NONE;
}

static int mock_parse_incomplete(protocol_parser_t *p, const uint8_t *d, uint32_t len)
{
    (void)p; (void)d; (void)len;
    return PARSER_ERR_INCOMPLETE;
}

static int mock_parse_fatal(protocol_parser_t *p, const uint8_t *d, uint32_t len)
{
    (void)p; (void)d; (void)len;
    return PARSER_ERR_FRAME;
}

static void mock_reset(protocol_parser_t *p)
{
    p->rx.data_len = 0;
}

static bool mock_poll_timeout(protocol_parser_t *p)
{
    (void)p;
    return false;
}

static const struct protocol_parser_ops mock_ops_none = {
    .parse_data = mock_parse_ok, .reset = mock_reset,
};

static const struct protocol_parser_ops mock_ops_incomplete = {
    .parse_data = mock_parse_incomplete, .reset = mock_reset,
};

static const struct protocol_parser_ops mock_ops_fatal = {
    .parse_data = mock_parse_fatal, .reset = mock_reset,
};

static const struct protocol_parser_ops mock_ops_timeout = {
    .parse_data = mock_parse_ok, .reset = mock_reset, .poll = mock_poll_timeout,
};

static const ErrorMapEntry mock_err_map[] = {
    { PARSER_ERR_NONE,       PARSER_ERR_NONE },
    { PARSER_ERR_INCOMPLETE, PARSER_ERR_INCOMPLETE },
    { PARSER_ERR_FRAME,      PARSER_ERR_FRAME },
    { PARSER_ERR_TIMEOUT,    PARSER_ERR_TIMEOUT },
};
static const ErrorMapper mock_err_mapper = {
    .table = mock_err_map, .count = 4,
};

static protocol_parser_t mock_p_ok;
static protocol_parser_t mock_p_inc;
static protocol_parser_t mock_p_fatal;
static protocol_parser_t mock_p_timeout;

static protocol_parser_t *mock_p_ok_ptr      = &mock_p_ok;
static protocol_parser_t *mock_p_inc_ptr     = &mock_p_inc;
static protocol_parser_t *mock_p_fatal_ptr   = &mock_p_fatal;
static protocol_parser_t *mock_p_timeout_ptr = &mock_p_timeout;

static void mock_parser_setup(void)
{
    memset(&mock_p_ok, 0, sizeof(mock_p_ok));
    mock_p_ok.ops          = &mock_ops_none;
    mock_p_ok.error_mapper = mock_err_mapper;

    memset(&mock_p_inc, 0, sizeof(mock_p_inc));
    mock_p_inc.ops          = &mock_ops_incomplete;
    mock_p_inc.error_mapper = mock_err_mapper;

    memset(&mock_p_fatal, 0, sizeof(mock_p_fatal));
    mock_p_fatal.ops          = &mock_ops_fatal;
    mock_p_fatal.error_mapper = mock_err_mapper;

    memset(&mock_p_timeout, 0, sizeof(mock_p_timeout));
    mock_p_timeout.ops              = &mock_ops_timeout;
    mock_p_timeout.error_mapper     = mock_err_mapper;
    mock_p_timeout.config.timeout_ms = 1000;
}

static protocol_chain *g_chain;
static const uint8_t g_test_data[] = { 0xAA, 0xBB, 0xCC };

static void chain_setup(void) { mock_parser_setup(); mock_time_ms = 0; g_chain = NULL; }
static void chain_teardown(void) { protocol_chain_destroy(g_chain); g_chain = NULL; }

/* ================================================================ */
/*  Category: Create / Destroy                                       */
/* ================================================================ */

void test_chain_create_null_max_returns_null(void)
{
    protocol_chain *c = protocol_chain_create(0);
    TEST_ASSERT_NULL(c);
}

void test_chain_create_and_destroy(void)
{
    g_chain = protocol_chain_create(2);
    TEST_ASSERT_NOT_NULL(g_chain);
    TEST_ASSERT_NULL(protocol_chain_get_locked_parser(g_chain));
}

void test_chain_destroy_null_silent(void)
{
    protocol_chain_destroy(NULL);
}

/* ================================================================ */
/*  Category: Add parser                                             */
/* ================================================================ */

void test_chain_add_parser_success(void)
{
    g_chain = protocol_chain_create(2);
    TEST_ASSERT(protocol_chain_add_parser(g_chain, mock_p_ok_ptr));
}

void test_chain_add_parser_null_chain_returns_false(void)
{
    TEST_ASSERT_FALSE(protocol_chain_add_parser(NULL, mock_p_ok_ptr));
}

void test_chain_add_parser_null_parser_returns_false(void)
{
    g_chain = protocol_chain_create(2);
    TEST_ASSERT_FALSE(protocol_chain_add_parser(g_chain, NULL));
}

void test_chain_add_parser_when_full_returns_false(void)
{
    g_chain = protocol_chain_create(2);
    protocol_chain_add_parser(g_chain, mock_p_ok_ptr);
    protocol_chain_add_parser(g_chain, mock_p_inc_ptr);
    TEST_ASSERT_FALSE(protocol_chain_add_parser(g_chain, mock_p_fatal_ptr));
}

/* ================================================================ */
/*  Category: Remove parser                                          */
/* ================================================================ */

void test_chain_remove_parser_success(void)
{
    g_chain = protocol_chain_create(2);
    protocol_chain_add_parser(g_chain, mock_p_ok_ptr);
    protocol_chain_add_parser(g_chain, mock_p_inc_ptr);
    TEST_ASSERT(protocol_chain_remove_parser(g_chain, mock_p_ok_ptr));
    TEST_ASSERT(protocol_chain_add_parser(g_chain, mock_p_fatal_ptr));
}

void test_chain_remove_parser_not_found_returns_false(void)
{
    g_chain = protocol_chain_create(2);
    protocol_chain_add_parser(g_chain, mock_p_ok_ptr);
    TEST_ASSERT_FALSE(protocol_chain_remove_parser(g_chain, mock_p_inc_ptr));
}

void test_chain_remove_locked_parser_unlocks(void)
{
    g_chain = protocol_chain_create(2);
    protocol_chain_add_parser(g_chain, mock_p_ok_ptr);
    protocol_chain_feed(g_chain, g_test_data, 3);
    TEST_ASSERT(protocol_chain_get_locked_parser(g_chain) == mock_p_ok_ptr);
    TEST_ASSERT(mock_p_ok.locked);

    protocol_chain_remove_parser(g_chain, mock_p_ok_ptr);
    TEST_ASSERT_NULL(protocol_chain_get_locked_parser(g_chain));
    TEST_ASSERT_FALSE(mock_p_ok.locked);
}

void test_chain_remove_parser_null_chain_returns_false(void)
{
    TEST_ASSERT_FALSE(protocol_chain_remove_parser(NULL, mock_p_ok_ptr));
}

void test_chain_remove_parser_null_parser_returns_false(void)
{
    g_chain = protocol_chain_create(2);
    TEST_ASSERT_FALSE(protocol_chain_remove_parser(g_chain, NULL));
}

/* ================================================================ */
/*  Category: Feed — unlocked                                        */
/* ================================================================ */

void test_chain_feed_first_parser_matches(void)
{
    g_chain = protocol_chain_create(2);
    protocol_chain_add_parser(g_chain, mock_p_ok_ptr);
    protocol_chain_add_parser(g_chain, mock_p_inc_ptr);

    parser_error_t err = protocol_chain_feed(g_chain, g_test_data, 3);
    TEST_ASSERT_EQUAL(PARSER_ERR_NONE, err);
    TEST_ASSERT(protocol_chain_get_locked_parser(g_chain) == mock_p_ok_ptr);
    TEST_ASSERT(mock_p_ok.locked);
}

void test_chain_feed_second_parser_wins(void)
{
    g_chain = protocol_chain_create(3);
    protocol_chain_add_parser(g_chain, mock_p_fatal_ptr);
    protocol_chain_add_parser(g_chain, mock_p_ok_ptr);
    protocol_chain_add_parser(g_chain, mock_p_inc_ptr);

    parser_error_t err = protocol_chain_feed(g_chain, g_test_data, 3);
    TEST_ASSERT_EQUAL(PARSER_ERR_NONE, err);
    TEST_ASSERT(protocol_chain_get_locked_parser(g_chain) == mock_p_ok_ptr);
    TEST_ASSERT(mock_p_ok.locked);
    TEST_ASSERT_FALSE(mock_p_fatal.locked);
    TEST_ASSERT_FALSE(mock_p_inc.locked);
}

void test_chain_feed_incomplete_falls_through(void)
{
    g_chain = protocol_chain_create(2);
    protocol_chain_add_parser(g_chain, mock_p_inc_ptr);
    protocol_chain_add_parser(g_chain, mock_p_ok_ptr);

    parser_error_t err = protocol_chain_feed(g_chain, g_test_data, 3);
    TEST_ASSERT_EQUAL(PARSER_ERR_NONE, err);
    TEST_ASSERT(protocol_chain_get_locked_parser(g_chain) == mock_p_ok_ptr);
}

void test_chain_feed_no_match_returns_unknown(void)
{
    g_chain = protocol_chain_create(2);
    protocol_chain_add_parser(g_chain, mock_p_fatal_ptr);
    protocol_chain_add_parser(g_chain, mock_p_fatal_ptr);

    parser_error_t err = protocol_chain_feed(g_chain, g_test_data, 3);
    TEST_ASSERT_EQUAL(PARSER_ERR_UNKNOWN, err);
    TEST_ASSERT_NULL(protocol_chain_get_locked_parser(g_chain));
}

void test_chain_feed_all_incomplete_returns_incomplete(void)
{
    g_chain = protocol_chain_create(2);
    protocol_chain_add_parser(g_chain, mock_p_inc_ptr);
    protocol_chain_add_parser(g_chain, mock_p_inc_ptr);

    parser_error_t err = protocol_chain_feed(g_chain, g_test_data, 3);
    TEST_ASSERT_EQUAL(PARSER_ERR_INCOMPLETE, err);
    TEST_ASSERT_NULL(protocol_chain_get_locked_parser(g_chain));
    TEST_ASSERT_FALSE(mock_p_inc.locked);
}

void test_chain_feed_null_chain_returns_invalid_param(void)
{
    parser_error_t err = protocol_chain_feed(NULL, g_test_data, 3);
    TEST_ASSERT_EQUAL(PARSER_ERR_INVALID_PARAM, err);
}

void test_chain_feed_null_data_returns_invalid_param(void)
{
    g_chain = protocol_chain_create(2);
    protocol_chain_add_parser(g_chain, mock_p_ok_ptr);
    parser_error_t err = protocol_chain_feed(g_chain, NULL, 3);
    TEST_ASSERT_EQUAL(PARSER_ERR_INVALID_PARAM, err);
}

void test_chain_feed_zero_len_returns_invalid_param(void)
{
    g_chain = protocol_chain_create(2);
    protocol_chain_add_parser(g_chain, mock_p_ok_ptr);
    parser_error_t err = protocol_chain_feed(g_chain, g_test_data, 0);
    TEST_ASSERT_EQUAL(PARSER_ERR_INVALID_PARAM, err);
}

void test_chain_feed_empty_chain_returns_invalid_param(void)
{
    g_chain = protocol_chain_create(2);
    parser_error_t err = protocol_chain_feed(g_chain, g_test_data, 3);
    TEST_ASSERT_EQUAL(PARSER_ERR_INVALID_PARAM, err);
}

/* ================================================================ */
/*  Category: Feed — locked                                          */
/* ================================================================ */

void test_chain_feed_locked_routes_to_locked(void)
{
    g_chain = protocol_chain_create(3);
    protocol_chain_add_parser(g_chain, mock_p_ok_ptr);
    protocol_chain_add_parser(g_chain, mock_p_fatal_ptr);

    /* first feed locks p_ok */
    protocol_chain_feed(g_chain, g_test_data, 3);
    TEST_ASSERT(protocol_chain_get_locked_parser(g_chain) == mock_p_ok_ptr);

    /* second feed: ok returns NONE, lock stays */
    parser_error_t err = protocol_chain_feed(g_chain, g_test_data, 3);
    TEST_ASSERT_EQUAL(PARSER_ERR_NONE, err);
    TEST_ASSERT(protocol_chain_get_locked_parser(g_chain) == mock_p_ok_ptr);
    TEST_ASSERT(mock_p_ok.locked);
}

void test_chain_feed_locked_incomplete_keeps_lock(void)
{
    g_chain = protocol_chain_create(2);
    protocol_chain_add_parser(g_chain, mock_p_inc_ptr);
    protocol_chain_add_parser(g_chain, mock_p_ok_ptr);

    /* inc matches (partial), causes saw_incomplete, then ok matches and locks */
    protocol_chain_feed(g_chain, g_test_data, 3);

    /* manually replace inc's ops to ok to ensure locked routing */
    mock_p_inc.ops = &mock_ops_none;

    /* feed again — locked parser (mock_p_ok) is used */
    parser_error_t err = protocol_chain_feed(g_chain, g_test_data, 3);
    TEST_ASSERT_EQUAL(PARSER_ERR_NONE, err);
    TEST_ASSERT(mock_p_ok.locked);
}

void test_chain_feed_locked_fatal_error_unlocks(void)
{
    g_chain = protocol_chain_create(3);
    protocol_chain_add_parser(g_chain, mock_p_ok_ptr);
    protocol_chain_add_parser(g_chain, mock_p_inc_ptr);

    /* lock mock_p_ok */
    protocol_chain_feed(g_chain, g_test_data, 3);
    TEST_ASSERT(mock_p_ok.locked);

    /* make mock_p_ok return fatal */
    mock_p_ok.ops = &mock_ops_fatal;

    parser_error_t err = protocol_chain_feed(g_chain, g_test_data, 3);
    TEST_ASSERT_EQUAL(PARSER_ERR_INCOMPLETE, err);
    TEST_ASSERT_FALSE(mock_p_ok.locked);
    TEST_ASSERT_NULL(protocol_chain_get_locked_parser(g_chain));
}

/* ================================================================ */
/*  Category: Feed frame                                             */
/* ================================================================ */

void test_chain_feed_frame_first_match_locks(void)
{
    g_chain = protocol_chain_create(2);
    protocol_chain_add_parser(g_chain, mock_p_ok_ptr);
    protocol_chain_add_parser(g_chain, mock_p_inc_ptr);

    parser_error_t err = protocol_chain_feed_frame(g_chain, g_test_data, 3);
    TEST_ASSERT_EQUAL(PARSER_ERR_NONE, err);
    TEST_ASSERT(protocol_chain_get_locked_parser(g_chain) == mock_p_ok_ptr);
    TEST_ASSERT(mock_p_ok.locked);
}

void test_chain_feed_frame_locked_any_error_unlocks(void)
{
    g_chain = protocol_chain_create(2);
    protocol_chain_add_parser(g_chain, mock_p_ok_ptr);

    /* lock via feed */
    protocol_chain_feed(g_chain, g_test_data, 3);
    TEST_ASSERT(mock_p_ok.locked);

    /* feed_frame with fatal on locked parser */
    mock_p_ok.ops = &mock_ops_fatal;
    parser_error_t err = protocol_chain_feed_frame(g_chain, g_test_data, 3);
    TEST_ASSERT_EQUAL(PARSER_ERR_FRAME, err);
    TEST_ASSERT_FALSE(mock_p_ok.locked);
    TEST_ASSERT_NULL(protocol_chain_get_locked_parser(g_chain));
}

void test_chain_feed_frame_no_match_returns_unknown(void)
{
    g_chain = protocol_chain_create(2);
    protocol_chain_add_parser(g_chain, mock_p_fatal_ptr);

    parser_error_t err = protocol_chain_feed_frame(g_chain, g_test_data, 3);
    TEST_ASSERT_EQUAL(PARSER_ERR_UNKNOWN, err);
    TEST_ASSERT_NULL(protocol_chain_get_locked_parser(g_chain));
}

void test_chain_feed_frame_null_chain_returns_invalid_param(void)
{
    parser_error_t err = protocol_chain_feed_frame(NULL, g_test_data, 3);
    TEST_ASSERT_EQUAL(PARSER_ERR_INVALID_PARAM, err);
}

void test_chain_feed_frame_null_data_returns_invalid_param(void)
{
    g_chain = protocol_chain_create(2);
    protocol_chain_add_parser(g_chain, mock_p_ok_ptr);
    parser_error_t err = protocol_chain_feed_frame(g_chain, NULL, 3);
    TEST_ASSERT_EQUAL(PARSER_ERR_INVALID_PARAM, err);
}

void test_chain_feed_frame_zero_len_returns_invalid_param(void)
{
    g_chain = protocol_chain_create(2);
    protocol_chain_add_parser(g_chain, mock_p_ok_ptr);
    parser_error_t err = protocol_chain_feed_frame(g_chain, g_test_data, 0);
    TEST_ASSERT_EQUAL(PARSER_ERR_INVALID_PARAM, err);
}

void test_chain_feed_frame_empty_chain_returns_invalid_param(void)
{
    g_chain = protocol_chain_create(2);
    parser_error_t err = protocol_chain_feed_frame(g_chain, g_test_data, 3);
    TEST_ASSERT_EQUAL(PARSER_ERR_INVALID_PARAM, err);
}

/* ================================================================ */
/*  Category: Lock management                                        */
/* ================================================================ */

void test_chain_get_locked_parser_null_chain_returns_null(void)
{
    TEST_ASSERT_NULL(protocol_chain_get_locked_parser(NULL));
}

void test_chain_get_locked_parser_initially_null(void)
{
    g_chain = protocol_chain_create(2);
    protocol_chain_add_parser(g_chain, mock_p_ok_ptr);
    TEST_ASSERT_NULL(protocol_chain_get_locked_parser(g_chain));
}

void test_chain_set_locked_parser_success(void)
{
    g_chain = protocol_chain_create(2);
    protocol_chain_add_parser(g_chain, mock_p_ok_ptr);
    TEST_ASSERT(protocol_chain_set_locked_parser(g_chain, mock_p_ok_ptr));
    TEST_ASSERT(mock_p_ok.locked);
    TEST_ASSERT(protocol_chain_get_locked_parser(g_chain) == mock_p_ok_ptr);
}

void test_chain_set_locked_parser_not_in_chain_returns_false(void)
{
    g_chain = protocol_chain_create(2);
    protocol_chain_add_parser(g_chain, mock_p_ok_ptr);
    TEST_ASSERT_FALSE(protocol_chain_set_locked_parser(g_chain, mock_p_inc_ptr));
    TEST_ASSERT_NULL(protocol_chain_get_locked_parser(g_chain));
}

void test_chain_set_locked_parser_null_unlocks(void)
{
    g_chain = protocol_chain_create(2);
    protocol_chain_add_parser(g_chain, mock_p_ok_ptr);
    protocol_chain_set_locked_parser(g_chain, mock_p_ok_ptr);
    TEST_ASSERT(mock_p_ok.locked);

    TEST_ASSERT(protocol_chain_set_locked_parser(g_chain, NULL));
    TEST_ASSERT_FALSE(mock_p_ok.locked);
    TEST_ASSERT_NULL(protocol_chain_get_locked_parser(g_chain));
}

void test_chain_set_locked_parser_null_chain_returns_false(void)
{
    TEST_ASSERT_FALSE(protocol_chain_set_locked_parser(NULL, mock_p_ok_ptr));
}

/* ================================================================ */
/*  Category: Timeout poll                                           */
/* ================================================================ */

void test_chain_timeout_poll_null_chain_returns_false(void)
{
    TEST_ASSERT_FALSE(protocol_chain_check_timeout_poll(NULL));
}

void test_chain_timeout_poll_empty_chain_returns_false(void)
{
    g_chain = protocol_chain_create(2);
    TEST_ASSERT_FALSE(protocol_chain_check_timeout_poll(g_chain));
}

void test_chain_timeout_poll_locked_triggers_and_unlocks(void)
{
    g_chain = protocol_chain_create(2);
    protocol_chain_add_parser(g_chain, mock_p_timeout_ptr);

    /* lock via set_locked */
    protocol_chain_set_locked_parser(g_chain, mock_p_timeout_ptr);
    /* parse_data to activate timeout */
    protocol_parser_parse_data(mock_p_timeout_ptr, g_test_data, 3);
    mock_time_ms = 0;

    /* not timed out yet */
    TEST_ASSERT_FALSE(protocol_chain_check_timeout_poll(g_chain));
    TEST_ASSERT(mock_p_timeout.locked);

    /* advance past timeout */
    mock_time_ms = 1100;

    TEST_ASSERT(protocol_chain_check_timeout_poll(g_chain));
    TEST_ASSERT_FALSE(mock_p_timeout.locked);
    TEST_ASSERT_NULL(protocol_chain_get_locked_parser(g_chain));
}

void test_chain_timeout_poll_unlocked_checks_all(void)
{
    g_chain = protocol_chain_create(2);
    protocol_chain_add_parser(g_chain, mock_p_timeout_ptr);

    /* activate timeout on parser */
    protocol_parser_parse_data(mock_p_timeout_ptr, g_test_data, 3);
    mock_time_ms = 0;

    TEST_ASSERT_FALSE(protocol_chain_check_timeout_poll(g_chain));

    mock_time_ms = 1100;
    TEST_ASSERT(protocol_chain_check_timeout_poll(g_chain));
}

/* ================================================================ */
/*  Category: Feed after manual lock — respect user lock             */
/* ================================================================ */

void test_chain_feed_respects_user_manual_lock(void)
{
    g_chain = protocol_chain_create(3);
    protocol_chain_add_parser(g_chain, mock_p_inc_ptr);
    protocol_chain_add_parser(g_chain, mock_p_ok_ptr);

    /* user manually locks inc parser */
    protocol_chain_set_locked_parser(g_chain, mock_p_inc_ptr);
    mock_p_inc.ops = &mock_ops_none;

    /* feed: locked parser is inc (now returns NONE), stays locked */
    parser_error_t err = protocol_chain_feed(g_chain, g_test_data, 3);
    TEST_ASSERT_EQUAL(PARSER_ERR_NONE, err);
    TEST_ASSERT(protocol_chain_get_locked_parser(g_chain) == mock_p_inc_ptr);
}

void test_chain_feed_user_lock_prevents_new_match(void)
{
    g_chain = protocol_chain_create(3);
    protocol_chain_add_parser(g_chain, mock_p_ok_ptr);
    protocol_chain_add_parser(g_chain, mock_p_inc_ptr);

    /* user manually locks the second (inc) parser */
    protocol_chain_set_locked_parser(g_chain, mock_p_inc_ptr);
    mock_p_inc.ops = &mock_ops_none;

    /* feed: locked parser honored, even though p_ok would match first */
    parser_error_t err = protocol_chain_feed(g_chain, g_test_data, 3);
    TEST_ASSERT_EQUAL(PARSER_ERR_NONE, err);
    TEST_ASSERT(protocol_chain_get_locked_parser(g_chain) == mock_p_inc_ptr);
    TEST_ASSERT_FALSE(mock_p_ok.locked);
}

/* ================================================================ */
/*  Category: feed_frame locked still follows order after unlock     */
/* ================================================================ */

void test_chain_feed_frame_locked_none_keeps_lock(void)
{
    g_chain = protocol_chain_create(2);
    protocol_chain_add_parser(g_chain, mock_p_ok_ptr);

    /* lock via feed */
    protocol_chain_feed(g_chain, g_test_data, 3);
    TEST_ASSERT(mock_p_ok.locked);

    parser_error_t err = protocol_chain_feed_frame(g_chain, g_test_data, 3);
    TEST_ASSERT_EQUAL(PARSER_ERR_NONE, err);
    TEST_ASSERT(mock_p_ok.locked);
}

/* ================================================================ */
/*  Category: Chain with single parser                               */
/* ================================================================ */

void test_chain_single_parser_feed_matches(void)
{
    g_chain = protocol_chain_create(1);
    protocol_chain_add_parser(g_chain, mock_p_ok_ptr);

    parser_error_t err = protocol_chain_feed(g_chain, g_test_data, 3);
    TEST_ASSERT_EQUAL(PARSER_ERR_NONE, err);
    TEST_ASSERT(mock_p_ok.locked);
}

void test_chain_single_parser_feed_no_match(void)
{
    g_chain = protocol_chain_create(1);
    protocol_chain_add_parser(g_chain, mock_p_fatal_ptr);

    parser_error_t err = protocol_chain_feed(g_chain, g_test_data, 3);
    TEST_ASSERT_EQUAL(PARSER_ERR_UNKNOWN, err);
    TEST_ASSERT_NULL(protocol_chain_get_locked_parser(g_chain));
}

/* ================================================================ */
/*  Category: remove from middle preserves order                     */
/* ================================================================ */

void test_chain_remove_middle_preserves_remaining(void)
{
    g_chain = protocol_chain_create(3);
    protocol_chain_add_parser(g_chain, mock_p_ok_ptr);
    protocol_chain_add_parser(g_chain, mock_p_fatal_ptr);
    protocol_chain_add_parser(g_chain, mock_p_inc_ptr);

    protocol_chain_remove_parser(g_chain, mock_p_fatal_ptr);

    /* feed: ok matches and locks */
    parser_error_t err = protocol_chain_feed(g_chain, g_test_data, 3);
    TEST_ASSERT_EQUAL(PARSER_ERR_NONE, err);
    TEST_ASSERT(protocol_chain_get_locked_parser(g_chain) == mock_p_ok_ptr);
}

/* ================================================================ */
/*  Runner                                                            */
/* ================================================================ */

int chain_run_all_tests(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_chain_create_null_max_returns_null);
    RUN_TEST(test_chain_create_and_destroy);
    RUN_TEST(test_chain_destroy_null_silent);

    RUN_TEST(test_chain_add_parser_success);
    RUN_TEST(test_chain_add_parser_null_chain_returns_false);
    RUN_TEST(test_chain_add_parser_null_parser_returns_false);
    RUN_TEST(test_chain_add_parser_when_full_returns_false);

    RUN_TEST(test_chain_remove_parser_success);
    RUN_TEST(test_chain_remove_parser_not_found_returns_false);
    RUN_TEST(test_chain_remove_locked_parser_unlocks);
    RUN_TEST(test_chain_remove_parser_null_chain_returns_false);
    RUN_TEST(test_chain_remove_parser_null_parser_returns_false);

    RUN_TEST(test_chain_feed_first_parser_matches);
    RUN_TEST(test_chain_feed_second_parser_wins);
    RUN_TEST(test_chain_feed_incomplete_falls_through);
    RUN_TEST(test_chain_feed_no_match_returns_unknown);
    RUN_TEST(test_chain_feed_all_incomplete_returns_incomplete);
    RUN_TEST(test_chain_feed_null_chain_returns_invalid_param);
    RUN_TEST(test_chain_feed_null_data_returns_invalid_param);
    RUN_TEST(test_chain_feed_zero_len_returns_invalid_param);
    RUN_TEST(test_chain_feed_empty_chain_returns_invalid_param);

    RUN_TEST(test_chain_feed_locked_routes_to_locked);
    RUN_TEST(test_chain_feed_locked_incomplete_keeps_lock);
    RUN_TEST(test_chain_feed_locked_fatal_error_unlocks);

    RUN_TEST(test_chain_feed_frame_first_match_locks);
    RUN_TEST(test_chain_feed_frame_locked_any_error_unlocks);
    RUN_TEST(test_chain_feed_frame_no_match_returns_unknown);
    RUN_TEST(test_chain_feed_frame_null_chain_returns_invalid_param);
    RUN_TEST(test_chain_feed_frame_null_data_returns_invalid_param);
    RUN_TEST(test_chain_feed_frame_zero_len_returns_invalid_param);
    RUN_TEST(test_chain_feed_frame_empty_chain_returns_invalid_param);

    RUN_TEST(test_chain_get_locked_parser_null_chain_returns_null);
    RUN_TEST(test_chain_get_locked_parser_initially_null);
    RUN_TEST(test_chain_set_locked_parser_success);
    RUN_TEST(test_chain_set_locked_parser_not_in_chain_returns_false);
    RUN_TEST(test_chain_set_locked_parser_null_unlocks);
    RUN_TEST(test_chain_set_locked_parser_null_chain_returns_false);

    RUN_TEST(test_chain_timeout_poll_null_chain_returns_false);
    RUN_TEST(test_chain_timeout_poll_empty_chain_returns_false);
    RUN_TEST(test_chain_timeout_poll_locked_triggers_and_unlocks);
    RUN_TEST(test_chain_timeout_poll_unlocked_checks_all);

    RUN_TEST(test_chain_feed_respects_user_manual_lock);
    RUN_TEST(test_chain_feed_user_lock_prevents_new_match);

    RUN_TEST(test_chain_feed_frame_locked_none_keeps_lock);

    RUN_TEST(test_chain_single_parser_feed_matches);
    RUN_TEST(test_chain_single_parser_feed_no_match);

    RUN_TEST(test_chain_remove_middle_preserves_remaining);

    return UNITY_END();
}

#include "test_suite.h"
TEST_SUITE_DEFINE(protocol_chain, chain_setup, chain_teardown, chain_run_all_tests);
