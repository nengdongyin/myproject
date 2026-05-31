/**
 * @file    isc.h
 * @brief   ISC (Image Sensor Controller) — FPGA 软核图像传感器控制框架
 *
 * 对标 Linux V4L2 控制框架 + subdev 模型，实现控制/数据分离。
 * CPU 仅负责 I2C/SPI/AXI 传感器配置，LVDS 像素数据完全由 FPGA 逻辑处理。
 *
 * 依赖方向 (#include 自顶向下):
 *   isc.h → isc_control.h / isc_port.h → isc_sensor_ops.h → isc_config.h
 */

#ifndef ISC_H
#define ISC_H

#include <stdint.h>
#include <stddef.h>
#include "isc_config.h"
#include "isc_port.h"
#include "isc_control.h"
#include "isc_sensor_ops.h"

/* ──── 不透明上下文 ──── */
typedef struct isc_dev_t isc_dev_t;

/* ──── 能力位图 ──── */
#define ISC_CAP_TIMING_QUERY      (1u << 0)  /**< 支持 isc_query_timing()              */
#define ISC_CAP_CONSTRAINT_QUERY  (1u << 1)  /**< 支持 isc_query_constraint()          */
#define ISC_CAP_TRIGGER_CONTROL   (1u << 2)  /**< 支持 fpga_ops.ioctl()                */
#define ISC_CAP_ROI               (1u << 3)  /**< 支持 ROI/裁剪 (isc_fmt_t.crop_*)     */
#define ISC_CAP_BINNING           (1u << 4)  /**< 支持 binning 模式                     */
#define ISC_CAP_SUBSAMPLE         (1u << 5)  /**< 支持抽点/skip 模式                    */
#define ISC_CAP_ROI_WITH_BINNING  (1u << 6)  /**< ROI 与 binning 可同时使能              */

/* ──── 错误码 ──── */
#define ISC_OK                    0    /**< 操作成功                                */
#define ISC_ERR_INVALID_ARG      -1    /**< 参数非法 (NULL/越界/crop 不合法)         */
#define ISC_ERR_NOT_OPEN         -2    /**< 设备未 open                             */
#define ISC_ERR_ALREADY_OPEN     -3    /**< 设备已 open (多开保护)                   */
#define ISC_ERR_NOT_SUPPORTED    -4    /**< 传感器/驱动不支持该操作                  */
#define ISC_ERR_TIMEOUT          -5    /**< 通信超时                                */
#define ISC_ERR_NO_MORE          -6    /**< 枚举结束 (非致命)                         */
#define ISC_ERR_NO_MEM           -7    /**< 内存不足 (S0 策略下保留)                 */
#define ISC_ERR_BUSY             -8    /**< 设备忙 (流中调 set_fmt 等)               */
#define ISC_ERR_NOT_FOUND        -9    /**< 未匹配到传感器                           */
#define ISC_ERR_IO               -10   /**< 底层通信失败                             */
#define ISC_ERR_CTRL_RANGE       -11   /**< 控制值超出 [min, max]                    */
#define ISC_ERR_STATE            -12   /**< 状态机不允许该操作                        */

/* ──── 像素格式 FourCC ──── */
#define ISC_PIX_FMT_SRGGB8    0x32424752u  /* 'RGGB' Bayer 8-bit  */
#define ISC_PIX_FMT_SBGGR8    0x31474242u  /* 'BGGR' Bayer 8-bit  */
#define ISC_PIX_FMT_SGRBG8    0x38425247u  /* 'GRBG' Bayer 8-bit  */
#define ISC_PIX_FMT_SGBRG8    0x34524247u  /* 'GBRG' Bayer 8-bit  */
#define ISC_PIX_FMT_SRGGB10   0x30314752u  /* 'RG10' Bayer 10-bit */
#define ISC_PIX_FMT_SBGGR10   0x30314742u  /* 'BG10' Bayer 10-bit */
#define ISC_PIX_FMT_SGRBG10   0x30315247u  /* 'GR10' Bayer 10-bit */
#define ISC_PIX_FMT_SGBRG10   0x30314747u  /* 'GB10' Bayer 10-bit */
#define ISC_PIX_FMT_SRGGB12   0x32314752u  /* 'RG12' Bayer 12-bit */
#define ISC_PIX_FMT_SBGGR12   0x32314742u  /* 'BG12' Bayer 12-bit */
#define ISC_PIX_FMT_GREY8     0x59455247u  /* 'GREY' Mono  8-bit  */
#define ISC_PIX_FMT_GREY10    0x30315947u  /* 'GY10' Mono 10-bit  */

