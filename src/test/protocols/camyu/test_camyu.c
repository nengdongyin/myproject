#include "unity.h"
#include "test_suite.h"
#include "protocol_parser_camyu.h"
#include <string.h>

extern uint32_t mock_time_ms;

/* ================================================================ */
/*  CRC-8/MAXIM lookup table (copied from protocol_parser_camyu.c)  */
/* ================================================================ */

static const uint8_t CRC8Table[] = {
    0, 94, 188, 226, 97, 63, 221, 131, 194, 156, 126, 32, 163, 253, 31, 65,
    157, 195, 33, 127, 252, 162, 64, 30, 95, 1, 227, 189, 62, 96, 130, 220,
    35, 125, 159, 193, 66, 28, 254, 160, 225, 191, 93, 3, 128, 222, 60, 98,
    190, 224, 2, 92, 223, 129, 99, 61, 124, 34, 192, 158, 29, 67, 161, 255,
    70, 24, 250, 164, 39, 121, 155, 197, 132, 218, 56, 102, 229, 187, 89, 7,
    219, 133, 103, 57, 186, 228, 6, 88, 25, 71, 165, 251, 120, 38, 196, 154,
    101, 59, 217, 135, 4, 90, 184, 230, 167, 249, 27, 69, 198, 152, 122, 36,
    248, 166, 68, 26, 153, 199, 37, 123, 58, 100, 134, 216, 91, 5, 231, 185,
    140, 210, 48, 110, 237, 179, 81, 15, 78, 16, 242, 172, 47, 113, 147, 205,
    17, 79, 173, 243, 112, 46, 204, 146, 211, 141, 111, 49, 178, 236, 14, 80,
    175, 241, 19, 77, 206, 144, 114, 44, 109, 51, 209, 143, 12, 82, 176, 238,
    50, 108, 142, 208, 83, 13, 239, 177, 240, 174, 76, 18, 145, 207, 45, 115,
    202, 148, 118, 40, 171, 245, 23, 73, 8, 86, 180, 234, 105, 55, 213, 139,
    87, 9, 235, 181, 54, 104, 138, 212, 149, 203, 41, 119, 244, 170, 72, 22,
    233, 183, 85, 11, 136, 214, 52, 106, 43, 117, 151, 201, 74, 20, 246, 168,
    116, 42, 200, 150, 21, 75, 169, 247, 182, 232, 10, 84, 215, 137, 107, 53
};

static uint8_t camyu_test_bcc(const uint8_t *data, uint32_t len)
{
    uint8_t crc = 0;
    for (; len > 0; len--) {
        crc = CRC8Table[crc ^ *data];
        data++;
    }
    return crc;
}

/* ================================================================ */
/*  Test infrastructure                                              */
/* ================================================================ */

#define CAMYU_TEST_RX_BUF_SIZE 384
#define CAMYU_TEST_TX_BUF_SIZE 384

static uint8_t g_rx_buf[CAMYU_TEST_RX_BUF_SIZE];
static uint8_t g_tx_buf[CAMYU_TEST_TX_BUF_SIZE];

static camyu_protocol_parser_t *g_parser;

static bool    g_frame_ready_called;
static int     g_frame_ready_count;
static uint64_t g_frame_ready_parsed_id;
static uint32_t g_frame_ready_data_len;
static uint8_t  g_frame_ready_data[32];
static int      g_frame_ready_opcode;

static bool    g_tx_ready_called;
static int     g_tx_ready_count;
static uint32_t g_tx_ready_len;

static void on_frame_ready_cb(protocol_parser_t *parser, void *parsed, void *ctx)
{
    (void)ctx;
    camyu_private_t *pri = (camyu_private_t *)parsed;
    g_frame_ready_called = true;
    g_frame_ready_count++;
    g_frame_ready_parsed_id = pri->parsed_id;
    g_frame_ready_data_len  = pri->parsed_data_len;
    g_frame_ready_opcode    = pri->current_frame_info.opcode;

    if (pri->parsed_data && pri->parsed_data_len > 0
        && pri->parsed_data_len <= sizeof(g_frame_ready_data)) {
        memcpy(g_frame_ready_data, pri->parsed_data, pri->parsed_data_len);
    }

    if (pri->current_frame_info.opcode == PCTOCAMERA_READ) {
        static uint8_t resp[4] = { 0xAA, 0xBB, 0xCC, 0xDD };
        pri->parsed_data = resp;
        pri->parsed_data_len = sizeof(resp);
    }
}

