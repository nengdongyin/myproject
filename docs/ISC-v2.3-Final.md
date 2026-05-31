# ISC — Image Sensor Controller 设计文档 v2.3 Final

**版本**：2.3.0
**日期**：2025-07-19
**平台**：FPGA 软核（≤150MHz / 64KB BRAM / SPI Flash≥2MB）
**数据路径**：LVDS → FPGA 逻辑（CPU 零参与像素数据）
**对标**：Linux V4L2 控制框架 + subdev 模型 + Selection API
**内存策略**：S0 — 全静态分配，禁止 `malloc`

---

## 1. 模块概述

**功能**：为 FPGA 软核提供纯控制面的统一图像传感器操作接口。CPU 通过 I2C/SPI/AXI 配置传感器（上电 / 格式 / ROI / binning / 曝光 / 增益 / 黑电平），并通过回调将传感器状态同步给 FPGA 逻辑。高速 LVDS 像素数据完全由 FPGA 逻辑处理，CPU 不接触任何像素数据。

**设计精髓**（取自 V4L2 三项核心遗产）：

1. **TRY_FMT 格式协商** — 试探不扰动硬件和 FPGA，多次 TRY 后一次 S_FMT 提交
2. **CID 控制框架** — 标准控制 + 厂商扩展统一 API，NEXT_CTRL 枚举驱动 UI 自适应
3. **传感器驱动解耦** — 新增传感器型号只需实现一个 ops 表，核心零改动

**版本演进**：

| 版本 | 关键变更 |
|------|---------|
| v1.0 | 初始 V4L2 风格框架（含缓冲管理，通用 MCU 向） |
| v2.0 | FPGA 软核适配：移除全部数据路径、新增 FPGA 同步 |
| v2.1 | 传输解耦 + 物理时序查询 + 传感器约束查询 + 触发控制 + 9 个新 CID |
| v2.2 | ROI/裁剪支持（isc_fmt_t crop 字段）+ 能力位图 + 校准透明化原则 |
| **v2.3** | **isc_init 参数明确 + isc_open(NULL) 自动探测 + V4L2 设计对照** |

---

## 2. 分层架构

```
┌──────────────────────────────────────────────────────┐
│  FPGA 逻辑 (HDL)                                      │
│  · LVDS 接收 → 解码 → ISP → 输出                      │
│  · 配置寄存器组 (AXI-Lite)：格式 / 流状态 / 触发      │
├──────────────────────────────────────────────────────┤
│  ★ ISC Core — 软核 CPU                               │
│  · 传感器注册表 + 工厂匹配（含自动探测）                 │
│  · 格式协商引擎 (TRY_FMT / S_FMT → FPGA 同步)        │
│  · CID 控制框架 (分发 + clamp/step + 缓存)           │
│  · 物理时序查询 + 传感器约束查询                      │
│  · 状态机 (INIT → OPEN → STREAMING)                  │
├──────────────────────────────────────────────────────┤
│  传感器驱动 (isc_sensor_xxx.c)                     │
│  · CHIP_ID 检测 + 上电时序 + 寄存器初始化             │
│  · 格式/ROI/crop → 寄存器位域映射                     │
│  · CID → 寄存器地址 + 位掩码                          │
│  · 传感器特定矫正（对应用透明）                        │
│  · 物理时序采集 + 约束暴露                            │
├──────────────────────────────────────────────────────┤
│  ISC Port / FPGA Ops (用户实现)                       │
│  · 通用传感器寄存器读写 (I2C / SPI / AXI)            │
│  · GPIO (PWDN / RESET / XCLR) / 延时                 │
│  · FPGA 同步回调 (格式变更 / 流状态 / 触发控制)       │
├──────────────────────────────────────────────────────┤
│  BSP：I2C/SPI/GPIO 控制器 IP + AXI 总线               │
└──────────────────────────────────────────────────────┘
```

**依赖方向**（`#include` 自顶向下，禁止反向）：
`isc.h` → `isc_control.h` / `isc_port.h` → `isc_sensor_ops.h` → `isc_config.h` → `isc_internal.h`(仅 .c)

**不依赖**：任何 RTOS、任何外设驱动库、任何动态内存、任何同层模块。

---

## 3. 公共接口（`isc.h`）

### 3.1 不透明上下文

```c
typedef struct isc_dev_t isc_dev_t;
```

### 3.2 能力位图

```c
#define ISC_CAP_TIMING_QUERY      (1u << 0)  /**< 支持 isc_query_timing()              */
#define ISC_CAP_CONSTRAINT_QUERY  (1u << 1)  /**< 支持 isc_query_constraint()          */
#define ISC_CAP_TRIGGER_CONTROL   (1u << 2)  /**< 支持 fpga_ops.ioctl()                */
#define ISC_CAP_ROI               (1u << 3)  /**< 支持 ROI/裁剪 (isc_fmt_t.crop_*)     */
#define ISC_CAP_BINNING           (1u << 4)  /**< 支持 binning 模式                     */
#define ISC_CAP_SUBSAMPLE         (1u << 5)  /**< 支持抽点/skip 模式                    */
#define ISC_CAP_ROI_WITH_BINNING  (1u << 6)  /**< ROI 与 binning 可同时使能              */
```

### 3.3 公共类型

