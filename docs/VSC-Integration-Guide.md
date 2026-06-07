# VSC 集成指南 (Integration Guide)

**版本**：1.0.0
**目标读者**：相机固件集成者、FPGA 工程师、现场应用工程师
**前置阅读**：`VSC-System-Architecture.md` (了解系统概览即可)

---

## 1. 环境与移植

### 1.1 工具链

| 工具 | 版本 | 用途 |
|------|------|------|
| GCC | ≥ 8.0 (C11) | 编译固件 |
| Python | ≥ 3.9 | Schema Compiler (`vsc_prop_gen.py`) |
| PyYAML | ≥ 6.0 | YAML 解析 |
| Vivado | ≥ 2022.1 | FPGA 综合 + `vivado_export_graph.tcl` |

### 1.2 安装依赖

```bash
pip install pyyaml
```

### 1.3 目录结构

```
your_project/
├── drivers/                # ← 你需要维护的
│   ├── registry.yaml       #    Driver ID 注册表
│   ├── system_graph.json   #    FPGA 拓扑 (Vivado 生成)
│   ├── board.json          #    板级配置 (手工)
│   ├── fpga/               #    FPGA IP Schema
│   └── sensor/             #    传感器 Schema
├── gen/vsc/                # ← 自动生成 (不要手工编辑)
├── src/vsc/                # ← VSC 框架 (通常不改)
├── src/modules/            # ← 驱动代码 (新增 IP 时添加)
└── tools/                  # ← 工具链
```

### 1.4 首次构建

```bash
# 1. 生成 Schema 产物
python tools/vsc_prop_gen.py drivers gen/vsc \
    --graph drivers/system_graph.json \
    --board drivers/board.json

# 2. 编译 (以 Zephyr 为例)
west build -b your_board

# 3. 校验生成产物未被手动修改
python tools/ci_check_vsc_gen.py
```

---

## 2. IP 管理

### 2.1 新增 IP 驱动

**场景**：FPGA 工程新增了一个 Scaler IP，需要加入 VSC。

**步骤**：

1. **创建 Schema YAML** — `drivers/fpga/scaler.schema.yaml`：

```yaml
driver: scaler
schema_version: 1
caps: [FORMAT_CONV]
description: "图像缩放 IP"

properties:
  - name: max_output_width
    index: 0x00
    type: u32
    flags: [readonly]
    default: 1920
    description: "最大输出宽度"

  - name: output_width
    index: 0x01
    type: u32
    flags: [runtime]
    default: 1920
    min: 64
    max_ref: max_output_width

transform:
  type: PASS_THROUGH
```

2. **注册 Driver_ID** — 编辑 `drivers/registry.yaml`：

```yaml
- id: 0x06
  name: scaler
  category: ip
  version: "1.0"
  status: active
```

3. **编写驱动代码** — 按 `src/modules/ip/crop/` 模板创建：

```
src/modules/ip/scaler/
├── scaler_driver.h       # 纯 HW API
├── scaler_driver.c       # 纯 HW 实现
├── scaler_vsc.h          # VSC 适配器
└── scaler_vsc.c          # vsc_ip_ops_t 实现
```

4. **注册驱动** — 在应用初始化代码中：

```c
#include "scaler_vsc.h"
vsc_driver_register(&scaler_vsc_driver);
```

5. **更新 CMakeLists.txt** — 加入新源文件

6. **重新生成**：

```bash
python tools/vsc_prop_gen.py drivers gen/vsc --graph drivers/system_graph.json --board drivers/board.json
```

### 2.2 删除 IP

1. 从 `drivers/registry.yaml` 将 `status` 改为 `deprecated`，添加 `replaced_by`：

```yaml
- id: 0x06
  name: scaler
  status: deprecated
  replaced_by: "scaler_v2"
```

2. **Driver_ID 永不复用** — 不要将 0x06 分配给新驱动。

3. 从 `system_graph.json` 移除对应节点。

4. 从 CMakeLists.txt 移除源文件。

5. 重新生成。

### 2.3 修改 IP 参数

**场景**：Crop IP 的硬件能力从 max_width=8192 升级到 16384。

1. 编辑 `drivers/fpga/crop.schema.yaml`：

```yaml
- name: max_width
  default: 16384   # 从 8192 改为 16384
  max: 16384
```

2. 更新 `schema_version` 并记录兼容性：

```yaml
schema_version: 2
compatibility:
  breaks:
    - version: 2
      changes:
        - "max_width: range [64,8192] → [64,16384]"
```

3. 重新生成 → CI checksum 变更 → 提交。

### 2.4 按 Instance 收紧参数