static void on_tx_ready_cb(protocol_parser_t *parser, void *ctx)
{
    (void)ctx;
    g_tx_ready_called = true;
    g_tx_ready_count++;
    g_tx_ready_len = parser->tx.data_len;
}

static void camyu_setup(void)
{
    memset(g_rx_buf, 0, sizeof(g_rx_buf));
    memset(g_tx_buf, 0, sizeof(g_tx_buf));
    mock_time_ms = 0;

    g_frame_ready_called = false;
    g_frame_ready_count  = 0;
    g_tx_ready_called    = false;
    g_tx_ready_count     = 0;
    g_frame_ready_data_len = 0;
    memset(g_frame_ready_data, 0, sizeof(g_frame_ready_data));

    g_parser = camyu_protocol_create(g_rx_buf, sizeof(g_rx_buf),
                                     g_tx_buf, sizeof(g_tx_buf));
    TEST_ASSERT_NOT_NULL(g_parser);

    protocol_parser_set_callbacks((protocol_parser_t *)g_parser,
                                  on_frame_ready_cb, NULL,
                                  on_tx_ready_cb, NULL);
}

static void camyu_teardown(void)
{
    if (g_parser) {
        protocol_parser_destroy((protocol_parser_t *)g_parser);
        g_parser = NULL;
    }
}

/* ================================================================ */
/*  Category: Create / API validation                                */
/* ================================================================ */

void test_camyu_create_with_buffers_succeeds(void)
{
    TEST_ASSERT_NOT_NULL(g_parser);
    camyu_private_t *pri = &g_parser->pri;
    TEST_ASSERT_EQUAL(CAMYU_PARSER_STATE_WAIT_HEAD, pri->camyu_parser_state);
    TEST_ASSERT_FALSE(pri->transchar_active);
    TEST_ASSERT_EQUAL(0, pri->parsed_id);
    TEST_ASSERT_NULL(pri->parsed_data);
    TEST_ASSERT_EQUAL(0, pri->parsed_data_len);
    TEST_ASSERT_EQUAL(0, pri->stats.header_errors);
    TEST_ASSERT_EQUAL(0, pri->stats.checksum_errors);
}

void test_camyu_create_null_buffers_dynamic_alloc(void)
{
    camyu_protocol_parser_t *p = camyu_protocol_create(NULL, 0, NULL, 0);
    TEST_ASSERT_NOT_NULL(p);
    protocol_parser_destroy((protocol_parser_t *)p);
}

/* ================================================================ */
/*  Category: READ command                                           */
/* ================================================================ */

void test_camyu_read_full_frame_no_data(void)
{
    uint8_t ftf = 0x41;
    uint8_t frame[] = {
        0x7E, ftf, 0x00, 0x12, 0x34, 0x00
    };
    frame[5] = camyu_test_bcc(&frame[1], 4);

    parser_error_t err = protocol_parser_parse_data(
        (protocol_parser_t *)g_parser, frame, 6);

    TEST_ASSERT_EQUAL(PARSER_ERR_NONE, err);
    TEST_ASSERT(g_frame_ready_called);
    TEST_ASSERT_EQUAL(0x3412, (unsigned)g_frame_ready_parsed_id);
    TEST_ASSERT_EQUAL(PCTOCAMERA_READ, g_frame_ready_opcode);
    TEST_ASSERT_EQUAL(0, g_frame_ready_data_len);

    TEST_ASSERT(g_tx_ready_called);
}

