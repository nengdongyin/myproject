# VSC 开发规范

**版本**：1.0.0

---

## 1. 编码规范

### 1.1 C 代码风格

- **标准**：C11 (`-std=c11`)
- **编译选项**：`-Wall -Wextra -Werror -pedantic`
- **命名**：
  - 类型：`vsc_` 前缀 + 小写下划线 (`vsc_mbus_fmt_t`, `vsc_pipeline_t`)
  - 函数：`vsc_` 前缀 + 小写下划线 (`vsc_resolver_try_fmt`)
  - 宏/常量：全大写下划线 (`VSC_MAX_ENTITIES`, `VSC_FMT_RAW8`)
  - 枚举值：全大写下划线 (`VSC_ENTITY_SENSOR`, `VSC_TRANSFORM_CROP`)
- **缩进**：4 空格，无 Tab
- **行宽**：≤ 100 字符
- **注释语言**：中文 (核心算法) + 英文 (API 文档)

### 1.2 S0 内存策略

- **禁止** `malloc` / `free`
- 全部结构体使用**固定大小静态数组**
- 编译期常量定义上限 (`VSC_MAX_*`)
- 池分配模式：`static ctx_pool[MAX]` + `in_use` 标志

### 1.3 文件组织

```
src/
├── vsc/              # VSC 核心框架 (无硬件依赖)
│   ├── vsc_types.h       # 全部公共类型
│   ├── vsc_resolver.c    # 格式协商引擎
│   ├── vsc_loader.c      # 系统初始化
│   ├── vsc_driver_registry.c  # 驱动注册表
│   └── vsc_feature.c     # 能力查询
│
├── modules/          # 驱动层
│   ├── ip/               # FPGA IP 驱动
│   │   ├── crop/
│   │   │   ├── crop_driver.h/.c    # 纯 HW 驱动 (零框架依赖)
│   │   │   └── crop_vsc.h/.c       # VSC 适配器
│   │   ├── binning/
│   │   ├── decoder/
│   │   └── histogram/
│   └── sensor/           # 传感器驱动
│       ├── sensor_vsc.h/.c         # ISC 桥接适配器
│       └── isc_bridge.c            # ISC 框架初始化
│
├── core/isc/          # ISC 传感器控制框架 (已有)
│
└── test/vsc/          # 单元测试
    └── test_vsc_resolver.c
```

### 1.4 依赖方向

```
纯 HW 驱动 (无框架依赖)
    ↑
VSC 适配器 (引用 vsc_types.h + 纯 HW 驱动)
    ↑
VSC 核心框架 (引用 vsc_types.h)
    ↑
应用层 (引用 VSC API)
```

**纯 HW 驱动不引用**：`vsc_types.h`、`param_manager.h`、任何 VSC 内部头文件。
**VSC 适配器引用**：`vsc_types.h`、对应的纯 HW 驱动头文件。

---

## 2. 新 IP 驱动开发模板

### 2.1 文件清单

每个 IP 驱动需要 4 个文件：

```
src/modules/ip/<your_ip>/
├── <your_ip>_driver.h     # 纯 HW 驱动 API (零框架依赖)
├── <your_ip>_driver.c     # 纯 HW 驱动实现
├── <your_ip>_vsc.h        # VSC 适配器 API
└── <your_ip>_vsc.c        # VSC 适配器实现 (vsc_ip_ops_t)
```

### 2.2 纯 HW 驱动模板

```c
// <your_ip>_driver.h
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint32_t base_addr;
    // ... IP 特定状态 ...
} <your_ip>_dev_t;

void <your_ip>_init(<your_ip>_dev_t *dev, uint32_t base_addr);
// ... set/get/commit API ...
```

```c
// <your_ip>_driver.c
#include "<your_ip>_driver.h"
#include <string.h>

void <your_ip>_init(<your_ip>_dev_t *dev, uint32_t base_addr) {
    memset(dev, 0, sizeof(*dev));
    dev->base_addr = base_addr;
    // ... 设置默认值 ...
}
```

**要点**：
- 文件内不 `#include "vsc_types.h"`
- 所有硬件寄存器访问封装在私有 `static inline` 函数中
- `reg_write()` 在宿主测试时为空实现，在真实硬件时恢复 volatile 写

### 2.3 VSC 适配器模板