```c
/* ──── 能力描述 ──── */
typedef struct isc_cap {
    char     model[ISC_MAX_MODEL_NAME];      /**< 传感器型号 */
    char     vendor[ISC_MAX_VENDOR_NAME];    /**< 厂商名 */
    uint32_t capabilities;                   /**< ISC_CAP_* 位掩码 */
    uint8_t  num_formats;                    /**< 像素格式数量 */
    uint8_t  num_ctrls;                      /**< 控制项数量 */
    uint8_t  bus_type;                       /**< 总线类型 (isc_bus_type_t) */
    uint8_t  reserved[3];
} isc_cap_t;

/* ──── 像素格式 FourCC ──── */
#define ISC_PIX_FMT_SRGGB8    0x32424752  /* 'RGGB' 8-bit  */
#define ISC_PIX_FMT_SBGGR8    0x31474242  /* 'BGGR' 8-bit  */
#define ISC_PIX_FMT_SGRBG8    0x38425247  /* 'GRBG' 8-bit  */
#define ISC_PIX_FMT_SGBRG8    0x34524247  /* 'GBRG' 8-bit  */
#define ISC_PIX_FMT_SRGGB10   0x30314752  /* 'RG10' 10-bit */
#define ISC_PIX_FMT_SBGGR10   0x30314742  /* 'BG10' 10-bit */
#define ISC_PIX_FMT_SGRBG10   0x30315247  /* 'GR10' 10-bit */
#define ISC_PIX_FMT_SGBRG10   0x30314747  /* 'GB10' 10-bit */
#define ISC_PIX_FMT_SRGGB12   0x32314752  /* 'RG12' 12-bit */
#define ISC_PIX_FMT_SBGGR12   0x32314742  /* 'BG12' 12-bit */
#define ISC_PIX_FMT_GREY8     0x59455247  /* 'GREY' 8-bit  */
#define ISC_PIX_FMT_GREY10    0x30315947  /* 'GY10' 10-bit */

/* ──── 格式描述（枚举用） ──── */
typedef struct isc_fmt_desc {
    uint32_t pixel_format;                   /**< 像素格式 FourCC */
    char     description[ISC_MAX_FMT_DESC];  /**< "Bayer RGGB 10-bit" */
    uint8_t  bit_depth;                      /**< 像素位深 (8/10/12/14/16) */

    /* ── 传感器像素阵列 ── */
    uint16_t sensor_width;                   /**< 传感器满幅宽度 (pixels) */
    uint16_t sensor_height;                  /**< 传感器满幅高度 (lines) */

    /* ── 裁剪约束 ── */
    uint16_t crop_step_x;                    /**< 水平裁剪步进 (列对齐粒度) */
    uint16_t crop_step_y;                    /**< 垂直裁剪步进 (行对齐粒度) */
    uint16_t min_crop_width;                 /**< 最小裁剪窗口宽度 */
    uint16_t min_crop_height;                /**< 最小裁剪窗口高度 */

    /**
     * ── 输出分辨率范围 ──
     *
     * 描述该格式在 1×1（无缩减）下的输出分辨率边界。
     * 这些值对应 isc_fmt_t.reduction=ISC_REDUCTION_NONE 时的基准。
     * 缩减方式由 isc_fmt_t.reduction 显式指定，不在本描述符中。
     *
     * 实际输出分辨率取决于裁剪窗口和缩减因子：
     *   output_width  = crop_width  / reduction_factor
     *   output_height = crop_height / reduction_factor
     *
     * 因此 max_width = sensor_width（无缩减时），
     * min_width  = min_crop_width（无缩减时）。
     */
    uint16_t min_width;                      /**< 最小输出宽度 (无缩减) */
    uint16_t max_width;                      /**< 最大输出宽度 = sensor_width */
    uint16_t min_height;                     /**< 最小输出高度 (无缩减) */
    uint16_t max_height;                     /**< 最大输出高度 = sensor_height */

    uint32_t max_frame_rate_num;             /**< 该格式下最高帧率分子 */
    uint32_t max_frame_rate_den;             /**< 该格式下最高帧率分母 */
} isc_fmt_desc_t;

/* ──── 缩减方式 ──── */
typedef enum {
    ISC_REDUCTION_NONE   = 0,  /**< 1×1 无缩减（默认，兼容零初始化）         */
    ISC_REDUCTION_BIN_2  = 1,  /**< 2×2 binning（电荷合并，提升 SNR）       */
    ISC_REDUCTION_BIN_4  = 2,  /**< 4×4 binning                             */
    ISC_REDUCTION_SKIP_2 = 3,  /**< 2×2 subsampling（跳像素，无电荷叠加）    */
    ISC_REDUCTION_SKIP_4 = 4,  /**< 4×4 subsampling                         */
} isc_reduction_t;

/* ──── 当前格式（含裁剪窗口） ──── */
typedef struct isc_fmt {
    /* ── 输出格式 (缩减后的最终帧属性) ── */
    uint32_t width;                          /**< 输出图像宽度 (pixels) */
    uint32_t height;                         /**< 输出图像高度 (lines) */
    uint32_t pixel_format;                   /**< 像素格式 FourCC */
    uint32_t frame_rate_num;                 /**< 帧率分子 */
    uint32_t frame_rate_den;                 /**< 帧率分母 */
    uint8_t  bit_depth;                      /**< 像素位深 */
    isc_reduction_t reduction;               /**< 缩减方式 */
    uint8_t  reserved[3];

    /* ── 裁剪窗口 (传感器像素阵列坐标系，缩减前) ── */
    /**
     * 约定：crop_width=0 且 crop_height=0 → 使用全传感器阵列（兼容零初始化）。
     * isc_get_fmt() 始终返回实际 crop 坐标（永不为 0×0）。
     * 输出与裁剪关系: width  = crop_width  / reduction_factor
     *                 height = crop_height / reduction_factor
     */
    uint32_t crop_left;                      /**< 裁剪窗口水平起始列 (0-based) */
    uint32_t crop_top;                       /**< 裁剪窗口垂直起始行 (0-based) */
    uint32_t crop_width;                     /**< 裁剪窗口宽度 (0=全阵列) */
    uint32_t crop_height;                    /**< 裁剪窗口高度 (0=全阵列) */
} isc_fmt_t;

/* ──── 物理时序快照 ──── */
typedef struct isc_timing {
    /* ── 传感器寄存器原始值 ── */
    uint32_t pixel_clock_hz;                 /**< 当前像素时钟 (Hz) */
    uint32_t line_length_pclk;               /**< 行长度 (pixel clocks, 含消隐) */
    uint32_t frame_length_lines;             /**< 帧长度 (lines, 含垂直消隐) */
    uint8_t  lane_count;                     /**< MIPI lane 数 */
    uint8_t  bit_depth;                      /**< 输出位深 */
    uint8_t  reserved[2];

    /* ── 曝光/读出 (lines — 与寄存器值同单位，无换算误差) ── */
    uint32_t exposure_lines;                 /**< 当前实际曝光时间 (lines) */
    uint32_t exposure_max_lines;             /**< 当前配置下最大曝光时间 (lines) */
    uint32_t readout_lines;                  /**< 读出跨度 (lines): 首行始→末行止 */

    /* ── 派生物理时间 (ns — 由 ISC 核心从原始值计算) ── */
    uint32_t line_period_ns;                 /**< 行周期: 1e9 × line_length / pixel_clock */
    uint32_t frame_period_ns;                /**< 帧周期: line_period × frame_length */
    uint32_t readout_time_ns;                /**< 读出时间: line_period × readout_lines */
    uint32_t exposure_time_ns;               /**< 曝光时间: line_period × exposure_lines */
    uint32_t exposure_max_ns;                /**< 最大曝光: line_period × exposure_max_lines */
} isc_timing_t;

/* ──── 控制值联合体 ──── */
typedef union {
    int64_t  i64;          /**< INTEGER / ENUM  */
    uint8_t  b;            /**< BOOLEAN         */
    float    f;            /**< FLOAT           */
} isc_ctrl_value_t;

/* ──── 控制描述符 ──── */
typedef struct isc_ctrl_desc {
    uint32_t          cid;                    /**< 控制 ID (含 ISC_CTRL_FLAG_NEXT_CTRL) */
    isc_ctrl_type_t   type;                   /**< 控制类型 */
    const char       *unit;                   /**< 单位字符串 ("ns"/"×"/"dB"/"fps"/NULL) */
    char              name[ISC_MAX_CTRL_NAME];/**< 可读名称 */
    isc_ctrl_value_t  min;                    /**< 最小值 */
    isc_ctrl_value_t  max;                    /**< 最大值 */
    isc_ctrl_value_t  step;                   /**< 步进 */
    isc_ctrl_value_t  def;                    /**< 默认值 */
    uint32_t          flags;                  /**< ISC_CTRL_FLAG_* */
    uint32_t          ctrl_class;             /**< 控制类 */
} isc_ctrl_desc_t;

/* ──── 批量控制 ──── */
typedef struct isc_ext_ctrls {
    uint32_t count;
    uint32_t error_idx;
    struct { uint32_t cid; isc_ctrl_value_t value; } items[ISC_MAX_EXT_CTRLS];
} isc_ext_ctrls_t;

/* ──── 约束类型 ──── */
/**
 * 厂商私有约束基值。各传感器驱动在自身头文件中定义私有约束类型：
 *   #define GSENSE_CONSTRAINT_DANGER_ZONE  (ISC_CONSTRAINT_PRIVATE_BASE + 0)
 *   #define SONY_CONSTRAINT_XXX           (ISC_CONSTRAINT_PRIVATE_BASE + 1)
 *
 * 约束数据结构体同样由驱动头文件定义，isc_query_constraint() 通过 void *
 * 传递，不纳入 isc.h 公共类型。
 */
typedef enum {
    ISC_CONSTRAINT_PRIVATE_BASE = 0,         /**< 厂商私有约束起点 (0–255) */
} isc_constraint_type_t;

/* ──── 回调类型 ──── */
typedef void (*isc_on_ctrl_change_t)(isc_dev_t *dev, uint32_t cid,
                                     isc_ctrl_value_t new_val, void *user_data);
typedef void (*isc_on_error_t)(isc_dev_t *dev, int error_code, void *user_data);
```

