#include "unity.h"
#include "test_suite.h"
#include "protocol_parser_imperx.h"
#include <string.h>

extern uint32_t mock_time_ms;

/* ================================================================ */
/*  Test infrastructure                                              */
/* ================================================================ */

#define IMPERX_TEST_BUF_SIZE 32

static uint8_t g_rx_buf[IMPERX_TEST_BUF_SIZE];
static uint8_t g_tx_buf[IMPERX_TEST_BUF_SIZE];

static imperx_protocol_parser_t *g_parser;

static bool     g_frame_ready_called;
static int      g_frame_ready_count;
static uint64_t g_frame_ready_parsed_id;
static int      g_frame_ready_parsed_cmd;
static uint32_t g_frame_ready_parsed_len;
static uint8_t  g_frame_ready_parsed_data[16];

static bool     g_tx_ready_called;
static int      g_tx_ready_count;
static uint32_t g_tx_ready_len;

static void on_frame_ready_cb(protocol_parser_t *parser, void *parsed, void *ctx)
{
    (void)ctx;
    imperx_private_t *pri = (imperx_private_t *)parsed;
    g_frame_ready_called = true;
    g_frame_ready_count++;
    g_frame_ready_parsed_id  = pri->parsed_id;
    g_frame_ready_parsed_cmd = pri->parsed_cmd;
    g_frame_ready_parsed_len = pri->parsed_len;

    if (pri->parsed_data && pri->parsed_len > 0 && pri->parsed_len <= sizeof(g_frame_ready_parsed_data)) {
        memcpy(g_frame_ready_parsed_data, pri->parsed_data, pri->parsed_len);
    }

    /* fill response data for READ commands (as real app does) */
    if (pri->parsed_cmd == IMPERX_CMD_READ) {
        static uint8_t resp[4] = { 0x12, 0x34, 0x56, 0x78 };
        pri->parsed_data = resp;
        pri->parsed_len  = sizeof(resp);
    }
}

static void on_tx_ready_cb(protocol_parser_t *parser, void *ctx)
{
    (void)ctx;
    g_tx_ready_called = true;
    g_tx_ready_count++;
    g_tx_ready_len = parser->tx.data_len;
}

static void imperx_setup(void)
{
    memset(g_rx_buf, 0, sizeof(g_rx_buf));
    memset(g_tx_buf, 0, sizeof(g_tx_buf));
    mock_time_ms = 0;

    g_frame_ready_called = false;
    g_frame_ready_count  = 0;
    g_tx_ready_called    = false;
    g_tx_ready_count     = 0;
    memset(g_frame_ready_parsed_data, 0, sizeof(g_frame_ready_parsed_data));

    g_parser = imperx_protocol_create(g_rx_buf, sizeof(g_rx_buf),
                                      g_tx_buf, sizeof(g_tx_buf));
    TEST_ASSERT_NOT_NULL(g_parser);

    protocol_parser_set_callbacks((protocol_parser_t *)g_parser,
                                  on_frame_ready_cb, NULL,
                                  on_tx_ready_cb, NULL);
}

static void imperx_teardown(void)
{
    if (g_parser) {
        protocol_parser_destroy((protocol_parser_t *)g_parser);
        g_parser = NULL;
    }
}

/* ================================================================ */
/*  Category: Create / API validation                                */
/* ================================================================ */

void test_imperx_create_with_buffers_succeeds(void)
{
    TEST_ASSERT_NOT_NULL(g_parser);
    imperx_private_t *pri = &g_parser->pri;
    TEST_ASSERT_EQUAL(IMPERX_STATE_WAIT_HEAD, pri->state);
    TEST_ASSERT_EQUAL(IMPERX_CMD_READ, pri->parsed_cmd);
    TEST_ASSERT_EQUAL(0, pri->parsed_id);
    TEST_ASSERT_NULL(pri->parsed_data);
    TEST_ASSERT_EQUAL(0, pri->header_errors);
}