`system_graph.json` 支持 per-instance override：

```json
{
  "type": "crop",
  "id": "crop_0",
  "base": "0x43C00000",
  "prop_overrides": {
    "crop.max_width": 1920
  }
}
```

**规则**：override 只能收紧（降低 max、提高 min），不能放宽。违反时 `vsc_system_init()` 返回错误。

---

## 3. 传感器配置与变更

### 3.1 当前支持的传感器

| 型号 | Driver | Schema |
|------|--------|--------|
| Sony IMX477 | `sensor_imx477_vsc_driver` | `drivers/sensor/sensor_imx477.schema.yaml` |

### 3.2 换传感器

**操作**：编辑 `drivers/board.json`：

```json
{
  "sensor": "sensor_imx477",
  "i2c_bus": 2,
  "i2c_addr": "0x1A"
}
```

传感器型号 → `vsc_driver_find("sensor_imx477")` → ISC 框架自动初始化。

**应用代码不需要修改**。

### 3.3 新增传感器型号

1. 在 ISC 框架中实现 `isc_sensor_ops_t` (已有 IMX477 参考 `src/core/isc/sensors/isc_sensor_imx477.c`)
2. 创建 VSC 适配器 `src/modules/sensor/<new>_vsc.c`
3. 创建 Schema YAML `drivers/sensor/<new>.schema.yaml`
4. 注册 Driver_ID + 注册驱动

### 3.4 传感器 I2C 地址

在 `board.json` 中配置。`vsc_system_init()` 自动通过 overrides 传递给传感器驱动的 `ops->init()`。

---

## 4. FPGA 流程

### 4.1 完整链路

```
Vivado Block Design
    │
    │  source tools/vivado_export_graph.tcl
    │  vsc_export_graph design_1 drivers/system_graph.json
    ▼
system_graph.json
    │
    │  python tools/check_graph_types.py drivers/system_graph.json drivers
    ▼
CI type 校验 ✓
    │
    │  python tools/vsc_prop_gen.py drivers gen/vsc \
    │      --graph drivers/system_graph.json --board drivers/board.json
    ▼
gen/vsc/ (8 文件)
    │
    │  west build
    ▼
固件 (含 VSC 初始化数据)
```

### 4.2 Vivado 导出脚本使用

```tcl
# Vivado Tcl Console
source tools/vivado_export_graph.tcl
vsc_export_graph design_1 drivers/system_graph.json
```

**输出**：
```
=== VSC Graph Export ===
  node: crop_0 → type=crop, base=0x43C00000
  node: binning_0 → type=binning, base=0x43C10000
  link: crop_0 → binning_0 (STREAM)
=== Export complete ===
```

### 4.3 IP 类型映射

`vivado_export_graph.tcl` 内置 VLNV → VSC type 映射表。新增 IP 时需在脚本中添加：

```tcl
array set type_map {
    "user.org:ip:scaler:*"   "scaler"
}
```

### 4.4 常见问题

| 问题 | 解决 |
|------|------|
| type 名称不匹配 | 检查 `check_graph_types.py` 输出，在 `registry.yaml` 中添加 `aliases` |
| base_addr 为 0x0 | Vivado 中未 Assign Address → 运行 `assign_bd_address` |
| TAP 未标记 | 目标 IP 名称不在 `classify_link` 的 ANALYZER 列表中 |

---

## 5. 工具参考

### 5.1 `vsc_prop_gen.py`

```bash
python tools/vsc_prop_gen.py <drivers_dir> <output_dir> \
    [--graph system_graph.json] [--board board.json]
```

| 参数 | 必填 | 说明 |
|------|------|------|
| `drivers_dir` | ✅ | `drivers/` 目录 |
| `output_dir` | ✅ | 输出目录 (通常是 `gen/vsc/`) |
| `--graph` | ❌ | `system_graph.json` 路径 |
| `--board` | ❌ | `board.json` 路径 |

**输出文件**：8 个 (`vsc_prop_ids.h`, `vsc_prop_schema.c`, ..., `vsc_system_init.c`)

**退出码**：0 = 成功, 1 = Fatal 错误 (E 类, 不产出文件)

### 5.2 `ci_check_vsc_gen.py`

```bash
python tools/ci_check_vsc_gen.py
```

校验 `gen/vsc/` 与 Schema YAML 的一致性。checksum 不匹配 → 退出码 1。

### 5.3 `check_graph_types.py`

```bash
python tools/check_graph_types.py [system_graph.json] [drivers_dir]
```

校验 `system_graph.json` 中所有 `type` 字段在 `registry.yaml` 中存在。