### 3.4 公共 API 函数声明

```c
/* ── 生命周期 ── */
/**
 * @brief 初始化 ISC 框架
 *
 * 注册全部传感器驱动、存储 port 和 fpga_ops 引用、初始化内部状态。
 * 必须在使用任何其他 ISC API 前调用，且只能调用一次（重复调用返回 ISC_OK 空操作）。
 *
 * @param[in] port           平台提供的传感器通信原语（必须，非 NULL）
 * @param[in] fpga_ops       FPGA 逻辑同步回调（必须，非 NULL）
 * @param[in] sensors        传感器 ops 表数组（必须，非 NULL）
 * @param[in] sensor_count  传感器驱动数量（必须 >0）
 * @return ISC_OK / ISC_ERR_INVALID_ARG / ISC_ERR_NO_MEM
 */
int isc_init(const isc_port_t *port,
             const isc_fpga_ops_t *fpga_ops,
             const isc_sensor_ops_t *const sensors[],
             uint8_t sensor_count);

/**
 * @brief 反初始化 ISC 框架
 *
 * 关闭所有已打开的设备、释放资源。重复调用安全（空操作）。
 * @return ISC_OK
 */
int isc_deinit(void);

/**
 * @brief 打开传感器设备
 *
 * 两种模式：
 *   - model 非 NULL：按名称精确匹配传感器驱动，init→probe 验证→返回
 *   - model = NULL：自动探测——遍历所有传感器驱动，逐一 init→probe，
 *                    第一个匹配成功即返回，全部失败返回 ISC_ERR_NOT_FOUND
 *
 * 成功后内部调用 isc_get_fmt() 获取默认格式并通知 FPGA:
 *   ioctl(ISC_FPGA_FORMAT_CHANGED, &fmt)
 * 同步给 FPGA 逻辑，确保 FPGA 与传感器初始状态一致。
 *
 * @param[in]  model  传感器型号名（如 "sony_imx477"），NULL=自动探测
 * @param[out] dev    设备句柄
 * @return ISC_OK / ISC_ERR_NOT_FOUND / ISC_ERR_IO / ISC_ERR_*
 */
int isc_open(const char *model, isc_dev_t **dev);

/**
 * @brief 关闭传感器设备
 *
 * 若正在流中则先停止流→传感器下电→释放传感器驱动资源。已关闭设备重复调用安全（空操作）。
 * @param[in] dev
 * @return ISC_OK / ISC_ERR_*
 */
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
```

### 3.5 错误码

| 错误码 | 值 | 含义 |
|--------|-----|------|
| `ISC_OK` | 0 | 成功 |
| `ISC_ERR_INVALID_ARG` | -1 | 参数非法（NULL / 越界 / crop 不合法） |
| `ISC_ERR_NOT_OPEN` | -2 | 设备未 open |
| `ISC_ERR_ALREADY_OPEN` | -3 | 设备已 open（多开保护，预留） |
| `ISC_ERR_NOT_SUPPORTED` | -4 | 传感器/传感器驱动不支持该操作 |
| `ISC_ERR_TIMEOUT` | -5 | 通信超时 |
| `ISC_ERR_NO_MORE` | -6 | 枚举结束（非致命） |
| `ISC_ERR_NO_MEM` | -7 | 内存不足（S0 策略下极少触发，保留） |
| `ISC_ERR_BUSY` | -8 | 设备忙（流中调 set_fmt 等） |
| `ISC_ERR_NOT_FOUND` | -9 | 未匹配到传感器（显式指定型号不存在或自动探测全部失败） |
| `ISC_ERR_IO` | -10 | 底层通信失败 |
| `ISC_ERR_CTRL_RANGE` | -11 | 控制值超出 [min, max] |
| `ISC_ERR_STATE` | -12 | 状态机不允许该操作 |