```c
// <your_ip>_vsc.c
#include "<your_ip>_vsc.h"
#include "<your_ip>_driver.h"
#include "vsc_prop_ids.h"
#include <string.h>

#define MAX_INSTANCES 4

typedef struct { bool in_use; <your_ip>_dev_t hw; } ctx_t;
static ctx_t g_pool[MAX_INSTANCES];

void <your_ip>_vsc_reset(void) { memset(g_pool, 0, sizeof(g_pool)); }

static int <your_ip>_vsc_init(void **drv_ctx, uint32_t base_addr,
                              const vsc_override_t *overrides, uint8_t num_over)
{
    ctx_t *ctx = NULL;
    for (int i = 0; i < MAX_INSTANCES; i++)
        if (!g_pool[i].in_use) { ctx = &g_pool[i]; ctx->in_use = true; break; }
    if (!ctx) return VSC_ERR_TOPOLOGY_BROKEN;

    <your_ip>_init(&ctx->hw, base_addr);
    // ... 应用 overrides ...
    *drv_ctx = ctx;
    return VSC_OK;
}

// ... try_fmt_sink / try_fmt_source / commit_fmt ...

static const vsc_fmt_transform_desc_t s_template = {
    .type = VSC_TRANSFORM_<YOUR_TYPE>,
    // ... params ...
};

const vsc_driver_t <your_ip>_vsc_driver = {
    .name = "<your_ip>",
    .driver_id = VSC_DRIVER_ID_<YOUR_IP>,
    .capabilities = VSC_CAP_<YOUR_CAP>,
    .transform_template = &s_template,
    .ops = { <your_ip>_vsc_init, <your_ip>_vsc_sink,
             <your_ip>_vsc_source, <your_ip>_vsc_commit },
};
```

### 2.4 注册与使用

```c
// 应用初始化代码
#include "<your_ip>_vsc.h"

vsc_driver_register(&<your_ip>_vsc_driver);

// 在 system_graph.json 中引用
{
    "nodes": [
        {"type": "<your_ip>", "id": "<your_ip>_0", "base": "0x43C00000"}
    ]
}
```

---

## 3. Schema YAML 规范

### 3.1 文件位置

```
drivers/<category>/<driver_name>.schema.yaml
```

### 3.2 格式

```yaml
driver: crop
schema_version: 1
caps: [CROP]              # VSC_CAP_* 字符串
description: "Crop IP"

properties:
  - name: max_width
    index: 0x00            # Driver 内序号 (0x00-0xFE)
    type: u32              # u32/i32/f32/bool/enum/string
    flags: [readonly]      # readonly/runtime/persist/transaction
    default: 8192
    min: 64
    max: 8192
    description: "最大宽度"

  - name: roi.width
    index: 0x04
    type: u32
    flags: [runtime, persist, transaction]
    default: 1920
    max_ref: max_width     # 引用同 Driver 的另一个 Property

transform:
  type: CROP
  params:
    min_w: 64
    max_w_ref: max_width   # 引用 Property 的值
    align_w: 8
```

### 3.3 Driver 注册

```yaml
# drivers/registry.yaml
drivers:
  - id: 0x03
    name: crop
    category: ip
    version: "1.0"
    status: active
    aliases: []             # 历史名称 (向后兼容)
```

### 3.4 ID 生命周期

- Driver_ID 分配后**永不回收**
- 废弃驱动标记 `status: deprecated` + `replaced_by: new_name`
- 重命名使用 `aliases` 保留旧名

---

## 4. 代码生成工作流

```
drivers/*.schema.yaml  ──┐
drivers/registry.yaml  ──┤
                         │
                         ▼
              tools/vsc_prop_gen.py
                         │
                         ▼
              gen/vsc/ (8 文件)
              ├── vsc_prop_ids.h          # ID 宏
              ├── vsc_prop_strings.c      # 名称映射
              ├── vsc_prop_schema.c       # 元数据注册表
              ├── vsc_driver_registry.c   # 驱动注册表 (占位)
              ├── vsc_dependency_map.c    # 依赖图
              ├── vsc_schema_checksum.h   # checksum
              └── vsc_system_init.c       # 初始化数据
```

**CI 规则**：
- Schema YAML 变更 → checksum 变更 → 必须提交更新后的 `gen/vsc/`
- `expected_checksum.txt` 与生成文件不匹配 → CI 失败
- `system_graph.json` 中的 type 不在 registry 中 → CI 失败

---

## 5. ISC 桥接规范

### 5.1 测试环境

```c
// isc_bridge.c — null_read 返回 IMX477 chip ID (0x0477)
// 其他寄存器返回 0 — 传感器驱动可完成 probe 但不会产生副作用
```

### 5.2 真实硬件

替换 `null_read` / `null_write` / `null_fpga_ioctl` 为真实 I2C/FPGA 操作：

```c
static isc_port_t g_port = {
    .bus_type = ISC_BUS_I2C,
    .read     = my_i2c_read,    // ← 替换
    .write    = my_i2c_write,   // ← 替换
    // ...
};
```

### 5.3 sensor_vsc 双模式

- `isc_dev != NULL`：使用真实 `isc_try_fmt()` / `isc_set_fmt()`
- `isc_dev == NULL`：使用内置格式表回退 (RAW8/10/12, 4056×3040)