### 5.4 `vivado_export_graph.tcl`

```tcl
source tools/vivado_export_graph.tcl
vsc_export_graph <bd_name> [output_path]
```

---

## 6. 应用开发

### 6.1 最小可运行示例

```c
#include "vsc_loader.h"
#include "vsc_feature.h"
#include "crop_vsc.h"
#include "binning_vsc.h"
#include "decoder_vsc.h"
#include "histogram_vsc.h"
#include "sensor_vsc.h"

void app_init(void) {
    /* 1. 注册驱动 */
    vsc_driver_register(&sensor_imx477_vsc_driver);
    vsc_driver_register(&crop_vsc_driver);
    vsc_driver_register(&binning_vsc_driver);
    vsc_driver_register(&decoder_vsc_driver);
    vsc_driver_register(&histogram_vsc_driver);

    /* 2. 系统初始化 */
    vsc_pipeline_t pipeline;
    vsc_system_init_default(&pipeline);
}

void app_run(void) {
    /* 3. 设置输出格式 */
    vsc_mbus_fmt_t intent = {1920, 1080, VSC_FMT_RGB888, 30, 1, 8, 4, {0}};
    vsc_resolver_result_t result;
    int rc = vsc_resolver_try_fmt(&pipeline, &intent, &result);

    if (rc != VSC_OK) {
        printf("try_fmt failed: %d\n", rc);
        return;
    }

    printf("Output: %dx%d fmt=0x%08X status=%d\n",
           result.primary_fmt.width,
           result.primary_fmt.height,
           result.primary_fmt.pixel_format,
           result.status);

    /* 4. 检查能力 */
    if (vsc_has_feature(VSC_FEATURE_AUTO_EXPOSURE)) {
        printf("AE available\n");
    }

    /* 5. 提交到硬件 */
    vsc_pipeline_commit_fmt(&pipeline, &result.primary_fmt);

    /* 6. 启动流 */
    // isc_stream_on(sensor_dev);  // ISC 层
}
```

### 6.2 格式协商结果解读

```
intent:   1920×1080 RGB888@30fps
result:   960×540 RGB888     (ADJUSTED)

adjustment_trace:
  [binning_0  SOURCE]  WIDTH  1920→960   (HALVE)
  [decoder_0  SOURCE]  FORMAT RAW→RGB888  (FORMAT_CONV)

解读: Binning 将 1920 折半为 960; Decoder 将 RAW10 转为 RGB888.
      最终输出 960×540 RGB888 —— 如果接受, 调用 commit.
      如果不接受 960×540, 调大 intent 的 width 到 3840 重试.
```

---

## 7. 故障排查

### 7.1 常见错误码

| 错误码 | 含义 | 排查 |
|--------|------|------|
| `-1` (INVALID_INTENT) | intent 参数非法 (width=0 等) | 检查 intent 初始化 |
| `-2` (UNREACHABLE) | 意图超出硬件能力 | 检查 `result.reachable_max` 中的可达范围 |
| `-5` (TOPOLOGY_BROKEN) | Pipeline 链路断裂 | 检查 `system_graph.json` 中的 link 是否完整 |
| `-8` (CANNOT_AUTO_BRIDGE) | 驱动未找到 | 检查 `vsc_driver_register()` 是否已调用, `registry.yaml` 中 type 名称是否匹配 |

### 7.2 adjustment_trace 为空

可能原因：所有 entity 的 transform 都是 PASS_THROUGH → 格式没有任何修改。

### 7.3 Feature 始终为 false

1. 确认 `vsc_feature_derive()` 已调用
2. 确认对应驱动已 `vsc_driver_register()`
3. 检查驱动的 `capabilities` 字段是否包含正确的 `VSC_CAP_*` 位

### 7.4 Schema 变更后 feature 未更新

```bash
python tools/vsc_prop_gen.py drivers gen/vsc --graph drivers/system_graph.json --board drivers/board.json
```

必须重新生成 → 重新编译。

---

## 8. 更新机制

| 触发条件 | 需要做什么 |
|---------|-----------|
| Schema YAML 变更 | 运行 `vsc_prop_gen.py` → 重新生成 → CI checksum 校验 → 提交 |
| 新增 IP 驱动 | 创建 Schema YAML + 纯 HW 驱动 + VSC 适配器 + 注册 Driver_ID + 重新生成 |
| 换传感器 | 编辑 `board.json` → 重新生成 |
| 换 FPGA 比特流 | Vivado 导出 `system_graph.json` → 重新生成 |
| VSC 框架升级 | 阅读对应 `notes/` 设计文档 → 更新驱动适配器 (如有 ABI 变更) |