### 3.6 接口设计检查清单

- [x] 仅凭 `isc.h` + `isc_control.h` + `isc_port.h` 可完成集成（无需看 .c）
- [x] 所有可能失败的操作均有错误返回路径
- [x] `init/deinit` + `open/close` 双层生命周期
- [x] `init/deinit` + `close` 均幂等（重复调用安全）
- [x] 无顺序陷阱：前置条件在文档中标注
- [x] 只读参数 `const`，大小参数 `uint16_t`/`uint32_t`
- [x] `isc_fmt_t` 含完整 crop 窗口（ROI 支持）
- [x] `isc_fmt_desc_t` 含传感器阵列尺寸和裁剪约束
- [x] `isc_timing_t` 提供系统时序计算所需的全部物理量
- [x] `isc_query_constraint()` 厂商私有约束通道，驱动头文件定义类型，void* 传递结构体
- [x] `isc_fpga_ops.ioctl()` FPGA 通用控制通道，预定义 TRIGGER_SET
- [x] `isc_port_t` 传输无关（I2C/SPI/AXI 通用 read/write）
- [x] 能力位图使应用可发现传感器特性组合
- [x] `isc_open(NULL)` 自动探测覆盖"固件不预知传感器型号"场景

---

## 4. 移植层接口（`isc_port.h`）

### 4.1 通用传输

```c
typedef enum {
    ISC_BUS_I2C = 0,
    ISC_BUS_SPI = 1,
    ISC_BUS_AXI = 2,
} isc_bus_type_t;

/**
 * @brief 通用传输原语 — 由用户提供
 *
 * user_data 携带传输句柄:
 *   - I2C: user_data = &{i2c_handle, dev_addr}
 *   - SPI: user_data = &{spi_handle, cs_pin}
 *   - AXI: user_data = &{axi_base_addr}
 *
 * read/write 的 reg_addr 语义由 bus_type 决定：
 *   - I2C:  低 16-bit 为寄存器地址
 *   - SPI:  按传感器约定编码（如首字节 = 0x80 | (addr << 1)）
 *   - AXI:  完整内存映射地址
 */
typedef struct isc_port {
    isc_bus_type_t bus_type;

    /** @brief 传感器寄存器读 */
    int (*read)(void *user_data, uint32_t reg_addr, uint8_t *data, uint16_t len);

    /** @brief 传感器寄存器写 */
    int (*write)(void *user_data, uint32_t reg_addr, const uint8_t *data, uint16_t len);

    /* SPI 专用 (其它总线类型填 NULL) */
    int (*cs_assert)(void *user_data);
    int (*cs_deassert)(void *user_data);

    /* 时序 */
    void (*delay_ms)(uint32_t ms);
    void (*delay_us)(uint32_t us);

    /* GPIO (PWDN / RESET / XCLR) */
    int (*gpio_write)(uint8_t pin, uint8_t level);

    void *user_data;
} isc_port_t;
```

### 4.2 FPGA 同步

```c
/**
 * @brief FPGA 同步/控制接口 — 由用户提供
 *
 * 所有 CPU→FPGA 通信通过此单一通道。ISC 框架在状态变更时
 * 主动调用（格式变更、流启停），传感器驱动也可借用此通道
 * 请求 FPGA 操作（触发控制等）。预定义命令见 ISC_FPGA_* 宏。
 */
typedef struct isc_fpga_ops {
    int (*ioctl)(uint32_t cmd, void *arg, void *user_data);
    void *user_data;
} isc_fpga_ops_t;

/* ──── 框架调用 (ISC → FPGA) ──── */
#define ISC_FPGA_FORMAT_CHANGED    0x0001  /**< arg: const isc_fmt_t *fmt          */
#define ISC_FPGA_STREAM_STATE      0x0002  /**< arg: uint8_t streaming (0=停,1=启) */

/* ──── 驱动调用 (sensor driver → FPGA) ──── */
#define ISC_FPGA_TRIGGER_SET       0x0003  /**< arg: uint8_t enable (0=关, 1=开)  */

/* ──── 厂商扩展 ──── */
#define ISC_FPGA_PRIVATE_BASE      0x0010  /**< 厂商扩展命令起点 (0x0010–0xFFFF) */
```

---

## 5. 控制框架（`isc_control.h`）

### 5.1 CID 分配方案

```
CID 结构 (32-bit):
┌──────────────┬──────────────┐
│  高 16-bit   │  低 16-bit   │
│  控制类      │  控制 ID     │
└──────────────┴──────────────┘
```

| 类 | 基址 | 范围 | 说明 |
|----|------|------|------|
| `ISC_CID_USER_BASE` | `0x00A00000` | 0x000–0x0FF | 翻转、测试模式 |
| `ISC_CID_CAMERA_BASE` | `0x00A10000` | 0x000–0x0FF | 曝光、增益、帧率、WB |
| `ISC_CID_SENSOR_BASE` | `0x00A20000` | 0x000–0x0FF | 黑电平/温度/lane/时钟/位深/行列长度 |
| `ISC_CID_PRIVATE_BASE` | `0x08000000` | 0x000–0xFFFF | 厂商私有 |

### 5.2 标准控制 ID 全集

| 宏 | 值 | 类型 | 流中可改 | 说明 |
|----|-----|------|---------|------|
| `ISC_CID_HFLIP` | user+1 | BOOLEAN | 部分 | 水平镜像，B |
| `ISC_CID_VFLIP` | user+2 | BOOLEAN | 部分 | 垂直镜像，B |
| `ISC_CID_EXPOSURE` | camera+1 | INTEGER | ✅ | 曝光时间 (ns)，I64 |
| `ISC_CID_ANALOG_GAIN` | camera+2 | FLOAT | ✅ | 模拟增益 (×)，F |
| `ISC_CID_DIGITAL_GAIN` | camera+3 | FLOAT | ✅ | 数字增益 (×)，F |
| `ISC_CID_EXPOSURE_AUTO` | camera+4 | ENUM | ✅ | 0=手动, 1=单次, 2=连续，I64 |
| `ISC_CID_GAIN_AUTO` | camera+5 | ENUM | ✅ | 0=手动, 1=单次, 2=连续，I64 |
| `ISC_CID_FRAME_RATE` | camera+6 | FLOAT | ❌ | 帧率 (fps)，F |
| `ISC_CID_TEST_PATTERN` | sensor+1 | ENUM | ❌ | 测试图输出，I64 |
| `ISC_CID_BLACK_LEVEL` | sensor+2 | INTEGER | ✅ | 光学黑像素均值 (DN)，I64 |
| `ISC_CID_TEMPERATURE` | sensor+3 | FLOAT | ✅ | 结温 (℃)，只读，F |
| `ISC_CID_LANE_COUNT` | sensor+4 | ENUM | ❌ | MIPI lane 数，I64 |
| `ISC_CID_PIXEL_CLOCK` | sensor+5 | INTEGER | ❌ | 像素时钟 (Hz)，只读，I64 |
| `ISC_CID_BIT_DEPTH` | sensor+6 | ENUM | ❌ | 输出位深，I64 |
| `ISC_CID_LINE_LENGTH` | sensor+7 | INTEGER | ❌ | 行长度 (pclk)，只读，I64 |
| `ISC_CID_FRAME_LENGTH` | sensor+8 | INTEGER | ❌ | 帧长度 (lines)，只读，I64 |