void test_imperx_create_null_buffers_dynamic_alloc(void)
{
    imperx_protocol_parser_t *p = imperx_protocol_create(NULL, 0, NULL, 0);
    TEST_ASSERT_NOT_NULL(p);
    protocol_parser_destroy((protocol_parser_t *)p);
}

void test_imperx_create_small_rx_buffer_fails(void)
{
    uint8_t small_buf[4];
    imperx_protocol_parser_t *p = imperx_protocol_create(small_buf, sizeof(small_buf),
                                                          g_tx_buf, sizeof(g_tx_buf));
    TEST_ASSERT_NULL(p);
}

void test_imperx_create_small_tx_buffer_fails(void)
{
    uint8_t small_buf[4];
    imperx_protocol_parser_t *p = imperx_protocol_create(g_rx_buf, sizeof(g_rx_buf),
                                                          small_buf, sizeof(small_buf));
    TEST_ASSERT_NULL(p);
}

/* ================================================================ */
/*  Category: READ command                                           */
/* ================================================================ */

void test_imperx_read_full_frame(void)
{
    uint8_t frame[] = { 0x52, 0xAB, 0xCD };
    parser_error_t err = protocol_parser_parse_data((protocol_parser_t *)g_parser, frame, 3);

    TEST_ASSERT_EQUAL(PARSER_ERR_NONE, err);
    TEST_ASSERT(g_frame_ready_called);
    TEST_ASSERT_EQUAL(0xABCD, g_frame_ready_parsed_id);
    TEST_ASSERT_EQUAL(IMPERX_CMD_READ, g_frame_ready_parsed_cmd);
    TEST_ASSERT_EQUAL(0, g_frame_ready_parsed_len);

    TEST_ASSERT(g_tx_ready_called);
    TEST_ASSERT_EQUAL(5, g_tx_ready_len);
    TEST_ASSERT_EQUAL(0x06, g_tx_buf[0]);
    TEST_ASSERT_EQUAL(0x12, g_tx_buf[1]);
    TEST_ASSERT_EQUAL(0x34, g_tx_buf[2]);
    TEST_ASSERT_EQUAL(0x56, g_tx_buf[3]);
    TEST_ASSERT_EQUAL(0x78, g_tx_buf[4]);
}

void test_imperx_read_byte_by_byte(void)
{
    uint8_t frame[] = { 0x52, 0x11, 0x22 };
    for (int i = 0; i < 3; i++) {
        parser_error_t err = protocol_parser_parse_data((protocol_parser_t *)g_parser, &frame[i], 1);
        if (i < 2) {
            TEST_ASSERT_EQUAL(PARSER_ERR_INCOMPLETE, err);
            TEST_ASSERT_FALSE(g_frame_ready_called);
        } else {
            TEST_ASSERT_EQUAL(PARSER_ERR_NONE, err);
            TEST_ASSERT(g_frame_ready_called);
            TEST_ASSERT_EQUAL(0x1122, g_frame_ready_parsed_id);
        }
    }
}

void test_imperx_read_two_byte_chunk(void)
{
    uint8_t chunk1[] = { 0x52, 0xDE };
    uint8_t chunk2[] = { 0xAD };

    parser_error_t err = protocol_parser_parse_data((protocol_parser_t *)g_parser, chunk1, 2);
    TEST_ASSERT_EQUAL(PARSER_ERR_INCOMPLETE, err);

    err = protocol_parser_parse_data((protocol_parser_t *)g_parser, chunk2, 1);
    TEST_ASSERT_EQUAL(PARSER_ERR_NONE, err);
    TEST_ASSERT_EQUAL(0xDEAD, g_frame_ready_parsed_id);
}