void test_camyu_read_byte_by_byte(void)
{
    uint8_t ftf = 0x41;
    uint8_t frame[] = {
        0x7E, ftf, 0x00, 0xDE, 0xAD, 0x00
    };
    frame[5] = camyu_test_bcc(&frame[1], 4);

    for (int i = 0; i < 6; i++) {
        parser_error_t err = protocol_parser_parse_data(
            (protocol_parser_t *)g_parser, &frame[i], 1);
        if (i < 5) {
            TEST_ASSERT_EQUAL(PARSER_ERR_INCOMPLETE, err);
            TEST_ASSERT_FALSE(g_frame_ready_called);
        } else {
            TEST_ASSERT_EQUAL(PARSER_ERR_NONE, err);
            TEST_ASSERT(g_frame_ready_called);
            TEST_ASSERT_EQUAL(0xADDE, (unsigned)g_frame_ready_parsed_id);
        }
    }
}

void test_camyu_read_two_byte_chunk(void)
{
    uint8_t ftf = 0x41;
    uint8_t frame[] = {
        0x7E, ftf, 0x00, 0xAB, 0xCD, 0x00
    };
    frame[5] = camyu_test_bcc(&frame[1], 4);

    parser_error_t err = protocol_parser_parse_data(
        (protocol_parser_t *)g_parser, frame, 3);
    TEST_ASSERT_EQUAL(PARSER_ERR_INCOMPLETE, err);

    err = protocol_parser_parse_data(
        (protocol_parser_t *)g_parser, &frame[3], 3);
    TEST_ASSERT_EQUAL(PARSER_ERR_NONE, err);
    TEST_ASSERT_EQUAL(0xCDAB, (unsigned)g_frame_ready_parsed_id);
}

void test_camyu_read_incomplete_returns_incomplete(void)
{
    uint8_t frame[] = { 0x7E, 0x41, 0x00, 0x01 };
    parser_error_t err = protocol_parser_parse_data(
        (protocol_parser_t *)g_parser, frame, 4);
    TEST_ASSERT_EQUAL(PARSER_ERR_INCOMPLETE, err);
    TEST_ASSERT_FALSE(g_frame_ready_called);
}

void test_camyu_read_with_data(void)
{
    uint8_t ftf = 0x41;
    uint8_t frame[] = {
        0x7E, ftf, 0x04, 0x11, 0x22,
        0x01, 0x02, 0x03, 0x04, 0x00
    };
    frame[9] = camyu_test_bcc(&frame[1], 8);

    parser_error_t err = protocol_parser_parse_data(
        (protocol_parser_t *)g_parser, frame, 10);

    TEST_ASSERT_EQUAL(PARSER_ERR_NONE, err);
    TEST_ASSERT(g_frame_ready_called);
    TEST_ASSERT_EQUAL(0x2211, (unsigned)g_frame_ready_parsed_id);
    TEST_ASSERT_EQUAL(4, g_frame_ready_data_len);
    TEST_ASSERT_EQUAL(0x01, g_frame_ready_data[0]);
    TEST_ASSERT_EQUAL(0x02, g_frame_ready_data[1]);
    TEST_ASSERT_EQUAL(0x03, g_frame_ready_data[2]);
    TEST_ASSERT_EQUAL(0x04, g_frame_ready_data[3]);
}

/* ================================================================ */
/*  Category: WRITE command                                          */
/* ================================================================ */

void test_camyu_write_with_response_full_frame(void)
{
    uint8_t ftf = 0x01;
    uint8_t frame[] = {
        0x7E, ftf, 0x04, 0xAA, 0xBB,
        0x10, 0x20, 0x30, 0x40, 0x00
    };
    frame[9] = camyu_test_bcc(&frame[1], 8);

    parser_error_t err = protocol_parser_parse_data(
        (protocol_parser_t *)g_parser, frame, 10);

    TEST_ASSERT_EQUAL(PARSER_ERR_NONE, err);
    TEST_ASSERT(g_frame_ready_called);
    TEST_ASSERT_EQUAL(0xBBAA, (unsigned)g_frame_ready_parsed_id);
    TEST_ASSERT_EQUAL(PCTOCAMERA_WRITE_RESPONSE, g_frame_ready_opcode);
    TEST_ASSERT_EQUAL(4, g_frame_ready_data_len);
    TEST_ASSERT_EQUAL(0x10, g_frame_ready_data[0]);
    TEST_ASSERT_EQUAL(0x20, g_frame_ready_data[1]);
    TEST_ASSERT_EQUAL(0x30, g_frame_ready_data[2]);
    TEST_ASSERT_EQUAL(0x40, g_frame_ready_data[3]);

    TEST_ASSERT(g_tx_ready_called);
}