### 5.3 控制类型与标志位

```c
typedef enum {
    ISC_CTRL_TYPE_INTEGER   = 0,  /**< → .i64            */
    ISC_CTRL_TYPE_BOOLEAN   = 1,  /**< → .b              */
    ISC_CTRL_TYPE_ENUM      = 2,  /**< → .i64 (枚举索引)  */
    ISC_CTRL_TYPE_FLOAT     = 3,  /**< → .f              */
} isc_ctrl_type_t;

#define ISC_CTRL_FLAG_READ_ONLY     (1u << 0)  /**< 只读 */
#define ISC_CTRL_FLAG_WRITE_ONLY    (1u << 1)  /**< 只写 */
#define ISC_CTRL_FLAG_VOLATILE      (1u << 2)  /**< 值可自行变化，get 须真读寄存器 */
#define ISC_CTRL_FLAG_INACTIVE      (1u << 3)  /**< 当前模式下不可用 */
#define ISC_CTRL_FLAG_STREAMABLE    (1u << 4)  /**< 流中可修改 */
#define ISC_CTRL_FLAG_NEXT_CTRL     (1u << 5)  /**< 枚举迭代标记 */
```

---

## 6. 传感器驱动接口（`isc_sensor_ops.h`）

```c
/**
 * @brief 传感器传感器操作表
 *
 * 每个支持的传感器型号提供一个静态常量实例。
 * 未实现的可选操作填 NULL，核心返回 ISC_ERR_NOT_SUPPORTED。
 */
typedef struct isc_sensor_ops {
    /* ── 标识 ── */
    const char *model;          /**< 型号名，与 isc_open() 的 model 参数匹配 */
    const char *vendor;         /**< 厂商名 */
    uint16_t    i2c_addr;       /**< I2C 7-bit 地址（SPI/AXI 传感器驱动填 0） */

    /* ── 生命周期 ── */
    int (*probe)(isc_dev_t *dev);           /**< 读 CHIP_ID 确认型号（必须） */
    int (*init)(isc_dev_t *dev);            /**< 上电时序 + 初始寄存器序列（必须） */
    int (*deinit)(isc_dev_t *dev);          /**< 下电、释放（可选） */
    int (*reset)(isc_dev_t *dev);           /**< 软复位（可选，NULL→deinit+init 降级） */

    /* ── 格式（v2.2: fmt_t 含 crop 字段，传感器驱动须处理） ── */
    int (*enum_fmts)(isc_dev_t *dev, uint8_t index, isc_fmt_desc_t *desc);
    int (*get_fmt)(isc_dev_t *dev, isc_fmt_t *fmt);
    int (*set_fmt)(isc_dev_t *dev, const isc_fmt_t *fmt);
    int (*try_fmt)(isc_dev_t *dev, isc_fmt_t *fmt);  /**< 可选：NULL→核心回滚模拟 */

    /* ── 控制 ── */
    int (*query_ctrl)(isc_dev_t *dev, isc_ctrl_desc_t *desc);
    int (*get_ctrl)(isc_dev_t *dev, uint32_t cid, isc_ctrl_value_t *val);
    int (*set_ctrl)(isc_dev_t *dev, uint32_t cid, isc_ctrl_value_t val);

    /* ── 流 ── */
    int (*stream_on)(isc_dev_t *dev);
    int (*stream_off)(isc_dev_t *dev);

    /* ── 物理状态与约束 ── */
    int (*query_timing)(isc_dev_t *dev, isc_timing_t *timing);         /**< 必须 */
    int (*query_constraint)(isc_dev_t *dev, isc_constraint_type_t type,
                            uint32_t index, void *data);               /**< 可选 */

    /* ── 扩展 ── */
    int (*sensor_ioctl)(isc_dev_t *dev, uint32_t cmd, void *arg);     /**< 可选 */
} isc_sensor_ops_t;
```

---

## 7. 状态机

```
                isc_init(port, fpga_ops, sensors, n)
                    │
                    ▼
   ┌──────────────────────────────┐
   │  ISC_STATE_INIT              │  ← 传感器驱动已注册，无活动设备
   └──────┬───────────────────────┘
          │ isc_open("sony_imx477", &dev)   ← 显式指定
          │ isc_open(NULL, &dev)            ← 自动探测
          ▼
   ┌──────────────────────────────┐
   │  ISC_STATE_OPEN              │  ← 传感器已识别、可配置
   │  open 成功后自动调:           │
   │  fpga_ops.ioctl(FORMAT_CHANGED) │  ← FPGA 获知初始格式
   │  允许: get/set_fmt           │
   │  允许: get/set_ctrl          │
   │  允许: try_fmt / enum_fmt    │
   │  允许: query_timing          │
   │  允许: query_constraint      │
   │  允许: stream_on             │
   └──────┬───────────────────────┘
          │ isc_stream_on()
          ▼
   ┌──────────────────────────────┐
   │  ISC_STATE_STREAMING         │  ← 传感器 LVDS 输出中
   │  允许: get_ctrl              │
   │  允许: set_ctrl (STREAMABLE) │
   │  允许: query_timing          │
   │  允许: stream_off            │
   │  禁止: set_fmt / try_fmt     │
   └──────┬───────────────────────┘
          │ isc_stream_off()
          ▼
       回到 ISC_STATE_OPEN

   任意状态 → isc_close()   → ISC_STATE_INIT
   任意状态 → isc_deinit()  → 全局清理 (幂等)
```

---

## 8. 资源预算