void test_imperx_read_incomplete_returns_incomplete(void)
{
    uint8_t frame[] = { 0x52, 0x01 };
    parser_error_t err = protocol_parser_parse_data((protocol_parser_t *)g_parser, frame, 2);
    TEST_ASSERT_EQUAL(PARSER_ERR_INCOMPLETE, err);
    TEST_ASSERT_FALSE(g_frame_ready_called);
}

/* ================================================================ */
/*  Category: WRITE command                                          */
/* ================================================================ */

void test_imperx_write_full_frame(void)
{
    uint8_t frame[] = { 0x57, 0x01, 0x23, 0xAA, 0xBB, 0xCC, 0xDD };
    parser_error_t err = protocol_parser_parse_data((protocol_parser_t *)g_parser, frame, 7);

    TEST_ASSERT_EQUAL(PARSER_ERR_NONE, err);
    TEST_ASSERT(g_frame_ready_called);
    TEST_ASSERT_EQUAL(0x0123, g_frame_ready_parsed_id);
    TEST_ASSERT_EQUAL(IMPERX_CMD_WRITE, g_frame_ready_parsed_cmd);
    TEST_ASSERT_EQUAL(4, g_frame_ready_parsed_len);
    TEST_ASSERT_EQUAL(0xDD, g_frame_ready_parsed_data[0]);
    TEST_ASSERT_EQUAL(0xCC, g_frame_ready_parsed_data[1]);
    TEST_ASSERT_EQUAL(0xBB, g_frame_ready_parsed_data[2]);
    TEST_ASSERT_EQUAL(0xAA, g_frame_ready_parsed_data[3]);

    TEST_ASSERT(g_tx_ready_called);
    TEST_ASSERT_EQUAL(1, g_tx_ready_len);
    TEST_ASSERT_EQUAL(0x06, g_tx_buf[0]);
}

void test_imperx_write_byte_by_byte(void)
{
    uint8_t frame[] = { 0x57, 0xFE, 0xDC, 0x10, 0x20, 0x30, 0x40 };
    for (int i = 0; i < 7; i++) {
        parser_error_t err = protocol_parser_parse_data((protocol_parser_t *)g_parser, &frame[i], 1);
        if (i < 6) {
            TEST_ASSERT_EQUAL(PARSER_ERR_INCOMPLETE, err);
        } else {
            TEST_ASSERT_EQUAL(PARSER_ERR_NONE, err);
            TEST_ASSERT(g_frame_ready_called);
            TEST_ASSERT_EQUAL(0xFEDC, g_frame_ready_parsed_id);
            TEST_ASSERT_EQUAL(0x40, g_frame_ready_parsed_data[0]);
            TEST_ASSERT_EQUAL(0x30, g_frame_ready_parsed_data[1]);
            TEST_ASSERT_EQUAL(0x20, g_frame_ready_parsed_data[2]);
            TEST_ASSERT_EQUAL(0x10, g_frame_ready_parsed_data[3]);
        }
    }
}

void test_imperx_write_incomplete(void)
{
    uint8_t frame[] = { 0x57, 0xAB, 0xCD };
    parser_error_t err = protocol_parser_parse_data((protocol_parser_t *)g_parser, frame, 3);
    TEST_ASSERT_EQUAL(PARSER_ERR_INCOMPLETE, err);
}

void test_imperx_write_incomplete_mid_data(void)
{
    uint8_t frame[] = { 0x57, 0xAB, 0xCD, 0x01, 0x02 };
    parser_error_t err = protocol_parser_parse_data((protocol_parser_t *)g_parser, frame, 5);
    TEST_ASSERT_EQUAL(PARSER_ERR_INCOMPLETE, err);
}

/* ================================================================ */
/*  Category: Invalid header                                         */
/* ================================================================ */

void test_imperx_invalid_header_returns_frame_error(void)
{
    uint8_t invalid = 0xFF;
    parser_error_t err = protocol_parser_parse_data((protocol_parser_t *)g_parser, &invalid, 1);
    TEST_ASSERT_EQUAL(PARSER_ERR_FRAME, err);
    TEST_ASSERT_FALSE(g_frame_ready_called);
}

