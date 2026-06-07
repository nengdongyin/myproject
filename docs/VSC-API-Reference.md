# VSC 接口规范 (API Reference)

**版本**：1.0.0

---

## 1. 公共类型

### 1.1 格式描述符 `vsc_mbus_fmt_t`

```c
typedef struct {
    uint32_t width;            /* 图像宽度 (pixels)              */
    uint32_t height;           /* 图像高度 (lines)               */
    uint32_t pixel_format;     /* FourCC: VSC_FMT_RAW8/10/12, RGB888, YUV422 */
    uint32_t frame_rate_num;   /* 帧率分子, 0=不关心             */
    uint32_t frame_rate_den;   /* 帧率分母                       */
    uint8_t  bit_depth;        /* 像素位深                       */
    uint8_t  lanes;            /* 数据通道数                     */
} vsc_mbus_fmt_t;
```

### 1.2 像素格式常量

```c
#define VSC_FMT_RAW8    0x52415738u   /* "RAW8"  */
#define VSC_FMT_RAW10   0x52415741u   /* "RAWA"  */
#define VSC_FMT_RAW12   0x52415742u   /* "RAWB"  */
#define VSC_FMT_RGB888  0x52474238u   /* "RGB8"  */
#define VSC_FMT_YUV422  0x59555532u   /* "YUV2"  */
#define VSC_FMT_INVALID 0x00000000u
```

### 1.3 错误码

```c
#define VSC_OK                         0   /* 成功                    */
#define VSC_ERR_INVALID_INTENT        -1   /* 格式意图非法            */
#define VSC_ERR_UNREACHABLE           -2   /* 意图超出硬件能力        */
#define VSC_ERR_PROPAGATION_SINK      -3   /* 下游拒绝格式            */
#define VSC_ERR_PROPAGATION_SOURCE    -4   /* 上游无有效输出          */
#define VSC_ERR_TOPOLOGY_BROKEN       -5   /* 拓扑断裂                */
#define VSC_ERR_NO_ACTIVE_ENDPOINT    -6   /* 无活跃端点              */
#define VSC_ERR_NO_SENSOR             -7   /* 无传感器                */
#define VSC_ERR_CANNOT_AUTO_BRIDGE    -8   /* 无法自动桥接            */
#define VSC_ERR_BUSY                  -9   /* 设备忙 (流中)           */
#define VSC_ERR_COMMIT_FAILED         -10  /* 硬件写入失败            */
#define VSC_WARN_TAP_INACTIVE          1   /* TAP 失效警告            */
```

### 1.4 协商状态

```c
typedef enum {
    VSC_NEGOTIATE_EXACT    = 0,  /* intent 完全匹配              */
    VSC_NEGOTIATE_ADJUSTED = 1,  /* 经修改后接受                  */
    VSC_NEGOTIATE_PARTIAL  = 2,  /* 部分分支/TAP 失效             */
    VSC_NEGOTIATE_FAILED   = 3,  /* 无有效配置                    */
} vsc_negotiation_status_t;
```

### 1.5 协商结果

```c
typedef struct {
    vsc_negotiation_status_t status;          /* 协商状态          */
    vsc_mbus_fmt_t           primary_fmt;     /* 主输出格式        */
    char                     primary_endpoint[32]; /* 主端点名称   */
    uint8_t                  num_endpoints;   /* 端点数            */
    vsc_endpoint_fmt_t       endpoint_fmts[8];/* 各端点格式        */
    vsc_adjustment_trace_t   trace;           /* 格式变更轨迹      */
    vsc_mbus_fmt_t           reachable_max;   /* 可达上限(失败时)  */
} vsc_resolver_result_t;
```

---

## 2. 应用层 API

### 2.1 格式协商

```c
/**
 * @brief 全链路格式协商
 * @param pipeline  已构建的管线
 * @param intent    应用期望的输出格式
 * @param result    [out] 协商结果
 * @return VSC_OK 或错误码
 */
int vsc_resolver_try_fmt(vsc_pipeline_t *pipeline,
                         const vsc_mbus_fmt_t *intent,
                         vsc_resolver_result_t *result);
```

**使用示例**：