/* ──── 公共类型 ──── */

/** @brief 传感器能力描述 */
typedef struct isc_cap {
    char     model[ISC_MAX_MODEL_NAME];      /**< 传感器型号                           */
    char     vendor[ISC_MAX_VENDOR_NAME];    /**< 厂商名                               */
    uint32_t capabilities;                   /**< ISC_CAP_* 位掩码                     */
    uint8_t  num_formats;                    /**< 像素格式数量                         */
    uint8_t  num_ctrls;                      /**< 控制项数量                           */
    uint8_t  bus_type;                       /**< 总线类型 (isc_bus_type_t)             */
    uint8_t  reserved[3];
} isc_cap_t;

/** @brief 格式描述 (枚举用，per-format 约束) */
typedef struct isc_fmt_desc {
    uint32_t pixel_format;                   /**< 像素格式 FourCC                      */
    char     description[ISC_MAX_FMT_DESC];  /**< 可读描述 "Bayer RGGB 10-bit"          */
    uint8_t  bit_depth;                      /**< 像素位深 (8/10/12/14/16)              */

    /* ── 传感器像素阵列 ── */
    uint16_t sensor_width;                   /**< 传感器满幅宽度 (pixels)               */
    uint16_t sensor_height;                  /**< 传感器满幅高度 (lines)                */

    /* ── 裁剪约束 ── */
    uint16_t crop_step_x;                    /**< 水平裁剪步进 (列对齐粒度)             */
    uint16_t crop_step_y;                    /**< 垂直裁剪步进 (行对齐粒度)             */
    uint16_t min_crop_width;                 /**< 最小裁剪窗口宽度                      */
    uint16_t min_crop_height;                /**< 最小裁剪窗口高度                      */

    /* ── 输出分辨率范围 (1×1 无缩减基准) ──
     * 实际: output = crop / reduction_factor, 本描述符固定 1×1.
     */
    uint16_t min_width;                      /**< 最小输出宽度 (无缩减)                  */
    uint16_t max_width;                      /**< 最大输出宽度 (无缩减, 等于 sensor_width) */
    uint16_t min_height;                     /**< 最小输出高度 (无缩减)                  */
    uint16_t max_height;                     /**< 最大输出高度 = sensor_height           */

    uint32_t max_frame_rate_num;             /**< 该格式下最高帧率分子 (全分辨率)        */
    uint32_t max_frame_rate_den;             /**< 该格式下最高帧率分母                   */
} isc_fmt_desc_t;

/** @brief 缩减方式 */
typedef enum {
    ISC_REDUCTION_NONE   = 0,   /**< 1×1 无缩减 (默认, 兼容零初始化)                 */
    ISC_REDUCTION_BIN_2  = 1,   /**< 2×2 binning (电荷合并, 提升 SNR)                */
    ISC_REDUCTION_BIN_4  = 2,   /**< 4×4 binning                                    */
    ISC_REDUCTION_SKIP_2 = 3,   /**< 2×2 subsampling (跳像素, 无电荷叠加)             */
    ISC_REDUCTION_SKIP_4 = 4,   /**< 4×4 subsampling                                */
} isc_reduction_t;

