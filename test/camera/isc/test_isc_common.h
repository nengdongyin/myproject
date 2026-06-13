/**
 * @file    test_isc_common.h
 * @brief   ISC 单元测试 — 公共夹具 (Mock Port + Mock Sensor)
 */

#ifndef TEST_ISC_COMMON_H
#define TEST_ISC_COMMON_H

#include "unity.h"
#include "isc.h"
#include "isc_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 *  Mock Port — 模拟 I2C 总线, 不依赖真实硬件
 * ═══════════════════════════════════════════════════════════════════════════ */

#define MOCK_REG_COUNT    64

/** @brief 模拟传感器寄存器文件 */
typedef struct {
    uint8_t  regs[MOCK_REG_COUNT];      /**< 寄存器值                           */
    uint32_t read_count;                /**< read() 被调用次数                   */
    uint32_t write_count;               /**< write() 被调用次数                  */
    int      inject_error;              /**< 注入 IO 错误 (0=正常, 非0=错误码)   */
    uint32_t last_reg;                  /**< 最近一次访问的寄存器地址             */
} mock_regfile_t;

extern mock_regfile_t g_mock_regs;

void mock_regfile_reset(void);
void mock_regfile_set_error(int err);

/** @brief 填充全局 Mock Port */
extern isc_port_t g_mock_port;

/* ═══════════════════════════════════════════════════════════════════════════
 *  Mock FPGA Ops — 记录调用, 不连接真实 FPGA
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    int      format_changed_count;      /**< ISC_FPGA_FORMAT_CHANGED 调用次数    */
    int      stream_state_count;        /**< ISC_FPGA_STREAM_STATE 调用次数      */
    uint8_t  last_stream_state;         /**< 最近一次 streaming 参数             */
    int      inject_error;              /**< 注入错误                            */
    isc_fmt_t last_fmt;                 /**< 最近一次通知的格式                  */
} mock_fpga_t;

extern mock_fpga_t g_mock_fpga;

void mock_fpga_reset(void);

/** @brief 填充全局 Mock FPGA Ops */
extern isc_fpga_ops_t g_mock_fpga_ops;

/* ═══════════════════════════════════════════════════════════════════════════
 *  Mock Sensor — 模拟传感器驱动 (纯软件, 不依赖 I2C)
 * ═══════════════════════════════════════════════════════════════════════════ */

/** @brief Mock 传感器私有数据 */
typedef struct {
    isc_fmt_t     current_fmt;          /**< 当前格式                           */
    isc_timing_t  current_timing;       /**< 当前时序                           */
    int           stream_state;         /**< 0=停, 1=流                         */
    int           probe_return;         /**< probe() 返回值                     */
    int           init_return;          /**< init() 返回值                      */

    /* 控制值存储 */
    int64_t       exposure;             /**< 曝光 ns                            */
    int64_t       analog_gain;          /**< 模拟增益码值                       */
    float         frame_rate;           /**< 帧率                               */
    int           hflip;                /**< 水平翻转                           */
    int           vflip;                /**< 垂直翻转                           */
    int           test_pattern;         /**< 测试图索引                         */

    /* probe 计数 */
    int           probe_called;
    int           init_called;
    int           deinit_called;
} mock_sensor_ctx_t;

extern mock_sensor_ctx_t g_mock_sensor;

void mock_sensor_reset(void);
void mock_sensor_set_probe_ret(int ret);
void mock_sensor_set_init_ret(int ret);

/** @brief Mock 传感器驱动 ops (声明的能力: TIMING + ROI + BINNING + 流中曝光) */
extern const isc_sensor_ops_t g_mock_sensor_ops;

/* ═══════════════════════════════════════════════════════════════════════════
 *  测试夹具 setup / teardown
 * ═══════════════════════════════════════════════════════════════════════════ */

void isc_setup(void);
void isc_teardown(void);

/* ═══════════════════════════════════════════════════════════════════════════
 *  辅助断言宏
 * ═══════════════════════════════════════════════════════════════════════════ */

#define TEST_ASSERT_ISC_OK(rc)    TEST_ASSERT_EQUAL_INT(ISC_OK, (rc))
#define TEST_ASSERT_ISC_ERR(rc, e) TEST_ASSERT_EQUAL_INT((e), (rc))

#ifdef __cplusplus
}
#endif

#endif /* TEST_ISC_COMMON_H */
