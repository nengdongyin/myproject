# VSC 视频流控制子系统 — 系统架构设计 (SAD)

**文档版本**：1.0.0
**对应代码版本**：P0/P1/P1.1/P2 冻结，47 测试通过
**适用范围**：FPGA + MicroBlaze SoC 工业相机嵌入式系统

---

## 1. 系统概述

### 1.1 目标

VSC（Video Stream Controller）是一个嵌入式视频流控制子系统，运行在 MicroBlaze 软核 CPU 上，负责 FPGA 视频管线的初始化配置和参数管理。核心目标是实现**应用层代码可移植性**——换传感器、换 FPGA IP、换链路拓扑时，应用代码零修改。

### 1.2 物理系统上下文

```
┌─────────────────────────────────────────────────────────────┐
│  FPGA + MicroBlaze SoC                                      │
│                                                             │
│  图像传感器 ──→ LVDS接收器 ──→ Binning ──→ 解码 ──→ 裁剪 ──→ 输出 │
│      │                                                            │
│      └── I2C/SPI ──→ MicroBlaze (VSC 子系统)                      │
│                         │                                        │
│                         ├── 传感器初始化/配置                      │
│                         ├── 格式协商 (try_fmt/set_fmt)            │
│                         ├── 流控制 (stream_on/off)                │
│                         └── 参数管理 (曝光/增益/锐化...)           │
└─────────────────────────────────────────────────────────────┘
```

---

## 2. 分层架构

```
┌──────────────────────────────────────────────────┐
│  应用层 (Application Layer)                      │
│  vsc_set_output()  vsc_has_feature()             │
│  vsc_set_param()   vsc_stream_on/off()           │
├──────────────────────────────────────────────────┤
│  P2 Feature System (能力查询)                     │
│  vsc_has_feature(VSC_FEATURE_AUTO_EXPOSURE)      │
├──────────────────────────────────────────────────┤
│  P0 Property Resolver (格式协商引擎)              │
│  Phase 1: feasibility_check (区间逆推)            │
│  Phase 2: forward_propagate (BFS 正向传播)        │
│  Phase 3: converge (多分支收敛 + trace)           │
│  Phase 4: commit (硬件提交)                       │
├──────────────────────────────────────────────────┤
│  P0 Pipeline Graph (拓扑管理)                    │
│  Entity / Pad / Link / 拓扑排序 / 自动桥接        │
├──────────────────────────────────────────────────┤
│  Bitstream Loader (系统初始化)                    │
│  system_graph.json → vsc_system_init()            │
├───────────────┬──────────────────────────────────┤
│  P1 Schema    │  驱动层                           │
│  + Codegen    │  vsc_ip_ops_t 虚表                │
│  (YAML→C)     │  crop/binning/decoder/histogram   │
│               │  sensor_imx477 (ISC 桥接)          │
├───────────────┴──────────────────────────────────┤
│  ISC (Image Sensor Controller) — 传感器控制框架   │
│  I2C/SPI 寄存器读写 / 上电时序 / CID 控制          │
├──────────────────────────────────────────────────┤
│  FPGA 硬件寄存器                                  │
└──────────────────────────────────────────────────┘
```

### 2.1 各层职责

| 层 | 职责 | 关键接口 |
|----|------|---------|
| 应用层 | 表达用户意图（分辨率/帧率/参数） | `vsc_set_output()`, `vsc_has_feature()` |
| Feature System | 硬件能力抽象查询 | `vsc_has_feature()`, `vsc_feature_derive()` |
| Property Resolver | 格式协商：用户意图 → 硬件可达格式 | `vsc_resolver_try_fmt()` |
| Pipeline Graph | 视频链路拓扑管理 | `vsc_pipeline_build()` |
| Bitstream Loader | 从 JSON 自动构建 Pipeline | `vsc_system_init()`, `vsc_system_init_default()` |
| Schema + Codegen | 驱动元数据 (YAML → C 代码生成) | `vsc_prop_gen.py` |
| 驱动层 | 硬件寄存器读写 + 格式变换 | `vsc_ip_ops_t::try_fmt_sink/source/commit` |
| ISC | 传感器 I2C 控制 | `isc_init()`, `isc_open()`, `isc_try_fmt()` |

---

## 3. 核心数据流

### 3.1 格式协商完整流程