void test_imperx_invalid_header_suppressed_when_unlocked(void)
{
    uint8_t invalid = 0xAB;
    parser_error_t err = protocol_parser_parse_data((protocol_parser_t *)g_parser, &invalid, 1);
    TEST_ASSERT_EQUAL(PARSER_ERR_FRAME, err);
    TEST_ASSERT_FALSE(g_tx_ready_called);
}

void test_imperx_invalid_header_sends_nack_when_locked(void)
{
    g_parser->base.locked = true;
    uint8_t invalid = 0x00;

    protocol_parser_parse_data((protocol_parser_t *)g_parser, &invalid, 1);

    TEST_ASSERT(g_tx_ready_called);
    TEST_ASSERT_EQUAL(2, g_tx_ready_len);
    TEST_ASSERT_EQUAL(0x15, g_tx_buf[0]);
    TEST_ASSERT_EQUAL(0x01, g_tx_buf[1]);
}

void test_imperx_invalid_header_increments_counter(void)
{
    imperx_private_t *pri = &g_parser->pri;
    TEST_ASSERT_EQUAL(0, pri->header_errors);

    uint8_t invalid = 0xAB;
    protocol_parser_parse_data((protocol_parser_t *)g_parser, &invalid, 1);
    TEST_ASSERT_EQUAL(1, pri->header_errors);

    protocol_parser_parse_data((protocol_parser_t *)g_parser, &invalid, 1);
    TEST_ASSERT_EQUAL(2, pri->header_errors);
}

void test_imperx_invalid_header_resets_state(void)
{
    uint8_t invalid = 0xAB;
    protocol_parser_parse_data((protocol_parser_t *)g_parser, &invalid, 1);
    TEST_ASSERT_EQUAL(IMPERX_STATE_WAIT_HEAD, g_parser->pri.state);

    /* after error, valid frame still works */
    uint8_t read_frame[] = { 0x52, 0x00, 0x01 };
    parser_error_t err = protocol_parser_parse_data((protocol_parser_t *)g_parser, read_frame, 3);
    TEST_ASSERT_EQUAL(PARSER_ERR_NONE, err);
    TEST_ASSERT(g_frame_ready_called);
}

void test_imperx_invalid_header_during_frame(void)
{
    /* start with valid head */
    uint8_t head = 0x52;
    protocol_parser_parse_data((protocol_parser_t *)g_parser, &head, 1);
    TEST_ASSERT_EQUAL(IMPERX_STATE_WAIT_ID, g_parser->pri.state);

    /* next byte triggers header error from WAIT_ID? No — WAIT_ID accepts any byte */
    uint8_t addr = 0xAB;
    protocol_parser_parse_data((protocol_parser_t *)g_parser, &addr, 1);
    TEST_ASSERT_EQUAL(IMPERX_STATE_WAIT_ID, g_parser->pri.state);
}

/* ================================================================ */
/*  Category: Consecutive frames                                     */
/* ================================================================ */

void test_imperx_two_consecutive_reads(void)
{
    uint8_t frame1[] = { 0x52, 0x00, 0x01 };
    uint8_t frame2[] = { 0x52, 0x00, 0x02 };

    protocol_parser_parse_data((protocol_parser_t *)g_parser, frame1, 3);
    TEST_ASSERT_EQUAL(0x0001, g_frame_ready_parsed_id);

    g_frame_ready_called = false;
    g_tx_ready_called = false;

    protocol_parser_parse_data((protocol_parser_t *)g_parser, frame2, 3);
    TEST_ASSERT(g_frame_ready_called);
    TEST_ASSERT_EQUAL(0x0002, g_frame_ready_parsed_id);
}