| 资源 | 预算 | 计算依据 |
|------|------|---------|
| **RAM 静态 (BRAM)** | **~1.35 KB** | |
| — `isc_dev_t` 上下文 | 128 B | 状态机 + 格式缓存(含 crop) + port/fpga_ops/sensor_ops 指针 |
| — 控制值缓存 | 320 B | ~20 项 × 16 字节 |
| — 传感器驱动私有上下文 | 128 B | 寄存器镜像 |
| — 时序缓存 | 40 B | `isc_timing_t` |
| — 控制描述符表 | 512 B | ~20 项 × 24 字节（`ISC_CTRL_NO_ENUM` 可裁剪） |
| — 传感器注册表 | 128 B | ≤4 传感器驱动 × 32 字节 |
| **RAM 栈峰值** | **~540 B** | |
| — isc_open 最深路径 | ~320 B | init→probe→多次 port→write→get_fmt→ioctl(FORMAT_CHANGED) |
| — isc_set_fmt (含 crop) | ~220 B | crop 校验 + API 调用帧 |
| **Flash .text** | **~8.8 KB** | |
| — 核心框架 | ~3.9 KB | 注册表(含自动探测) + 状态机 + 格式协商(含 crop) + CID 分发 |
| — 控制框架 | ~1.8 KB | CID 匹配 + clamp/step + 缓存 |
| — 时序/约束查询 | ~0.9 KB | query_timing 派生计算 + query_constraint 类型分发 |
| — 单传感器驱动 | ~2.5 KB | 寄存器表 + 上电 + 控制映射 + 时序采集 |
| **Flash .rodata** | **~2.4 KB** | |
| — 控制项名称 | ~700 B | ~20 × 35 字符（可裁剪） |
| — 格式描述表 | ~200 B | ~10 种格式 + 约束 |
| — 寄存器表 | ~1 KB | 三传感器初始化序列 |
| — 约束描述字符串 | ~300 B | |
| — 错误字符串 | ~200 B | |
| **CPU isc_init** | <0.5 ms | 传感器驱动表拷贝 + 校验（纯 CPU） |
| **CPU isc_open (显式)** | ~15 ms | 上电延时 10ms + I2C 读 ID + 寄存器表 + FPGA 通知 |
| **CPU isc_open (自动探测, 3 传感器驱动)** | ~50 ms | 最坏：前 2 个 fail（各 ~15ms deinit/reinit）+ 第 3 个 success |
| **CPU isc_set_ctrl** | ~300 μs | CID 二分查找 + 1× I2C 写 |
| **CPU isc_set_fmt (含 crop)** | ~800 μs | crop 校验 + 多寄存器写 + FPGA 回调 |
| **CPU isc_query_timing** | ~500 μs | 3–5 次 I2C 读 + 派生计算 |
| **CPU isc_try_fmt** | <80 μs | 纯 CPU，含 crop 约束校验 |

**是否超预算**：否。64KB BRAM 中占比 <3%。自动探测最坏 ~50ms 仍在系统启动预算（~500ms）内。

**极端裁剪**（单传感器驱动 + `ISC_CTRL_NO_ENUM` + `ISC_CTRL_NO_NAME`）：RAM <0.7KB, Flash <5KB。

---

## 9. 设计模式

| 模式 | 解决问题 | 选择理由 |
|------|---------|---------|
| **不透明指针** | 隐藏内部结构 | 零开销，接口稳定 |
| **策略模式 (Sensor Ops)** | 运行时切换传感器 | 运行时多传感器；条件编译仅用于 Flash 裁剪 |
| **工厂模式 (注册表 + 自动探测)** | `isc_open("imx477")` 精确匹配 / `isc_open(NULL)` 自动发现 | 封装发现逻辑，两种使用模式统一入口 |
| **状态机** | 状态转换合法性 | 集中守卫，`ISC_ERR_BUSY` 精准拦截 |
| **V4L2 控制框架** | 标准+扩展统一 API | CID 枚举驱动 UI 自适应 |
| **TRY_FMT 协商** | 试探不改变硬件 | 避免无效 I2C 和 FPGA 误触发；ROI+bin 组合合法性发现 |
| **观察者/回调** | CPU→FPGA 同步 | 事件驱动，时序由 ISC 精确控制 |
| **命令模式 (sensor_ioctl)** | 厂商特殊功能逃生舱 | 保持核心 API 最小化 |

---

## 10. V4L2 设计继承对照

这是本框架最重要的设计参考。下表逐项说明从 V4L2 继承、适配、或舍弃的内容及理由。

### 10.1 完全继承

| V4L2 概念 | ISC 对应 | 说明 |
|-----------|---------|------|
| **VIDIOC_QUERYCAP** | `isc_query_cap()` | 能力位图 + 型号/厂商/格式数/控制数 |
| **VIDIOC_ENUM_FMT** | `isc_enum_fmt()` | 索引迭代枚举，返回 `isc_fmt_desc_t` |
| **VIDIOC_G_FMT / S_FMT** | `isc_get_fmt()` / `isc_set_fmt()` | 格式读写，S_FMT 做 best-match 调整 |
| **VIDIOC_TRY_FMT** | `isc_try_fmt()` | ★ 最重要继承：不改变硬件状态，纯计算格式转换结果 |
| **VIDIOC_S_SELECTION (CROP)** | `isc_fmt_t.crop_*` | 裁剪窗口嵌入格式结构体，S_FMT 一次提交 |
| **VIDIOC_ENUM_FRAMESIZES** | `isc_fmt_desc_t` sensor/step/min/max | 传感器阵列尺寸 + 步进约束 + 输出分辨率范围 |
| **VIDIOC_QUERYCTRL + NEXT_CTRL** | `isc_query_ctrl()` + `ISC_CTRL_FLAG_NEXT_CTRL` | 无条件迭代枚举，驱动 UI 自动生成控制面板 |
| **VIDIOC_QUERYMENU** | `isc_query_menu()` | ENUM 型控制项选项名称查询 |
| **VIDIOC_G_CTRL / S_CTRL** | `isc_get_ctrl()` / `isc_set_ctrl()` | 单控制读写，自动 clamp/step |
| **VIDIOC_G_EXT_CTRLS / S_EXT_CTRLS** | `isc_get_ext_ctrls()` / `isc_set_ext_ctrls()` | 批量操作，error_idx 指示失败位置 |
| **VIDIOC_G_PARM (只读)** | `isc_query_timing()` | 返回物理时序而非配置值 |
| **V4L2 Control Class Hierarchy** | `ISC_CID_*_BASE` 类层次 | 用户/相机/传感器/私有四级分类 |
| **V4L2 Control Flags** | `ISC_CTRL_FLAG_*` | READ_ONLY / VOLATILE / INACTIVE / GRABBED→STREAMABLE |
| **Private CID (V4L2_CID_PRIVATE_BASE)** | `ISC_CID_PRIVATE_BASE` | 厂商扩展控制不破坏标准 CID 空间 |
| **Subdev Model (v4l2_subdev_ops)** | `isc_sensor_ops_t` | 传感器操作虚表，核心与具体型号解耦 |