void test_camyu_write_norsponse_full_frame(void)
{
    uint8_t ftf = 0x21;
    uint8_t frame[] = {
        0x7E, ftf, 0x02, 0x54, 0x66,
        0xFE, 0xED, 0x00
    };
    frame[7] = camyu_test_bcc(&frame[1], 6);

    parser_error_t err = protocol_parser_parse_data(
        (protocol_parser_t *)g_parser, frame, 8);

    TEST_ASSERT_EQUAL(PARSER_ERR_NONE, err);
    TEST_ASSERT(g_frame_ready_called);
    TEST_ASSERT_EQUAL(0x6654, (unsigned)g_frame_ready_parsed_id);
    TEST_ASSERT_EQUAL(PCTOCAMERA_WRITE_NORESPONSE, g_frame_ready_opcode);
    TEST_ASSERT_EQUAL(2, g_frame_ready_data_len);
    TEST_ASSERT_EQUAL(0xFE, g_frame_ready_data[0]);
    TEST_ASSERT_EQUAL(0xED, g_frame_ready_data[1]);
}

void test_camyu_write_norsponse_incomplete(void)
{
    uint8_t ftf = 0x21;
    uint8_t frame[] = {
        0x7E, ftf, 0x04, 0x01, 0x02
    };
    parser_error_t err = protocol_parser_parse_data(
        (protocol_parser_t *)g_parser, frame, 5);
    TEST_ASSERT_EQUAL(PARSER_ERR_INCOMPLETE, err);
    TEST_ASSERT_FALSE(g_frame_ready_called);
}

/* ================================================================ */
/*  Category: Encode response verification                           */
/* ================================================================ */

void test_camyu_read_encode_response_format(void)
{
    uint8_t ftf = 0x41;
    uint8_t frame[] = {
        0x7E, ftf, 0x00, 0x12, 0x34, 0x00
    };
    frame[5] = camyu_test_bcc(&frame[1], 4);

    protocol_parser_parse_data(
        (protocol_parser_t *)g_parser, frame, 6);

    TEST_ASSERT(g_tx_ready_called);
    TEST_ASSERT(g_tx_ready_len > 0);

    TEST_ASSERT_EQUAL(0x7E, g_tx_buf[0]);

    uint8_t resp_ftf = g_tx_buf[1] & 0xE0;
    TEST_ASSERT_EQUAL((uint8_t)(CAMERATOPC_RESPONSE << 5), resp_ftf);

    uint8_t resp_len = g_tx_buf[2];
    TEST_ASSERT_EQUAL(4, resp_len);

    TEST_ASSERT_EQUAL(0x12, g_tx_buf[3]);
    TEST_ASSERT_EQUAL(0x34, g_tx_buf[4]);

    uint8_t status_byte = g_tx_buf[5];
    TEST_ASSERT_EQUAL(0x00, status_byte);

    TEST_ASSERT_EQUAL(0xAA, g_tx_buf[6]);
    TEST_ASSERT_EQUAL(0xBB, g_tx_buf[7]);
    TEST_ASSERT_EQUAL(0xCC, g_tx_buf[8]);
    TEST_ASSERT_EQUAL(0xDD, g_tx_buf[9]);

    uint8_t expected_bcc = camyu_test_bcc(&g_tx_buf[1], 9);
    TEST_ASSERT_EQUAL(expected_bcc, g_tx_buf[10]);

    TEST_ASSERT_EQUAL(11, g_tx_ready_len);
}