void test_imperx_read_then_write(void)
{
    uint8_t read_frame[]  = { 0x52, 0x10, 0x20 };
    uint8_t write_frame[] = { 0x57, 0x30, 0x40, 0x01, 0x02, 0x03, 0x04 };

    protocol_parser_parse_data((protocol_parser_t *)g_parser, read_frame, 3);
    TEST_ASSERT_EQUAL(IMPERX_CMD_READ, g_frame_ready_parsed_cmd);
    TEST_ASSERT_EQUAL(0x1020, g_frame_ready_parsed_id);

    g_frame_ready_called = false;

    protocol_parser_parse_data((protocol_parser_t *)g_parser, write_frame, 7);
    TEST_ASSERT(g_frame_ready_called);
    TEST_ASSERT_EQUAL(IMPERX_CMD_WRITE, g_frame_ready_parsed_cmd);
    TEST_ASSERT_EQUAL(0x3040, g_frame_ready_parsed_id);
}

void test_imperx_write_then_read(void)
{
    uint8_t write_frame[] = { 0x57, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF };
    uint8_t read_frame[]  = { 0x52, 0xCC, 0xDD };

    protocol_parser_parse_data((protocol_parser_t *)g_parser, write_frame, 7);
    TEST_ASSERT_EQUAL(IMPERX_CMD_WRITE, g_frame_ready_parsed_cmd);

    g_frame_ready_called = false;

    protocol_parser_parse_data((protocol_parser_t *)g_parser, read_frame, 3);
    TEST_ASSERT(g_frame_ready_called);
    TEST_ASSERT_EQUAL(IMPERX_CMD_READ, g_frame_ready_parsed_cmd);
}

/* ================================================================ */
/*  Category: Encode / response                                      */
/* ================================================================ */

void test_imperx_encode_write_response_ack_only(void)
{
    uint8_t frame[] = { 0x57, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };
    g_tx_ready_called = false;
    protocol_parser_parse_data((protocol_parser_t *)g_parser, frame, 7);

    TEST_ASSERT(g_tx_ready_called);
    TEST_ASSERT_EQUAL(1, g_tx_ready_len);
    TEST_ASSERT_EQUAL(0x06, g_tx_buf[0]);
}

void test_imperx_encode_read_response_with_data(void)
{
    uint8_t frame[] = { 0x52, 0x00, 0x01 };

    protocol_parser_parse_data((protocol_parser_t *)g_parser, frame, 3);

    TEST_ASSERT_EQUAL(5, g_tx_ready_len);
    TEST_ASSERT_EQUAL(0x06, g_tx_buf[0]);
    TEST_ASSERT_EQUAL(0x12, g_tx_buf[1]);
    TEST_ASSERT_EQUAL(0x34, g_tx_buf[2]);
    TEST_ASSERT_EQUAL(0x56, g_tx_buf[3]);
    TEST_ASSERT_EQUAL(0x78, g_tx_buf[4]);
}

/* ================================================================ */
/*  Category: Null / zero-length data                                */
/* ================================================================ */

void test_imperx_null_data_returns_invalid_param(void)
{
    parser_error_t err = protocol_parser_parse_data((protocol_parser_t *)g_parser, NULL, 1);
    TEST_ASSERT_EQUAL(PARSER_ERR_INVALID_PARAM, err);
}

void test_imperx_zero_len_returns_invalid_param(void)
{
    uint8_t dummy = 0x52;
    parser_error_t err = protocol_parser_parse_data((protocol_parser_t *)g_parser, &dummy, 0);
    TEST_ASSERT_EQUAL(PARSER_ERR_INVALID_PARAM, err);
}

void test_imperx_null_parser_silent(void)
{
    uint8_t dummy = 0x52;
    parser_error_t err = protocol_parser_parse_data(NULL, &dummy, 1);
    TEST_ASSERT_EQUAL(PARSER_ERR_INVALID_PARAM, err);
}

/* ================================================================ */
/*  Category: Reset                                                  */
/* ================================================================ */