### 10.2 适配简化

| V4L2 概念 | ISC 处理 | 简化理由 |
|-----------|---------|---------|
| **VIDIOC_REQBUFS / QUERYBUF / QBUF / DQBUF** | 全部移除 | FPGA 处理数据路径，CPU 无缓冲需求 |
| **VIDIOC_STREAMON / STREAMOFF** | `isc_stream_on/off()` | 简化为纯传感器寄存器操作 + FPGA 通知 |
| **vb2_queue / MMAP / DMABUF / USERPTR** | 全部移除 | 无 CPU 侧缓冲管理 |
| **V4L2 Event (ctrl change)** | `isc_on_ctrl_change_t` 回调 | 直接回调替代事件队列（嵌入式无 poll） |
| **ioctl 单入口** | 直接函数 API | 无内核-用户空间边界，编译器可类型检查 |
| **Device Node 命名** | `isc_open(model)` | 无 udev，型号名直接匹配 |

### 10.3 有意舍弃

| V4L2 概念 | 舍弃理由 |
|-----------|---------|
| **Media Controller** | 单传感器，无复杂管线拓扑需求 |
| **Subdev pad / format propagation** | 单级传感器→CSI，无多级格式传递 |
| **Audio / Tuner / Modulator** | 不适用图像传感器 |
| **Sliced VBI** | 不适用 |
| **Multi-planar formats** | 单平面 Bayer/RGB 即可覆盖 |
| **Colorspace / Transfer Function** | 暂不需要；FPGA ISP 自行处理色彩空间转换 |
| **Video Input/Output Routing** | 单传感器无路由 |

### 10.4 V4L2 设计哲学保留清单

| 哲学 | ISC 体现 |
|------|---------|
| **"配置与查询分离"** | `isc_set_fmt()` 配置 / `isc_get_fmt()` 查询 / `isc_query_timing()` 物理状态——三者独立 |
| **"TRY before COMMIT"** | `isc_try_fmt()` 不改变硬件，应用积累多次 TRY 后一次 S_FMT |
| **"能力可发现"** | 能力位图 + 格式枚举 + 控制枚举 + 约束枚举——应用不需硬编码传感器型号 |
| **"扩展不破坏"** | 新 CID 不改变 API 签名，新约束类型不改变 query_constraint 接口 |
| **"驱动与框架分离"** | isc_sensor_ops_t 虚表——新传感器 = 新实例，核心零改动 |
| **"物理约束显式化"** | 时序结构体 + 约束查询——不在注释里口口相传硬件限制 |

---

## 11. 关键设计决策

### 11.1 控制面/数据面彻底分离
FPGA 架构天然将管道切为两部分。ISC 精确继承 V4L2 控制面，数据面让渡给 FPGA。移除所有缓冲管理，CPU 永不接触像素。节省 >1KB RAM。

### 11.2 裁剪窗口嵌入 fmt_t
V4L2 的 Selection API 将 crop/compose/format 分为三次调用——因为它们是独立的硬件模块。嵌入式中传感器通常只有一级裁剪（sensor crop → binning → output），嵌入 `isc_fmt_t` 减少 API 表面积，一次 S_FMT 同时设定裁剪窗口和输出格式。`crop_width=0` 兼容零初始化。

### 11.3 控制值缓存 + VOLATILE 降级
写时更新缓存，读时返回缓存。`ISC_CTRL_FLAG_VOLATILE` 标记项（如自动曝光下的真实曝光值）降级为 I2C 真读。每次 get_ctrl 节省 ~250μs。20 项缓存仅 320 字节。

### 11.4 传输无关端口
`isc_port_t` 使用通用 `read`/`write` + `isc_bus_type_t` 标记，`reg_addr` 为 32-bit。支持 I2C/SPI/AXI 三种物理总线，新增总线类型不改 API。I2C dev_addr 在传感器驱动而非 port（是传感器属性不是总线属性）。

### 11.5 probe 双重角色
`isc_sensor_ops.probe` 在两个场景被调用：
- **显式 `isc_open("imx477")`**：init→probe 验证 CHIP_ID = 0x0477，确认硬件与预期一致
- **自动 `isc_open(NULL)`**：遍历所有传感器驱动逐一 init→probe，第一个 CHIP_ID 匹配即返回。全部失败返回 `ISC_ERR_NOT_FOUND`

### 11.6 传感器校准透明化
传感器特定矫正（曝光危险区躲避、黑电平起始行调整等）在传感器驱动内部完成。应用发送目标值（`isc_set_ctrl`），获取真实状态（`isc_query_timing`），中间矫正由传感器驱动透明处理。`isc_query_constraint()` 仅在应用**必须做决策**时使用（如 ROI 放置）。

### 11.7 能力位图可发现
`ISC_CAP_ROI` / `ISC_CAP_BINNING` / `ISC_CAP_ROI_WITH_BINNING` 使应用在编译期即可知传感器能力矩阵。`isc_try_fmt()` 作为运行时兜底——非法组合自动调整为最接近合法组合。

---

## 12. 场景验证

### 12.1 自动探测（固件不预知传感器型号）

```c
int main(void) {
    const isc_sensor_ops_t *sensors[] = {
        &isc_sensor_sony,
        &isc_sensor_gsense,
        &isc_sensor_smartsens,
    };
    isc_init(&port, &fpga_ops, backs, 3);

    isc_dev_t *dev;
    int rc = isc_open(NULL, &dev);        // 自动探测
    if (rc == ISC_OK) {
        isc_cap_t cap;
        isc_query_cap(dev, &cap);
        printf("检测到: %s %s\n", cap.vendor, cap.model);
        // → "Sony sony_imx477"
    }
}
```

### 12.2 ROI + binning 完整流程