/* ================================================================ */
/*  Category: Byte escaping                                          */
/* ================================================================ */

void test_camyu_escape_head_in_data(void)
{
    uint8_t ftf = 0x01;
    uint8_t frame[] = {
        0x7E, ftf, 0x02, 0x01, 0x02,
        0x7D, 0x5E, 0x55, 0x00
    };
    uint8_t frame1[] = {
        0x7E, ftf, 0x02, 0x01, 0x02,
        0x7E, 0x55, 0x00
    };
    frame[8] = camyu_test_bcc(&frame1[1], 6);

    parser_error_t err = protocol_parser_parse_data(
        (protocol_parser_t *)g_parser, frame, 9);

    TEST_ASSERT_EQUAL(PARSER_ERR_NONE, err);
    TEST_ASSERT(g_frame_ready_called);
    TEST_ASSERT_EQUAL(2, g_frame_ready_data_len);

    TEST_ASSERT_EQUAL(0x7E, g_frame_ready_data[0]);
    TEST_ASSERT_EQUAL(0x55, g_frame_ready_data[1]);
}

void test_camyu_escape_transchar_in_data(void)
{
    uint8_t ftf = 0x01;
    uint8_t frame[] = {
        0x7E, ftf, 0x02, 0x01, 0x02,
        0x7D, 0x5D, 0x66, 0x00
    };
    uint8_t frame1[] = {
        0x7E, ftf, 0x02, 0x01, 0x02,
        0x7D, 0x66, 0x00
    };
    frame[8] = camyu_test_bcc(&frame1[1], 6);

    parser_error_t err = protocol_parser_parse_data(
        (protocol_parser_t *)g_parser, frame, 9);

    TEST_ASSERT_EQUAL(PARSER_ERR_NONE, err);
    TEST_ASSERT(g_frame_ready_called);
    TEST_ASSERT_EQUAL(2, g_frame_ready_data_len);

    TEST_ASSERT_EQUAL(0x7D, g_frame_ready_data[0]);
    TEST_ASSERT_EQUAL(0x66, g_frame_ready_data[1]);
}

void test_camyu_escape_invalid_second_byte(void)
{
    uint8_t frame[] = {
        0x7E, 0x01, 0x01, 0x01, 0x02,
        0x7D, 0xFF
    };
    parser_error_t err = protocol_parser_parse_data(
        (protocol_parser_t *)g_parser, frame, 7);

    TEST_ASSERT_EQUAL(PARSER_ERR_INVALID_PARAM, err);
    TEST_ASSERT_FALSE(g_frame_ready_called);
}

/* ================================================================ */
/*  Category: BCC verification                                       */
/* ================================================================ */

void test_camyu_bcc_invalid_returns_frame_error(void)
{
    uint8_t ftf = 0x41;
    uint8_t frame[] = {
        0x7E, ftf, 0x00, 0x12, 0x34, 0xFF
    };

    parser_error_t err = protocol_parser_parse_data(
        (protocol_parser_t *)g_parser, frame, 6);

    TEST_ASSERT_EQUAL(PARSER_ERR_FRAME, err);
    TEST_ASSERT_FALSE(g_frame_ready_called);

    camyu_private_t *pri = &g_parser->pri;
    TEST_ASSERT_EQUAL(1, pri->stats.checksum_errors);
}

/* ================================================================ */
/*  Category: Invalid header / FTF error                             */
/* ================================================================ */

void test_camyu_invalid_opcode_in_ftf(void)
{
    uint8_t ftf = 0xE1;
    uint8_t frame[] = {
        0x7E, ftf, 0x00, 0x12, 0x34, 0x00
    };
    parser_error_t err = protocol_parser_parse_data(
        (protocol_parser_t *)g_parser, frame, 6);

    TEST_ASSERT_EQUAL(PARSER_ERR_FRAME, err);
    TEST_ASSERT_FALSE(g_frame_ready_called);

    camyu_private_t *pri = &g_parser->pri;
    TEST_ASSERT_EQUAL(5, pri->stats.header_errors);
}