void test_imperx_reset_clears_state(void)
{
    /* advance state to WAIT_ID */
    uint8_t head = 0x52;
    protocol_parser_parse_data((protocol_parser_t *)g_parser, &head, 1);
    TEST_ASSERT_EQUAL(IMPERX_STATE_WAIT_ID, g_parser->pri.state);

    /* call reset via ops->reset */
    g_parser->base.ops->reset((protocol_parser_t *)g_parser);
    TEST_ASSERT_EQUAL(IMPERX_STATE_WAIT_HEAD, g_parser->pri.state);
    TEST_ASSERT_EQUAL(IMPERX_CMD_READ, g_parser->pri.parsed_cmd);
    TEST_ASSERT_EQUAL(0, g_parser->pri.parsed_id);
    TEST_ASSERT_NULL(g_parser->pri.parsed_data);
    TEST_ASSERT_EQUAL(0, g_parser->pri.parsed_len);
}

void test_imperx_reset_clears_rx_buffer(void)
{
    uint8_t head = 0x52;
    protocol_parser_parse_data((protocol_parser_t *)g_parser, &head, 1);
    TEST_ASSERT_EQUAL(1, g_parser->base.rx.data_len);

    g_parser->base.ops->reset((protocol_parser_t *)g_parser);
    TEST_ASSERT_EQUAL(0, g_parser->base.rx.data_len);
}

/* ================================================================ */
/*  Category: Frame callback counts                                  */
/* ================================================================ */

void test_imperx_callback_counts_multiple_frames(void)
{
    uint8_t f1[] = { 0x52, 0x00, 0x01 };
    uint8_t f2[] = { 0x57, 0x00, 0x02, 0xAA, 0xBB, 0xCC, 0xDD };
    uint8_t f3[] = { 0x52, 0x00, 0x03 };

    protocol_parser_parse_data((protocol_parser_t *)g_parser, f1, 3);
    protocol_parser_parse_data((protocol_parser_t *)g_parser, f2, 7);
    protocol_parser_parse_data((protocol_parser_t *)g_parser, f3, 3);

    TEST_ASSERT_EQUAL(3, g_frame_ready_count);
    TEST_ASSERT_EQUAL(3, g_tx_ready_count);
}

/* ================================================================ */
/*  Category: Stats tracking                                         */
/* ================================================================ */

void test_imperx_stats_frames_received(void)
{
    parser_stats_t stats;
    protocol_parser_get_stats((protocol_parser_t *)g_parser, &stats);
    TEST_ASSERT_EQUAL(0, stats.frames_received);

    uint8_t f1[] = { 0x52, 0x00, 0x01 };
    uint8_t f2[] = { 0x57, 0x00, 0x02, 0xAA, 0xBB, 0xCC, 0xDD };

    protocol_parser_parse_data((protocol_parser_t *)g_parser, f1, 3);
    protocol_parser_parse_data((protocol_parser_t *)g_parser, f2, 7);

    protocol_parser_get_stats((protocol_parser_t *)g_parser, &stats);
    TEST_ASSERT_EQUAL(2, stats.frames_received);
}

void test_imperx_stats_frame_errors(void)
{
    parser_stats_t stats;
    uint8_t invalid = 0xFF;
    protocol_parser_parse_data((protocol_parser_t *)g_parser, &invalid, 1);

    protocol_parser_get_stats((protocol_parser_t *)g_parser, &stats);
    TEST_ASSERT_EQUAL(1, stats.frame_errors);
}

/* ================================================================ */
/*  Category: WRITE data correctly placed in rx_buffer               */
/* ================================================================ */