```c
isc_dev_t *dev;
isc_open("sony_imx477", &dev);           // 显式指定（主路径）

isc_fmt_desc_t desc;
isc_enum_fmt(dev, 0, &desc);
// desc.sensor_width=4056, crop_step_x=4, min_crop_width=64

/* 试探: 中心裁 1920×1080 + 2×2 bin → 输出 960×540 */
isc_fmt_t fmt = {
    .width=960, .height=540,
    .pixel_format=ISC_PIX_FMT_SRGGB10,
    .frame_rate_num=120, .frame_rate_den=1,
    .bit_depth=10,
    .reduction=ISC_REDUCTION_BIN_2,
    .crop_left=(4056-1920)/2, .crop_top=(3040-1080)/2,
    .crop_width=1920, .crop_height=1080,
};
isc_try_fmt(dev, &fmt);                  // 约束校验通过
isc_set_fmt(dev, &fmt);                  // 提交 → 写寄存器 → 通知 FPGA

isc_set_ctrl(dev, ISC_CID_EXPOSURE, 500);
isc_set_ctrl(dev, ISC_CID_ANALOG_GAIN, 2048);  // 2.0×
isc_stream_on(dev);
```

### 12.3 GSENSE 校准透明化

```c
isc_set_ctrl(dev, ISC_CID_EXPOSURE, 2000);   // 应用发值

/* 传感器驱动内部自动矫正 */
// gsense_set_ctrl() → 检测 2000 行落入危险区 → 退避到 1984 → 写寄存器
// 应用完全不知情

isc_timing_t t;
isc_query_timing(dev, &t);
// t.exposure_time_ns = 5230000  (1984行的真实时间)
// 应用只需关心最终物理状态
```

### 12.4 Sony 触发协调

```c
/* Sony 传感器驱动: set_ctrl 内部 */
static int sony_set_ctrl(isc_dev_t *dev, uint32_t cid, int32_t val) {
    if (cid == ISC_CID_FRAME_RATE) {
        isc_fpga_ops_t *fpga = dev->fpga_ops;
        uint8_t en = 0; fpga->ioctl(ISC_FPGA_TRIGGER_SET, &en, fpga->user_data);
        sony_write_pll_config(dev, val);
        en = 1; fpga->ioctl(ISC_FPGA_TRIGGER_SET, &en, fpga->user_data);
    }
    return ISC_OK;
}
```

### 12.5 型号 A (ROI+bin) vs 型号 B (互斥)

```c
isc_cap_t cap;
isc_query_cap(dev, &cap);

if (cap.capabilities & ISC_CAP_ROI_WITH_BINNING) {
    /* 型号 A: 直接组合 ROI+bin */
    fmt.crop_width = 1920; fmt.width = 960;
} else {
    /* 型号 B: try_fmt 发现互斥 → 传感器驱动调整为合法组合 */
    isc_try_fmt(dev, &fmt);
    // fmt 被调整为全阵列 crop（放弃 ROI，保留 bin）
    // 或 fmt.width = fmt.crop_width（放弃 bin，保留 ROI）
}
```

---

## 13. 文件清单

| 文件 | 状态 | 说明 |
|------|------|------|
| `isc/isc.h` | 📋 Final | 公共 API + 全部公共类型 (含 crop + 自动探测语义) |
| `isc/isc_control.h` | 📋 Final | 16 个 CID 宏 + 控制类 + 类型枚举 + 标志位 |
| `isc/isc_port.h` | 📋 Final | `isc_port_t` (通用 read/write) + `isc_fpga_ops_t` (含 ioctl) + FPGA 命令宏 |
| `isc/isc_sensor_ops.h` | 📋 Final | `isc_sensor_ops_t` + 注册数据类型 |
| `isc/isc_config.h` | 📋 Final | 编译期裁剪宏 (`ISC_MAX_SENSORS`/`ISC_MAX_CTRLS`/`ISC_CTRL_NO_ENUM` 等) |
| `isc/isc_internal.h` | 📋 Final | `isc_dev_t` 完整定义 + 状态枚举 + 控制缓存 + 时序缓存 |
| `isc/isc_core.c` | 🔶 骨架 | 全部 20 个 API 骨架 + TODO |
| `isc/sensors/isc_sensor_sony.c` | 🔶 骨架 | Sony IMX 传感器驱动 (FPGA ioctl 触发协调 + crop 映射) |
| `isc/sensors/isc_sensor_gsense.c` | 🔶 骨架 | 长光晨芯传感器驱动 (danger_zone 约束 + 内部曝光矫正) |
| `isc/sensors/isc_sensor_smartsens.c` | 🔶 骨架 | 斯特威传感器驱动 |

---

## 14. 后续实现路线图

- [ ] **阶段 1**：生成全部 6 个 `.h` 文件，交叉引用编译通过
- [ ] **阶段 2**：`isc_core.c` — `isc_init`/`isc_deinit` + 传感器注册表
- [ ] **阶段 3**：`isc_core.c` — 状态机守卫
- [ ] **阶段 4**：`isc_core.c` — `isc_open` (含自动探测分支) + `isc_close`
- [ ] **阶段 5**：`isc_core.c` — 格式协商引擎 (try/set/get_fmt + crop 校验 + FPGA 同步)
- [ ] **阶段 6**：`isc_core.c` — 控制框架 (CID 分发 + 缓存 + VOLATILE 降级)
- [ ] **阶段 7**：`isc_core.c` — `isc_query_timing()` 派生计算 + `isc_query_constraint()` 类型分发
- [ ] **阶段 8**：Sony IMX477 完整传感器驱动 (ROI/crop + binning + FPGA ioctl 触发协调)
- [ ] **阶段 9**：GSENSE 传感器驱动 (danger_zone 约束 + 内部曝光矫正)
- [ ] **阶段 10**：SmartSens 传感器驱动骨架
- [ ] **阶段 11**：mock `isc_port_t` 单元测试 (自动探测/格式协商/ROI 约束/控制枚举/时序计算)
- [ ] **阶段 12**：FPGA 硬件集成测试

---

> **设计总结**：ISC 精确继承 V4L2 的六项核心哲学——配置查询分离、TRY-before-COMMIT、能力可发现、扩展不破坏、驱动框架解耦、物理约束显式化——在 FPGA 异构架构中实现控制/数据彻底分离。**加新传感器 = 一个 ops 表实例。配置格式经过 TRY 不扰动硬件。时序约束结构化暴露而非注释口传。应用发值、传感器驱动矫正、query 验证——三层各司其职，无越界耦合。**