/** @brief 当前格式 (含裁剪窗口) */
typedef struct isc_fmt {
    /* ── 输出格式 (缩减后) ── */
    uint32_t width;                          /**< 输出图像宽度 (pixels)                 */
    uint32_t height;                         /**< 输出图像高度 (lines)                  */
    uint32_t pixel_format;                   /**< 像素格式 FourCC                      */
    uint32_t frame_rate_num;                 /**< 帧率分子                              */
    uint32_t frame_rate_den;                 /**< 帧率分母                              */
    uint8_t  bit_depth;                      /**< 像素位深                              */
    isc_reduction_t reduction;               /**< 缩减方式                              */
    uint8_t  reserved[3];

    /* ── 裁剪窗口 (传感器像素阵列坐标系, 缩减前) ── */
    /**
     * crop_width=0 && crop_height=0 → 全传感器阵列 (兼容零初始化).
     * isc_get_fmt() 始终返回实际 crop 坐标 (永不为 0×0).
     * output = crop / reduction_factor.
     */
    uint32_t crop_left;                      /**< 裁剪窗口水平起始列 (0-based)           */
    uint32_t crop_top;                       /**< 裁剪窗口垂直起始行 (0-based)           */
    uint32_t crop_width;                     /**< 裁剪窗口宽度 (0=全阵列)                */
    uint32_t crop_height;                    /**< 裁剪窗口高度 (0=全阵列)                */
} isc_fmt_t;

/** @brief 物理时序快照 */
typedef struct isc_timing {
    /* ── 传感器寄存器原始值 ── */
    uint32_t pixel_clock_hz;                 /**< 当前像素时钟 (Hz)                     */
    uint32_t line_length_pclk;               /**< 行长度 (pixel clocks, 含消隐)          */
    uint32_t frame_length_lines;             /**< 帧长度 (lines, 含垂直消隐)             */
    uint8_t  lane_count;                     /**< MIPI lane 数                          */
    uint8_t  bit_depth;                      /**< 输出位深                              */
    uint8_t  reserved[2];

    /* ── 曝光/读出 (lines — 与寄存器同单位) ── */
    uint32_t exposure_lines;                 /**< 当前实际曝光时间 (lines)               */
    uint32_t exposure_max_lines;             /**< 当前配置下最大曝光时间 (lines)          */
    uint32_t readout_lines;                  /**< 读出跨度 (lines): 首行始→末行止         */

    /* ── 派生物理时间 (ns — ISC 核心计算) ── */
    uint32_t line_period_ns;                 /**< 行周期: 1e9*line_length/pixel_clock    */
    uint32_t frame_period_ns;                /**< 帧周期: line_period * frame_length     */
    uint32_t readout_time_ns;                /**< 读出时间: line_period * readout_lines  */
    uint32_t exposure_time_ns;               /**< 曝光时间: line_period * exposure_lines */
    uint32_t exposure_max_ns;                /**< 最大曝光: line_period*exposure_max_lines*/
} isc_timing_t;

/** @brief 控制值联合体 */
typedef union isc_ctrl_value {
    int64_t  i64;          /**< INTEGER / ENUM                                    */
    uint8_t  b;            /**< BOOLEAN                                           */
    float    f;            /**< FLOAT                                             */
} isc_ctrl_value_t;

/** @brief 控制项描述符 */
typedef struct isc_ctrl_desc {
    uint32_t          cid;                    /**< 控制 ID (含 ISC_CTRL_FLAG_NEXT_CTRL) */
    isc_ctrl_type_t   type;                   /**< 控制类型                              */
    const char       *unit;                   /**< 单位 ("ns"/"×"/"dB"/"fps"/NULL)      */
    char              name[ISC_MAX_CTRL_NAME];/**< 可读名称                              */
    isc_ctrl_value_t  min;                    /**< 最小值                                */
    isc_ctrl_value_t  max;                    /**< 最大值                                */
    isc_ctrl_value_t  step;                   /**< 步进                                  */
    isc_ctrl_value_t  def;                    /**< 默认值                                */
    uint32_t          flags;                  /**< ISC_CTRL_FLAG_*                      */
    uint32_t          ctrl_class;             /**< 控制类                                */
} isc_ctrl_desc_t;