```
应用: vsc_set_output({1920, 1080, RGB888, 30})
        │
        ▼
vsc_resolver_try_fmt(pipeline, intent, &result)
    │
    ├── Phase 1: feasibility_check()
    │   ┌──────────────────────────────────────┐
    │   │ 逆推: 1920×1080 RGB888               │
    │   │  → decoder 逆推 (RAW10)              │
    │   │  → binning 逆推 (×2: 3840×2160)     │
    │   │  → crop 逆推 (≥3840)                │
    │   │  → 检查 sensor max=4056×3040 ✓       │
    │   └──────────────────────────────────────┘
    │
    ├── Phase 2: forward_propagate()
    │   ┌──────────────────────────────────────┐
    │   │ BFS 正向传播:                        │
    │   │  sensor→1920×1080 RAW10              │
    │   │  → crop→1920×1080 RAW10             │
    │   │  → binning→960×540 RAW10            │
    │   │  → decoder→960×540 RGB888           │
    │   └──────────────────────────────────────┘
    │
    ├── Phase 3: converge()
    │   ┌──────────────────────────────────────┐
    │   │ primary_fmt = {960, 540, RGB888}     │
    │   │ status = ADJUSTED                    │
    │   │ adjustment_trace:                    │
    │   │  [binning: WIDTH halved]            │
    │   │  [decoder: FORMAT RAW→RGB]          │
    │   └──────────────────────────────────────┘
    │
    └── Phase 4: commit()
        逐级写硬件寄存器
```

### 3.2 系统初始化流程

```
system_graph.json ──┐
board.json         ──┤
                     │
                     ▼
         vsc_system_init(desc, board, pipeline)
              │
              ├── Step 1: 创建 SENSOR entity (board.sensor_type)
              ├── Step 2: 创建 FPGA entities (desc.nodes[])
              ├── Step 3: 隐式 link: sensor → first_fpga
              ├── Step 4: 显式 links (desc.links[])
              ├── Step 5: vsc_pipeline_build() → Kahn 拓扑排序
              └── Step 6: vsc_feature_derive()
```

### 3.3 驱动注册与实例化

```
vsc_driver_register(&crop_vsc_driver)  → g_registered[]
vsc_driver_register(&binning_vsc_driver)
...

vsc_driver_find("crop") → 查找 g_registered[] + _vsc_drivers[]

vsc_instance_create(driver, base_addr, overrides, &entity)
    ├── entity->transform_desc = *driver->transform_template
    ├── 应用 overrides (收紧)
    ├── driver->ops.init(&drv_ctx, base_addr, overrides)
    └── entity->ops = &driver->ops
```

---

## 4. 核心设计决策

| 决策 | 选择 | 理由 |
|------|------|------|
| 内存策略 | S0 — 全静态，零 malloc | MicroBlaze 无 MMU，BRAM 仅 64KB |
| 格式协商方向 | source→sink 单向传播 | FPGA 流水线无反馈，双向协商不必要 |
| Link 语义 | STREAM (数据流) / TAP (旁路) | 统计模块不阻塞主链路 |
| Entity 分类 | SENSOR / STREAM / ANALYZER / ENDPOINT | 不同 class 有不同接口契约 |
| 拓扑真相源 | Bitstream Descriptor (自动生成) | 避免软件手工维护与 RTL 不同步 |
| 控制闭环 | Algorithm Service (独立于 Pipeline) | AE 周期与帧率解耦 |
| 能力表达 | Capability 位掩码 + Property 数值 | 比版本号更稳定，比纯位掩码更灵活 |
| Property ID | (Driver_ID << 8) \| Property_Index | 人肉可反解，调试可读 |
| JSON 解析 | 构建时 Python 生成 C 代码 | 零运行时开销，与 Schema Compiler 风格统一 |

---

## 5. 模块依赖关系

```
vsc_types.h                     ← 所有模块的基础类型
    │
    ├── vsc_resolver.c          ← 格式协商引擎 (P0)
    ├── vsc_loader.c            ← 系统初始化 (Bitstream Loader)
    ├── vsc_driver_registry.c   ← 驱动注册表
    ├── vsc_feature.c           ← 能力查询 (P2)
    │
    └── 驱动适配器 (VSC 侧)
        ├── crop_vsc.c          ← 依赖 crop_driver.c (纯 HW)
        ├── binning_vsc.c       ← 依赖 binning_driver.c (纯 HW)
        ├── decoder_vsc.c       ← 依赖 decoder_driver.c (纯 HW)
        ├── histogram_vsc.c     ← 依赖 histogram_driver.c (纯 HW)
        └── sensor_vsc.c        ← 依赖 ISC 框架 (isc_bridge.c)
                                    └── isc_core.c + isc_sensor_imx477.c

纯 HW 驱动（零框架依赖）：
    crop_driver.c / binning_driver.c / decoder_driver.c / histogram_driver.c
```

---

## 6. 文件清单

### 6.1 核心框架 (`src/vsc/`)

| 文件 | 行数 | 说明 |
|------|------|------|
| `vsc_types.h` | 540 | 全部类型定义 (Entity/Link/Pipeline/Resolver 结果) |
| `vsc_resolver.c` | 880 | 格式协商引擎 (Phase 1-4 + 拓扑排序 + 自动桥接) |
| `vsc_loader.h` | 120 | Bitstream Loader API |
| `vsc_loader.c` | 160 | 系统初始化 + Instance 创建 |
| `vsc_driver_registry.h` | 30 | 驱动注册表 API |
| `vsc_driver_registry.c` | 60 | 驱动注册、查找、索引 |
| `vsc_feature.h` | 100 | Feature 系统 API |
| `vsc_feature.c` | 140 | Feature 推导 + 查询 |

