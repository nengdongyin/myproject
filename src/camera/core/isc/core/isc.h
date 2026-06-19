/**
 * @file    isc.h
 * @brief   ISC (Image Sensor Controller) — FPGA 软核图像传感器控制框架
 *
 * 对标 Linux V4L2 控制框架 + subdev 模型，实现控制/数据分离。
 * CPU 仅负责 I2C/SPI/AXI 传感器配置，LVDS 像素数据完全由 FPGA 逻辑处理。
 *
 * 公共头文件依赖 (扁平 include):
 *   isc.h 包含 isc_config.h, isc_port.h, isc_control.h, isc_sensor_ops.h
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
#define ISC_ERR_ALREADY_OPEN     -3    /**< @reserved 预留: 多开保护                  */
#define ISC_ERR_NOT_SUPPORTED    -4    /**< 传感器/驱动不支持该操作                  */
#define ISC_ERR_TIMEOUT          -5    /**< @reserved 预留: 通信超时                  */
#define ISC_ENUM_END             -6    /**< 枚举正常结束                            */
#define ISC_ERR_NO_MEM           -7    /**< 槽位已满                                 */
#define ISC_ERR_BUSY             -8    /**< 设备忙 (流中调不可改的控制项)             */
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

/**
 * @brief 传感器能力描述 — isc_query_cap() 返回的结构体
 * @details model/vendor 为指针，指向 isc_sensor_ops_t 中的常量字符串，零拷贝。
 *          capabilities 位掩码由 isc_open 时一次性探测并缓存于 isc_dev_t 中。
 */
typedef struct isc_cap {
    const char *model;                       /**< 传感器型号 (指向 ops->model)          */
    const char *vendor;                      /**< 厂商名 (指向 ops->vendor)             */
    uint32_t    capabilities;                /**< ISC_CAP_* 位掩码                     */
    uint8_t     num_formats;                 /**< 像素格式数量                         */
    uint8_t     num_ctrls;                   /**< 控制项数量                           */
    uint8_t     reserved[2];
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

    /* ── 输出分辨率范围 (缩减前, 调用者根据 reduction 自行折算) ── */
    uint16_t min_width;                      /**< 最小输出宽度 (无缩减)                  */
    uint16_t max_width;                      /**< 最大输出宽度 (无缩减)                  */
    uint16_t min_height;                     /**< 最小输出高度 (无缩减)                  */
    uint16_t max_height;                     /**< 最大输出高度 (无缩减)                  */

    uint32_t max_frame_rate_num;             /**< 该格式下最高帧率分子 (全分辨率)        */
    uint32_t max_frame_rate_den;             /**< 该格式下最高帧率分母                   */
} isc_fmt_desc_t;

/** @brief 缩减模式 */
typedef enum {
    ISC_REDUCE_NONE    = 0,  /**< 1×1 无缩减                               */
    ISC_REDUCE_BIN_SUM = 1,  /**< 模拟域电荷叠加 (SNR↑)                     */
    ISC_REDUCE_BIN_AVG = 2,  /**< 数字域平均合并 (噪声特性不同于 SUM)        */
    ISC_REDUCE_SKIP    = 3,  /**< 跳像素/抽点 (SNR 不变, 功耗↓)             */
} isc_reduce_mode_t;