void test_imperx_write_parsed_data_in_rx_buffer(void)
{
    uint8_t frame[] = { 0x57, 0x12, 0x34, 0xDE, 0xAD, 0xBE, 0xEF };
    protocol_parser_parse_data((protocol_parser_t *)g_parser, frame, 7);

    /* parsed_data should point into rx_buffer at byte index 3 */
    TEST_ASSERT_EQUAL(0xEF, g_frame_ready_parsed_data[0]);
    TEST_ASSERT_EQUAL(0xBE, g_frame_ready_parsed_data[1]);
    TEST_ASSERT_EQUAL(0xAD, g_frame_ready_parsed_data[2]);
    TEST_ASSERT_EQUAL(0xDE, g_frame_ready_parsed_data[3]);

    /* verify rx_buffer holds the frame */
    TEST_ASSERT_EQUAL(0x57, g_rx_buf[0]);
    TEST_ASSERT_EQUAL(0x12, g_rx_buf[1]);
    TEST_ASSERT_EQUAL(0x34, g_rx_buf[2]);
}

/* ================================================================ */
/*  Category: Callback not called on error                           */
/* ================================================================ */

void test_imperx_frame_ready_not_called_on_error(void)
{
    uint8_t invalid = 0xFF;
    g_frame_ready_called = false;
    g_frame_ready_count = 0;

    protocol_parser_parse_data((protocol_parser_t *)g_parser, &invalid, 1);
    TEST_ASSERT_FALSE(g_frame_ready_called);
    TEST_ASSERT_EQUAL(0, g_frame_ready_count);
}

/* ================================================================ */
/*  Runner                                                            */
/* ================================================================ */

int imperx_run_all_tests(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_imperx_create_with_buffers_succeeds);
    RUN_TEST(test_imperx_create_null_buffers_dynamic_alloc);
    RUN_TEST(test_imperx_create_small_rx_buffer_fails);
    RUN_TEST(test_imperx_create_small_tx_buffer_fails);

    RUN_TEST(test_imperx_read_full_frame);
    RUN_TEST(test_imperx_read_byte_by_byte);
    RUN_TEST(test_imperx_read_two_byte_chunk);
    RUN_TEST(test_imperx_read_incomplete_returns_incomplete);

    RUN_TEST(test_imperx_write_full_frame);
    RUN_TEST(test_imperx_write_byte_by_byte);
    RUN_TEST(test_imperx_write_incomplete);
    RUN_TEST(test_imperx_write_incomplete_mid_data);

    RUN_TEST(test_imperx_invalid_header_returns_frame_error);
    RUN_TEST(test_imperx_invalid_header_suppressed_when_unlocked);
    RUN_TEST(test_imperx_invalid_header_sends_nack_when_locked);
    RUN_TEST(test_imperx_invalid_header_increments_counter);
    RUN_TEST(test_imperx_invalid_header_resets_state);
    RUN_TEST(test_imperx_invalid_header_during_frame);

    RUN_TEST(test_imperx_two_consecutive_reads);
    RUN_TEST(test_imperx_read_then_write);
    RUN_TEST(test_imperx_write_then_read);

    RUN_TEST(test_imperx_encode_write_response_ack_only);
    RUN_TEST(test_imperx_encode_read_response_with_data);

    RUN_TEST(test_imperx_null_data_returns_invalid_param);
    RUN_TEST(test_imperx_zero_len_returns_invalid_param);
    RUN_TEST(test_imperx_null_parser_silent);

    RUN_TEST(test_imperx_reset_clears_state);
    RUN_TEST(test_imperx_reset_clears_rx_buffer);

    RUN_TEST(test_imperx_callback_counts_multiple_frames);
    RUN_TEST(test_imperx_stats_frames_received);
    RUN_TEST(test_imperx_stats_frame_errors);

    RUN_TEST(test_imperx_write_parsed_data_in_rx_buffer);
    RUN_TEST(test_imperx_frame_ready_not_called_on_error);

    return UNITY_END();
}

#include "test_suite.h"
TEST_SUITE_DEFINE(imperx, imperx_setup, imperx_teardown, imperx_run_all_tests);