### 6.2 驱动层 (`src/modules/`)

| 目录 | 文件 | 说明 |
|------|------|------|
| `ip/crop/` | `crop_driver.h/.c` (120 行) | Crop IP 纯 HW 驱动 |
| | `crop_vsc.h/.c` (170 行) | Crop VSC 适配器 |
| `ip/binning/` | `binning_driver.h/.c` (80 行) | Binning IP 纯 HW 驱动 |
| | `binning_vsc.h/.c` (80 行) | Binning VSC 适配器 |
| `ip/decoder/` | `decoder_driver.h/.c` (80 行) | Decoder IP 纯 HW 驱动 |
| | `decoder_vsc.h/.c` (70 行) | Decoder VSC 适配器 |
| `ip/histogram/` | `histogram_driver.h/.c` (70 行) | Histogram IP 纯 HW 驱动 |
| | `histogram_vsc.h/.c` (60 行) | Histogram VSC 适配器 (ANALYZER) |
| `sensor/` | `sensor_vsc.h/.c` (130 行) | Sensor VSC 适配器 (ISC 桥接) |
| | `isc_bridge.h/.c` (100 行) | ISC 桥接模块 |

### 6.3 工具链 (`tools/`)

| 文件 | 行数 | 说明 |
|------|------|------|
| `vsc_prop_gen.py` | 90 | Schema Compiler 主入口 |
| `vsc_compiler/ast_nodes.py` | 70 | AST 节点定义 |
| `vsc_compiler/parser.py` | 120 | YAML 解析器 |
| `vsc_compiler/checker.py` | 230 | 语义检查器 (18 Fatal + 5 Warning) |
| `vsc_compiler/depgraph.py` | 60 | 依赖图 + Kahn 拓扑排序 |
| `vsc_compiler/codegen.py` | 370 | C 代码生成器 (7 文件) |
| `vsc_compiler/checksum.py` | 30 | 确定性 SHA256 |
| `vivado_export_graph.tcl` | 220 | Vivado Block Design → system_graph.json |
| `check_graph_types.py` | 60 | CI type 名称校验 |
| `ci_check_vsc_gen.py` | 50 | CI checksum 校验 |

### 6.4 配置与生成产物

| 路径 | 说明 |
|------|------|
| `drivers/registry.yaml` | Driver ID 注册表 (5 active + 1 deprecated) |
| `drivers/fpga/*.schema.yaml` | FPGA IP Schema (crop/binning/decoder) |
| `drivers/analyzer/*.schema.yaml` | ANALYZER Schema (histogram) |
| `drivers/sensor/*.schema.yaml` | Sensor Schema (sensor_imx477) |
| `drivers/system_graph.json` | FPGA 拓扑描述 (示例) |
| `drivers/board.json` | 硬件板级配置 (示例) |
| `gen/vsc/` | Codegen 产物 (8 文件) |

### 6.5 测试

| 文件 | 行数 | 说明 |
|------|------|------|
| `src/test/vsc/test_vsc_resolver.c` | 1500 | 47 个单元测试 |
| `test_runner_vsc.c` | 30 | 独立测试运行器 |

---

## 7. 构建与运行

### 7.1 构建

```bash
# Schema Compiler (每次 Schema YAML 变更后运行)
python tools/vsc_prop_gen.py drivers gen/vsc \
    --graph drivers/system_graph.json \
    --board drivers/board.json

# CI checksum 校验
python tools/ci_check_vsc_gen.py

# CI type 校验
python tools/check_graph_types.py drivers/system_graph.json drivers
```

### 7.2 测试

```bash
# 编译测试
gcc -std=c11 -Isrc/vsc -Igen/vsc -Isrc/test/core -Isrc/test/core/unity \
    src/vsc/*.c src/modules/**/*.c gen/vsc/*.c src/test/vsc/*.c \
    test_runner_vsc.c -o build/test_vsc

# 运行
./build/test_vsc
# 输出: 47 Tests 0 Failures 0 Ignored OK
```

### 7.3 Vivado 集成

```tcl
# Vivado Tcl Console
source tools/vivado_export_graph.tcl
vsc_export_graph design_1 drivers/system_graph.json
```

---

## 8. 约束与限制

| 约束 | 值 | 说明 |
|------|-----|------|
| S0 内存 | 零 malloc | 全部静态数组 |
| 最大 Entity 数 | 16 | `VSC_MAX_ENTITIES` |
| 最大 Link 数 | 32 | `VSC_MAX_LINKS` |
| 每 Entity 最大下游 | 8 | `VSC_MAX_ADJ` |
| 每 Driver 最大 Property | 255 | 8-bit Property_Index |
| 最大 Driver 数 | 254 | 8-bit Driver_ID，0x00/0xFF 保留 |
| MULTI_STAGE 最大深度 | 4 | 编译期限制 |