```c
vsc_mbus_fmt_t intent = {1920, 1080, VSC_FMT_RGB888, 30, 1, 8, 4, {0}};
vsc_resolver_result_t result;
int rc = vsc_resolver_try_fmt(&pipeline, &intent, &result);
if (rc == VSC_OK && result.status == VSC_NEGOTIATE_EXACT) {
    vsc_pipeline_commit_fmt(&pipeline, &result.primary_fmt);
}
```

### 2.2 能力查询

```c
/**
 * @brief 查询系统是否支持某项功能
 * @param feature  功能 ID (VSC_FEATURE_*)
 * @return true 如果可用
 */
bool vsc_has_feature(vsc_feature_id_t feature);
```

**Feature ID 清单**：

| ID | 含义 |
|----|------|
| `VSC_FEATURE_STREAMING` | 视频流输出 |
| `VSC_FEATURE_AUTO_EXPOSURE` | 自动曝光控制 |
| `VSC_FEATURE_AUTO_WHITE_BALANCE` | 自动白平衡 |
| `VSC_FEATURE_HDR` | HDR 模式 |
| `VSC_FEATURE_TRIGGER` | 外部触发 |
| `VSC_FEATURE_CROP` | 裁剪/ROI |
| `VSC_FEATURE_BINNING` | Binning/降采样 |
| `VSC_FEATURE_HISTOGRAM` | 直方图统计 |

### 2.3 系统初始化

```c
/**
 * @brief 从描述符构建完整 Pipeline
 * @param desc     system_graph.json 解析结果
 * @param board    board.json 解析结果
 * @param pipeline [out] 构建好的管线
 * @return VSC_OK 或错误码
 */
int vsc_system_init(const vsc_system_desc_t *desc,
                    const vsc_board_config_t *board,
                    vsc_pipeline_t *pipeline);

/**
 * @brief 使用编译期生成的描述符初始化（构建时由 vsc_prop_gen.py 生成）
 */
int vsc_system_init_default(vsc_pipeline_t *pipeline);
```

### 2.4 流控制

```c
/**
 * @brief 提交格式到硬件
 * @return VSC_OK 或 VSC_ERR_COMMIT_FAILED / VSC_ERR_BUSY
 */
int vsc_pipeline_commit_fmt(vsc_pipeline_t *pipeline,
                            const vsc_mbus_fmt_t *final_fmt);
```

---

## 3. 驱动层 API (`vsc_ip_ops_t`)

每个 IP 驱动必须实现此虚表：

```c
struct vsc_ip_ops {
    /** @brief 实例初始化 — 分配上下文，保存 base_addr，应用 overrides */
    int (*init)(void **drv_ctx, uint32_t base_addr,
                const vsc_override_t *overrides, uint8_t num_over);

    /** @brief 格式校验 — 给定输入格式，返回可接受的最接近格式 */
    int (*try_fmt_sink)(void *drv_ctx, const vsc_mbus_fmt_t *proposed,
                        vsc_mbus_fmt_t *clamped);

    /** @brief 格式变换 — 给定输入格式，计算此 IP 的输出格式 */
    int (*try_fmt_source)(void *drv_ctx, const vsc_mbus_fmt_t *sink_fmt,
                          vsc_mbus_fmt_t *source_fmt);

    /** @brief 硬件提交 — 将最终格式写入 FPGA 寄存器 */
    int (*commit_fmt)(void *drv_ctx, const vsc_mbus_fmt_t *final_fmt);
};
```

**按 Entity Class 的实现要求**：

| Class | init | try_fmt_sink | try_fmt_source | commit_fmt |
|-------|------|-------------|----------------|------------|
| SENSOR | ✅ | NULL | ✅ | ✅ |
| STREAM | ✅ | ✅ | ✅ | ✅ |
| ANALYZER | ✅ | ✅ | NULL | NULL |
| ENDPOINT | ✅ | ✅ | NULL | ✅ |

---

## 4. 驱动注册 API

```c
/** @brief 注册驱动到全局注册表 */
void vsc_driver_register(const vsc_driver_t *driver);

/** @brief 按名称查找驱动 (先查注册表, 再查生成表) */
const vsc_driver_t *vsc_driver_find(const char *name);

/** @brief 按索引遍历所有驱动 (注册表优先) */
const vsc_driver_t *vsc_driver_by_index(int idx);
```

---

## 5. Pipeline 管理 API