/** @brief 批量控制 */
typedef struct isc_ext_ctrls {
    uint32_t count;                           /**< 控制项数量                            */
    uint32_t error_idx;                       /**< 失败时指示出错位置                     */
    struct {
        uint32_t          cid;                /**< 控制 ID                              */
        isc_ctrl_value_t  value;              /**< 值                                   */
    } items[ISC_MAX_EXT_CTRLS];
} isc_ext_ctrls_t;

/** @brief 约束类型 (仅含厂商私有基值) */
typedef enum {
    ISC_CONSTRAINT_PRIVATE_BASE = 0,          /**< 厂商私有约束起点 (0–255)               */
} isc_constraint_type_t;

/* ──── 回调类型 ──── */

/** @brief 控制值变更回调 */
typedef void (*isc_on_ctrl_change_t)(isc_dev_t *dev, uint32_t cid,
                                     isc_ctrl_value_t new_val, void *user_data);

/** @brief 传感器异常回调 */
typedef void (*isc_on_error_t)(isc_dev_t *dev, int error_code, void *user_data);

/* ──── 公共 API ──── */

/**
 * @brief 初始化 ISC 框架
 *
 * @param[in] port          平台传感器通信原语 (必须非 NULL)
 * @param[in] fpga_ops      FPGA 同步/控制接口 (必须非 NULL)
 * @param[in] sensors       传感器驱动 ops 表数组 (必须非 NULL)
 * @param[in] sensor_count  传感器驱动数量 (必须 >0)
 * @return ISC_OK / ISC_ERR_INVALID_ARG / ISC_ERR_NO_MEM
 */
int isc_init(const isc_port_t *port,
             const isc_fpga_ops_t *fpga_ops,
             const isc_sensor_ops_t *const sensors[],
             uint8_t sensor_count);

/** @brief 反初始化 ISC 框架 (幂等) */
int isc_deinit(void);

/**
 * @brief 打开传感器设备
 *
 * @param[in]  model  型号名 ("sony_imx477"), NULL=自动探测
 * @param[out] dev    设备句柄
 * @return ISC_OK / ISC_ERR_NOT_FOUND / ISC_ERR_*
 */
int isc_open(const char *model, isc_dev_t **dev);

/** @brief 关闭传感器设备 (幂等) */
int isc_close(isc_dev_t *dev);

/* ── 能力与格式 ── */
int isc_query_cap(isc_dev_t *dev, isc_cap_t *cap);
int isc_enum_fmt(isc_dev_t *dev, uint8_t index, isc_fmt_desc_t *desc);
int isc_get_fmt(isc_dev_t *dev, isc_fmt_t *fmt);
int isc_set_fmt(isc_dev_t *dev, isc_fmt_t *fmt);
int isc_try_fmt(isc_dev_t *dev, isc_fmt_t *fmt);

/* ── 控制框架 ── */
int isc_query_ctrl(isc_dev_t *dev, isc_ctrl_desc_t *desc);
int isc_query_menu(isc_dev_t *dev, uint32_t cid, uint32_t index, char *name);
int isc_get_ctrl(isc_dev_t *dev, uint32_t cid, isc_ctrl_value_t *value);
int isc_set_ctrl(isc_dev_t *dev, uint32_t cid, isc_ctrl_value_t value);
int isc_get_ext_ctrls(isc_dev_t *dev, isc_ext_ctrls_t *ctrls);
int isc_set_ext_ctrls(isc_dev_t *dev, isc_ext_ctrls_t *ctrls);

/* ── 流控制 ── */
int isc_stream_on(isc_dev_t *dev);
int isc_stream_off(isc_dev_t *dev);

/* ── 物理状态与约束 ── */
int isc_query_timing(isc_dev_t *dev, isc_timing_t *timing);
int isc_query_constraint(isc_dev_t *dev, isc_constraint_type_t type,
                         uint32_t index, void *constraint_data);

/* ── 传感器扩展 ── */
int isc_sensor_ioctl(isc_dev_t *dev, uint32_t cmd, void *arg);

#endif /* ISC_H */