/** @brief 当前格式 (含裁剪窗口) */
typedef struct isc_fmt {
    /* ── 输出格式 (缩减后) ── */
    uint32_t width;                          /**< 输出图像宽度 (pixels)                 */
    uint32_t height;                         /**< 输出图像高度 (lines)                  */
    uint32_t pixel_format;                   /**< 像素格式 FourCC                      */
    uint32_t frame_rate_num;                 /**< 帧率分子                              */
    uint32_t frame_rate_den;                 /**< 帧率分母                              */
    uint8_t  bit_depth;                      /**< 像素位深                              */
    uint8_t  reduction_x;                    /**< X 缩减因子 (1=无, 2=1/2, 4=1/4)       */
    uint8_t  reduction_y;                    /**< Y 缩减因子 (1=无, 2=1/2, 4=1/4)       */
    uint8_t  reduction_mode;                 /**< isc_reduce_mode_t                   */

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
    uint32_t          cid;                    /**< 控制 ID                              */
    isc_ctrl_type_t   type;                   /**< 控制类型                              */
    const char       *unit;                   /**< 单位 ("ns"/"×"/"dB"/"fps"/NULL)      */
    char              name[ISC_MAX_CTRL_NAME];/**< 可读名称                              */
    isc_ctrl_value_t  min;                    /**< 最小值                                */
    isc_ctrl_value_t  max;                    /**< 最大值                                */
    isc_ctrl_value_t  step;                   /**< 步进                                  */
    isc_ctrl_value_t  def;                    /**< 默认值                                */
    uint32_t          flags;                  /**< ISC_CTRL_FLAG_*                      */
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
 * @param[in] port          全局默认通信接口 (可为 NULL, 见说明)
 * @param[in] fpga_ops      FPGA 同步/控制接口 (必须非 NULL)
 * @param[in] sensors       传感器驱动 ops 表数组 (必须非 NULL)
 * @param[in] sensor_count  传感器驱动数量 (必须 >0)
 *
 * port 为全局回退默认值。若传感器在 ops->port 中自带总线接口,
 * 该传感器将忽略全局 port。若所有传感器均自带 port, 全局可为 NULL。
 *
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

/**
 * @brief 查询 ISC 框架是否已完成初始化
 * @return 1=已初始化, 0=未初始化或已 deinit
 */
int isc_is_initialized(void);

/**
 * @brief 注册控制值变更回调
 *
 * 每次 set_ctrl 成功后触发。同一设备仅支持一个回调;
 * 重复调用会覆盖旧回调。传 NULL 取消注册。
 *
 * @param[in] dev       设备句柄
 * @param[in] cb        回调函数 (NULL=取消)
 * @param[in] user_data 回调透传数据
 * @return ISC_OK / ISC_ERR_*
 */
int isc_register_ctrl_callback(isc_dev_t *dev, isc_on_ctrl_change_t cb,
                               void *user_data);

/**
 * @brief 注册传感器异常回调 (预留 — 需硬件中断+轮询线程配合)
 *
 * @param[in] dev       设备句柄
 * @param[in] cb        回调函数 (NULL=取消)
 * @param[in] user_data 回调透传数据
 * @return ISC_OK / ISC_ERR_*
 */
int isc_register_error_callback(isc_dev_t *dev, isc_on_error_t cb,
                                void *user_data);

/* ── 能力与格式 ── */
/**
 * @brief 查询传感器能力 (模型名、厂商、格式/控制项数量、ISC_CAP_* 位掩码)
 * @param[in]  dev  设备句柄
 * @param[out] cap  能力描述 (model/vendor 为指针，生命周期同 ops 表)
 * @return ISC_OK / ISC_ERR_*
 */
int isc_query_cap(isc_dev_t *dev, isc_cap_t *cap);

/**
 * @brief 枚举传感器支持的像素格式
 * @param[in]  dev   设备句柄
 * @param[in]  index 格式索引 (0-based, 递增至 ISC_ENUM_END)
 * @param[out] desc  格式描述 (含分辨率约束、裁剪步进、帧率上限)
 * @return ISC_OK / ISC_ENUM_END / ISC_ERR_*
 */
int isc_enum_fmt(isc_dev_t *dev, uint8_t index, isc_fmt_desc_t *desc);
/**
 * @brief 获取当前生效格式 (纯查询, 不写 dev 内部状态)
 *
 * 与 isc_set_fmt 配对 — get 读取当前格式, set 提交新格式。
 * current_fmt 缓存仅由 isc_set_fmt() / isc_open() 更新。
 */
int isc_get_fmt(isc_dev_t *dev, isc_fmt_t *fmt);
/**
 * @brief 提交新格式 — 写传感器寄存器 + 通知 FPGA (有副作用)
 * @param[in]     dev  设备句柄
 * @param[in,out] fmt  目标格式; crop=0 由驱动填充全阵列值
 * @return ISC_OK / ISC_ERR_*
 * @note 成功后 dev->current_fmt 被更新, FPGA 收到 ISC_FPGA_FORMAT_CHANGED
 */
int isc_set_fmt(isc_dev_t *dev, isc_fmt_t *fmt);

/**
 * @brief 试探格式合法性 — 仅校验约束, 不写硬件 (无副作用)
 * @param[in]     dev  设备句柄
 * @param[in,out] fmt  目标格式; 驱动会对其做对齐/钳位修正
 * @return ISC_OK / ISC_ERR_*
 * @note 与 isc_set_fmt 配对: 先 try 验约束, 再 set 提交
 */
int isc_try_fmt(isc_dev_t *dev, isc_fmt_t *fmt);

/* ── 控制框架 ── */
/**
 * @brief 查询指定 CID 的控制项属性 (类型/范围/步进/默认值/标志位)
 * @param[in]     dev  设备句柄
 * @param[in,out] desc 输入: desc->cid = 目标 CID; 输出: 完整属性描述
 * @return ISC_OK / ISC_ERR_*
 * @note 调用后会重置 isc_query_next_ctrl 的枚举游标
 */
int isc_query_ctrl(isc_dev_t *dev, isc_ctrl_desc_t *desc);

/**
 * @brief 枚举下一个有效控制项
 *
 * 每次调用返回当前枚举位置之后的下一个控制项描述符,
 * 并推进内部迭代器。枚举穷尽时返回 ISC_ENUM_END。
 * 直接调用 isc_query_ctrl() 会重置枚举迭代器。
 *
 * @param[in]  dev  设备句柄
 * @param[out] desc 下一控制项描述符
 * @return ISC_OK / ISC_ENUM_END / ISC_ERR_*
 */
int isc_query_next_ctrl(isc_dev_t *dev, isc_ctrl_desc_t *desc);

/**
 * @brief 查询 ENUM 型控制项的菜单名称
 * @param[in]  dev   设备句柄
 * @param[in]  cid   控制 ID (必须为 ENUM 类型)
 * @param[in]  index 菜单索引 (0..max-1, 超出范围返回 ISC_ERR_CTRL_RANGE)
 * @param[out] name  菜单名称缓冲区 (调用者提供, ≥ISC_MAX_MENU_NAME 字节)
 * @return ISC_OK / ISC_ERR_*
 */
int isc_query_menu(isc_dev_t *dev, uint32_t cid, uint32_t index, char *name);

/**
 * @brief 读取控制值 — 非 VOLATILE 项优先读缓存, 减少 I2C 通信
 * @param[in]  dev   设备句柄
 * @param[in]  cid   控制 ID
 * @param[out] value 当前值
 * @return ISC_OK / ISC_ERR_*
 */
int isc_get_ctrl(isc_dev_t *dev, uint32_t cid, isc_ctrl_value_t *value);

/**
 * @brief 设置控制值 — 钳位到 [min,max] + 步进对齐 + 写硬件 + 更新缓存
 * @param[in] dev   设备句柄
 * @param[in] cid   控制 ID
 * @param[in] value 目标值 (超出范围自动钳位)
 * @return ISC_OK / ISC_ERR_*
 * @note 成功后触发 isc_on_ctrl_change_t 回调 (如已注册)。
 *       回调在释放全局锁后执行, 回调内可安全调用 ISC API。
 */
int isc_set_ctrl(isc_dev_t *dev, uint32_t cid, isc_ctrl_value_t value);

/**
 * @brief 批量读取多个控制值
 * @param[in]     dev    设备句柄
 * @param[in,out] ctrls  输入: count + items[].cid; 输出: items[].value
 * @return ISC_OK / ISC_ERR_*; 失败时 ctrls->error_idx 指示第一个失败项索引
 */
int isc_get_ext_ctrls(isc_dev_t *dev, isc_ext_ctrls_t *ctrls);

/**
 * @brief 批量设置多个控制值
 * @param[in]     dev    设备句柄
 * @param[in,out] ctrls  输入: count + items[].cid + items[].value
 * @return ISC_OK / ISC_ERR_*; 失败时 ctrls->error_idx 指示第一个失败项索引, 成功时清零
 */
int isc_set_ext_ctrls(isc_dev_t *dev, isc_ext_ctrls_t *ctrls);

/* ── 流控制 ── */
/**
 * @brief 启动传感器数据输出 — 通知 FPGA 流状态, 设备转入 STREAMING 态
 * @param[in] dev  设备句柄 (状态必须为 ISC_STATE_OPEN)
 * @return ISC_OK / ISC_ERR_*
 */
int isc_stream_on(isc_dev_t *dev);

/**
 * @brief 停止传感器数据输出 — 通知 FPGA 流状态, 设备转回 OPEN 态
 * @param[in] dev  设备句柄 (状态必须为 ISC_STATE_STREAMING)
 * @return ISC_OK / ISC_ERR_*
 */
int isc_stream_off(isc_dev_t *dev);

/* ── 物理状态与约束 ── */
/**
 * @brief 查询传感器当前物理时序 — 读寄存器原始值 + ISC 核心计算派生 ns 值
 * @param[in]  dev    设备句柄
 * @param[out] timing 时序快照 (原始值 + 派生 ns: line_period/readout/exposure)
 * @return ISC_OK / ISC_ERR_*
 */
int isc_query_timing(isc_dev_t *dev, isc_timing_t *timing);

/**
 * @brief 试探指定格式下的预期物理时序（无副作用）
 *
 * 与 isc_try_fmt 配对——先 try_fmt 确认格式合法，再 try_timing
 * 获取预期时序，验证 FPGA 流水线/输出带宽约束，最后一次性 set_fmt 提交。
 *
 * @param[in]  dev     设备句柄
 * @param[in]  fmt     试探格式（通常来自 isc_try_fmt 的返回值）
 * @param[out] timing  预期物理时序（ISC 核心计算派生 ns 值）
 * @return ISC_OK / ISC_ERR_*
 */
int isc_try_timing(isc_dev_t *dev, const isc_fmt_t *fmt, isc_timing_t *timing);

/**
 * @brief 查询传感器约束
 *
 * @param[in]  dev             设备句柄
 * @param[in]  type            约束类型 ID
 * @param[in]  index           同类型约束索引 (0-based)
 * @param[out] constraint_data 约束数据缓冲区
 * @param[in]  data_size       缓冲区字节数 (用于边界校验)
 * @return ISC_OK / ISC_ERR_*
 */
int isc_query_constraint(isc_dev_t *dev, isc_constraint_type_t type,
                         uint32_t index, void *constraint_data,
                         uint32_t data_size);

/* ── 传感器扩展 ── */
/**
 * @brief 传感器专属操作 (最后手段 — 仅用于无法纳入标准 CID 的私有功能)
 *
 * @param[in]     dev  设备句柄
 * @param[in]     cmd  命令码 (FourCC 风格, 厂商定义)
 * @param[in,out] arg  命令参数 (类型/大小由 cmd 决定, 无框架校验)
 * @return ISC_OK / ISC_ERR_*
 */
int isc_sensor_ioctl(isc_dev_t *dev, uint32_t cmd, void *arg);

#endif /* ISC_H */