```c
/** @brief 构建拓扑：Kahn 拓扑排序 + 端点/TAP 分类 + 邻接表 */
int vsc_pipeline_build(vsc_pipeline_t *pipeline);

/** @brief 移除可选 entity 并自动桥接 (SISO 链路) */
int vsc_pipeline_remove_optional(vsc_pipeline_t *pipeline, uint8_t idx);

/** @brief 从 Driver 创建 Instance，应用 overrides */
int vsc_instance_create(const vsc_driver_t *driver, uint32_t base_addr,
                        const vsc_override_t *overrides, uint8_t num_over,
                        vsc_entity_t *entity);
```

---

## 6. Feature API

```c
void vsc_feature_derive(void);                    /* 自动推导 */
bool vsc_has_feature(vsc_feature_id_t feature);   /* 查询     */
const vsc_feature_t *vsc_feature_get(vsc_feature_id_t f); /* 描述 */
void vsc_feature_dump(void);                      /* 打印     */
```

---

## 7. 驱动能力位掩码 (`VSC_CAP_*`)

```c
#define VSC_CAP_SENSOR          (1 << 0)   /* 图像传感器              */
#define VSC_CAP_EXPOSURE_CTRL   (1 << 1)   /* 曝光控制                */
#define VSC_CAP_STATISTICS      (1 << 2)   /* 统计数据输出            */
#define VSC_CAP_HDR             (1 << 3)   /* HDR 支持                */
#define VSC_CAP_TRIGGER         (1 << 4)   /* 外部触发                */
#define VSC_CAP_CROP            (1 << 5)   /* 裁剪/ROI                */
#define VSC_CAP_BINNING         (1 << 6)   /* Binning/降采样          */
#define VSC_CAP_FORMAT_CONV     (1 << 7)   /* 像素格式转换            */
```

---

## 8. Transform 类型

```c
typedef enum {
    VSC_TRANSFORM_PASS_THROUGH   = 0,  /* 透传                      */
    VSC_TRANSFORM_BINNING        = 1,  /* 折半 (factor_x/y)          */
    VSC_TRANSFORM_CROP           = 2,  /* 裁剪 (min/max/align)      */
    VSC_TRANSFORM_PIXEL_FMT_CONV = 3,  /* 格式转换 (fmt_in→fmt_out) */
    VSC_TRANSFORM_MULTI_STAGE    = 4,  /* 复合变换 (subs[])          */
} vsc_fmt_transform_type_t;
```

---

## 9. Entity / Link / Pipeline 结构

### 9.1 Entity

```c
typedef struct vsc_entity {
    char                      name[32];
    vsc_entity_class_t        entity_class;   /* SENSOR/STREAM/ANALYZER/ENDPOINT */
    vsc_fmt_transform_desc_t  transform_desc; /* Phase 1 代数模型                */
    vsc_propagation_state_t   prop_state;     /* Phase 2 运行时状态              */
    const vsc_ip_ops_t       *ops;            /* 驱动虚表 (共享)                */
    void                     *drv_ctx;        /* 驱动私有上下文                  */
} vsc_entity_t;
```

### 9.2 Entity Class

```c
typedef enum {
    VSC_ENTITY_SENSOR   = 0,  /* 数据源, 仅 SOURCE pad           */
    VSC_ENTITY_STREAM   = 1,  /* 处理节点, SINK + SOURCE pads    */
    VSC_ENTITY_ANALYZER = 2,  /* TAP 观察者, 仅 SINK pad         */
    VSC_ENTITY_ENDPOINT = 3,  /* 数据汇, 仅 SINK pad             */
} vsc_entity_class_t;
```

### 9.3 Link

```c
typedef struct {
    uint8_t          src_entity;   /* 源 entity 索引              */
    uint8_t          dst_entity;   /* 目标 entity 索引            */
    vsc_link_type_t  type;         /* STREAM / TAP                */
} vsc_link_t;
```

### 9.4 Pipeline

```c
typedef struct {
    uint8_t       num_entities;
    vsc_entity_t  entities[16];          /* Entity 数组            */
    uint8_t       num_links;
    vsc_link_t    links[32];             /* Link 数组              */
    uint8_t       execution_order[16];   /* 拓扑排序结果           */
    uint8_t       adj_dst[16][8];        /* 邻接表 (dst per src)   */
    vsc_pipeline_state_t state;
} vsc_pipeline_t;
```