void test_camyu_invalid_opcode_recovery(void)
{
    uint8_t ftf = 0xE1;
    uint8_t frame[] = {
        0x7E, ftf, 0x00, 0x12, 0x34, 0x00
    };
    protocol_parser_parse_data(
        (protocol_parser_t *)g_parser, frame, 6);
    TEST_ASSERT_FALSE(g_frame_ready_called);

    uint8_t ftf2 = 0x41;
    uint8_t frame2[] = {
        0x7E, ftf2, 0x00, 0xDE, 0xAD, 0x00
    };
    frame2[5] = camyu_test_bcc(&frame2[1], 4);

    parser_error_t err = protocol_parser_parse_data(
        (protocol_parser_t *)g_parser, frame2, 6);
    TEST_ASSERT_EQUAL(PARSER_ERR_NONE, err);
    TEST_ASSERT(g_frame_ready_called);
    TEST_ASSERT_EQUAL(0xADDE, (unsigned)g_frame_ready_parsed_id);
}

/* ================================================================ */
/*  Category: Frame info — addrlen and version                       */
/* ================================================================ */

void test_camyu_addrlen_one(void)
{
    uint8_t ftf = 0x43;
    uint8_t frame[] = {
        0x7E, ftf, 0x00,
        0x11, 0x22, 0x33, 0x44,
        0x00
    };
    frame[7] = camyu_test_bcc(&frame[1], 6);

    parser_error_t err = protocol_parser_parse_data(
        (protocol_parser_t *)g_parser, frame, 8);

    TEST_ASSERT_EQUAL(PARSER_ERR_NONE, err);
    TEST_ASSERT(g_frame_ready_called);

    camyu_private_t *pri = &g_parser->pri;
    TEST_ASSERT_EQUAL(4, pri->current_frame_info.cmd_len_byte);
}

void test_camyu_addrlen_zero(void)
{
    uint8_t ftf = 0x41;
    uint8_t frame[] = {
        0x7E, ftf, 0x00,
        0xAA, 0xBB,
        0x00
    };
    frame[5] = camyu_test_bcc(&frame[1], 4);

    parser_error_t err = protocol_parser_parse_data(
        (protocol_parser_t *)g_parser, frame, 6);

    TEST_ASSERT_EQUAL(PARSER_ERR_NONE, err);
    camyu_private_t *pri = &g_parser->pri;
    TEST_ASSERT_EQUAL(2, pri->current_frame_info.cmd_len_byte);
}

/* ================================================================ */
/*  Category: Header resync (0x7E mid-frame)                         */
/* ================================================================ */

void test_camyu_header_mid_frame_resets_state(void)
{
    uint8_t ftf = 0x41;
    uint8_t frame[] = {
        0x7E, ftf, 0x00, 0x12,
        0x7E,
        0x41, 0x00, 0xDE, 0xAD, 0x00
    };
    frame[9] = camyu_test_bcc(&frame[5], 4);

    parser_error_t err = protocol_parser_parse_data(
        (protocol_parser_t *)g_parser, frame, 10);

    TEST_ASSERT_EQUAL(PARSER_ERR_NONE, err);
    TEST_ASSERT(g_frame_ready_called);

    TEST_ASSERT_EQUAL(0xADDE, (unsigned)g_frame_ready_parsed_id);
    TEST_ASSERT_EQUAL(1, g_frame_ready_count);
}

/* ================================================================ */
/*  Category: Null pointer / invalid param                          */
/* ================================================================ */

void test_camyu_parse_null_parser_returns_error(void)
{
    uint8_t data = 0x7E;
    parser_error_t err = protocol_parser_parse_data(NULL, &data, 1);
    TEST_ASSERT_EQUAL(PARSER_ERR_INVALID_PARAM, err);
}

void test_camyu_parse_null_data_returns_error(void)
{
    parser_error_t err = protocol_parser_parse_data(
        (protocol_parser_t *)g_parser, NULL, 1);
    TEST_ASSERT_EQUAL(PARSER_ERR_INVALID_PARAM, err);
}

void test_camyu_parse_zero_len_returns_error(void)
{
    uint8_t data = 0x7E;
    parser_error_t err = protocol_parser_parse_data(
        (protocol_parser_t *)g_parser, &data, 0);
    TEST_ASSERT_EQUAL(PARSER_ERR_INVALID_PARAM, err);
}

/* ================================================================ */
/*  Category: Reset / set-up repeatable                               */
/* ================================================================ */

void test_camyu_second_frame_after_reset(void)
{
    uint8_t ftf = 0x41;
    uint8_t frame1[] = {
        0x7E, ftf, 0x00, 0x12, 0x34, 0x00
    };
    frame1[5] = camyu_test_bcc(&frame1[1], 4);

    protocol_parser_parse_data((protocol_parser_t *)g_parser, frame1, 6);
    TEST_ASSERT(g_frame_ready_called);

    g_frame_ready_called = false;
    g_frame_ready_count  = 0;

    uint8_t frame2[] = {
        0x7E, ftf, 0x00, 0xAB, 0xCD, 0x00
    };
    frame2[5] = camyu_test_bcc(&frame2[1], 4);

    protocol_parser_parse_data((protocol_parser_t *)g_parser, frame2, 6);
    TEST_ASSERT(g_frame_ready_called);
    TEST_ASSERT_EQUAL(0xCDAB, (unsigned)g_frame_ready_parsed_id);
    TEST_ASSERT_EQUAL(1, g_frame_ready_count);
}

/* ================================================================ */
/*  Category: Write response generates tx (resp_en logic)            */
/* ================================================================ */

void test_camyu_write_response_generates_tx(void)
{
    uint8_t ftf = 0x01;
    uint8_t frame[] = {
        0x7E, ftf, 0x00, 0x01, 0x02, 0x00
    };
    frame[5] = camyu_test_bcc(&frame[1], 4);

    protocol_parser_parse_data((protocol_parser_t *)g_parser, frame, 6);
    TEST_ASSERT(g_tx_ready_called);
    TEST_ASSERT(g_tx_ready_len > 0);
}

/* ================================================================ */
/*  Runner                                                            */
/* ================================================================ */

int camyu_run_test_camyu(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_camyu_create_with_buffers_succeeds);
    RUN_TEST(test_camyu_create_null_buffers_dynamic_alloc);

    RUN_TEST(test_camyu_read_full_frame_no_data);
    RUN_TEST(test_camyu_read_byte_by_byte);
    RUN_TEST(test_camyu_read_two_byte_chunk);
    RUN_TEST(test_camyu_read_incomplete_returns_incomplete);
    RUN_TEST(test_camyu_read_with_data);

    RUN_TEST(test_camyu_write_with_response_full_frame);
    RUN_TEST(test_camyu_write_norsponse_full_frame);
    RUN_TEST(test_camyu_write_norsponse_incomplete);

    RUN_TEST(test_camyu_read_encode_response_format);

    RUN_TEST(test_camyu_escape_head_in_data);
    RUN_TEST(test_camyu_escape_transchar_in_data);
    RUN_TEST(test_camyu_escape_invalid_second_byte);

    RUN_TEST(test_camyu_bcc_invalid_returns_frame_error);

    RUN_TEST(test_camyu_invalid_opcode_in_ftf);
    RUN_TEST(test_camyu_invalid_opcode_recovery);

    RUN_TEST(test_camyu_addrlen_one);
    RUN_TEST(test_camyu_addrlen_zero);

    RUN_TEST(test_camyu_header_mid_frame_resets_state);

    RUN_TEST(test_camyu_parse_null_parser_returns_error);
    RUN_TEST(test_camyu_parse_null_data_returns_error);
    RUN_TEST(test_camyu_parse_zero_len_returns_error);

    RUN_TEST(test_camyu_second_frame_after_reset);
    RUN_TEST(test_camyu_write_response_generates_tx);

    return UNITY_END();
}

#include "test_suite.h"

TEST_SUITE_DEFINE(camyu, camyu_setup, camyu_teardown, camyu_run_test_camyu);
