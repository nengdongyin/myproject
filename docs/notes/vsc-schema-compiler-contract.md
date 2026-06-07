# VSC Schema Compiler Contract

**版本**：1.0.0
**状态**：P1.1 实现前冻结
**依赖**：P1 Property Schema Model（`docs/vsc-p1-property-schema-design.md`）

---

## 零、定位

Schema Compiler 是整个 VSC Driver 生态的基础设施。它的职责是：

```
registry.yaml + *.schema.yaml  →  Compiler  →  生成产物（6 个文件）
```

本文档定义 Compiler 的**输入/输出契约、错误等级、语义检查规则、版本兼容规则**。
Compiler 实现必须严格遵守本契约，不得读取契约之外的输入，不得产生契约之外的输出。

---

## 一、Compiler 输入

### 1.1 输入目录结构

```
drivers/
├── registry.yaml              ← 必须存在，Driver_ID 注册表
├── sensor/
│   ├── imx219.schema.yaml
│   ├── imx296.schema.yaml
│   └── imx477.schema.yaml
├── fpga/
│   ├── binning.schema.yaml
│   ├── crop.schema.yaml
│   ├── decoder.schema.yaml
│   └── scaler.schema.yaml
└── analyzer/
    └── histogram.schema.yaml
```

### 1.2 输入文件列表

| 文件 | 必填 | 格式 | 说明 |
|------|------|------|------|
| `drivers/registry.yaml` | ✅ | YAML | Driver_ID 注册表，定义所有已知 Driver |
| `drivers/**/*.schema.yaml` | ✅ | YAML | 每个 Driver 的 Property Schema + Transform 定义 |

### 1.3 确定性约束

> Compiler **只能**读取上述 YAML 文件。禁止读取：
> - 系统环境变量
> - 系统时钟（时间戳使用 YAML 文件的 git commit date）
> - 文件系统路径顺序（排序规则见 §1.4）
> - 任何非确定性数据源

### 1.4 处理顺序（确定性）

1. 首先读取 `drivers/registry.yaml`，按文件中 `drivers` 数组的声明顺序依次处理
2. 对每个 Driver，在 `drivers/` 目录树下查找 `<name>.schema.yaml`（精确匹配，不使用文件系统遍历顺序）
3. 每个 `.schema.yaml` 内的 `properties` 按 `index` 升序处理
4. `MULTI_STAGE` 的 `stages` 按 YAML 数组中的书写顺序处理

### 1.5 Driver 发现规则

```
对于 registry.yaml 中 status=active 的每个 Driver:
    查找路径: drivers/**/<name>.schema.yaml
    未找到 → Fatal E0001 "Schema file not found for driver '<name>'"

对于 status=deprecated 的 Driver:
    可以没有对应的 .schema.yaml（已从构建中移除）
    但仍需在 registry.yaml 中保留条目（ID 生命周期）
```

---

## 二、Compiler 输出

### 2.1 输出目录

```
build/gen/vsc/
├── vsc_prop_ids.h           ← Property ID 宏定义
├── vsc_prop_strings.c       ← name → ID 查找表
├── vsc_prop_schema.c        ← vsc_prop_meta_t[] 注册表 + transform_desc 初始化
├── vsc_driver_registry.c    ← vsc_driver_t[] 注册表
├── vsc_dependency_map.c     ← max_ref/min_ref 依赖图
└── vsc_schema_checksum.h    ← 编译期 checksum 常量
```

### 2.2 各产物职责边界

| 产物 | 包含 | 不包含 |
|------|------|--------|
| `vsc_prop_ids.h` | `#define VSC_PROP_*` 宏 + `VSC_DRIVER_ID_*` 宏 | 不包含任何运行时代码 |
| `vsc_prop_strings.c` | `const char*` 数组：ID → 字符串映射 | 不包含 schema 元数据 |
| `vsc_prop_schema.c` | `vsc_prop_meta_t[]` 数组 + `vsc_fmt_transform_desc_t` 初始化 | 不包含 Driver 注册信息 |
| `vsc_driver_registry.c` | `vsc_driver_t[]` 全局注册表 + `vsc_driver_registry_dump()` | 不包含 Property 元数据 |
| `vsc_dependency_map.c` | `vsc_prop_dep_t[]` 依赖表 | 不包含 transform 信息 |
| `vsc_schema_checksum.h` | `#define VSC_SCHEMA_CHECKSUM 0xXXXXXXXX` | 不包含其他内容 |

**职责漂移红线**：禁止在一个产物中混入另一个产物的职责。例如 `vsc_prop_schema.c` 不得包含 Driver 的 `ops` 函数指针赋值（那是 Driver 的 `.c` 文件的职责）。

### 2.3 生成文件头部

每个生成文件必须包含以下头部注释：

```c
/* ═══════════════════════════════════════════════════════════════════════
 *  GENERATED FILE — DO NOT EDIT
 *
 *  Compiler:  vsc_prop_gen.py  v<COMPILER_VERSION>
 *  Inputs:
 *    drivers/registry.yaml                    (git: <commit_hash>)
 *    drivers/fpga/crop.schema.yaml            (git: <commit_hash>)
 *    ... (所有输入文件列表)
 *
 *  Schema Checksum: 0x<CHECKSUM>
 *  Generated:      <ISO_8601_UTC> (git commit date of registry.yaml)
 * ═══════════════════════════════════════════════════════════════════════ */
```

---

## 三、Compiler 错误等级

### 3.1 Fatal（E 类）— 停止生成

产生 Fatal 错误时，Compiler **不产出任何文件**（或产出空文件，由 CI 检测）。

| 错误码 | 触发条件 | 示例 |
|--------|---------|------|
| `E0001` | 必读文件不存在 | `registry.yaml` 缺失；active Driver 的 `.schema.yaml` 缺失 |
| `E0002` | YAML 语法错误 | 无法解析为合法 YAML |
| `E1001` | Driver_ID 重复 | 两个 Driver 都声明 `id: 0x03` |
| `E1002` | Driver name 重复 | 两个 Driver 都声明 `name: crop` |
| `E1003` | Driver_ID 超出范围 | `id: 0x00` 或 `id: 0xFF` |
| `E1004` | status=deprecated 的 ID 被重新分配 | 新 Driver 使用已废弃的 Driver_ID |
| `E2001` | Property index 在 Driver 内重复 | 同一 Driver 内两个 Property 都声明 `index: 0x04` |
| `E2002` | Property index 超出范围 | `index: 0xFF` 或 `index: 0x100` |
| `E2003` | `max_ref` / `min_ref` 引用的 Property 不存在 | `max_ref: nonexistent_prop` |
| `E2004` | `type` 字段值非法 | `type: uint64`（不在支持的类型列表中） |
| `E2005` | `flags` 包含未知值 | `flags: [readonly, magical]` |
| `E3001` | `transform.type` 值非法 | `type: MAGIC_TRANSFORM` |
| `E3002` | `transform.params` 缺少必填字段 | `CROP` 缺少 `max_w` |
| `E3003` | `MULTI_STAGE` 的 `stages` 嵌套深度超过上限 | 嵌套 > 4 |
| `E4001` | Property 依赖图存在环路 | `A.max_ref→B, B.max_ref→C, C.max_ref→A` |
| `E5001` | alias 全局重复 | 两个不同 Driver 的 aliases 中都包含 `"crop_v1"` |

### 3.2 Warning（W 类）— 允许生成，但发出警告

| 警告码 | 触发条件 | 示例 |
|--------|---------|------|
| `W1001` | status=deprecated 的 Driver 仍被其他 Driver 引用 | histogram v1 的 register 引用已废弃的 ae_engine_legacy |
| `W2001` | Property 超出推荐数量上限 | 单个 Driver 的 Property 数 > 128 |
| `W2002` | `default` 值超出 `min`/`max` 范围 | `default: 0, min: 64` |
| `W3001` | `transform.params` 中的 `*_ref` 值放宽了编译期默认值 | `max_w_ref` 引用的 Property 默认值 > `max_w` 字面值 |
| `W5001` | Bitstream Descriptor 中引用了 deprecated Driver | （此检查在 Bitstream Loader 阶段，不在 Compiler） |

### 3.3 Info（I 类）— 仅报告

```
I0001  Compiler version: vsc_prop_gen.py v1.0.0
I0002  Input: drivers/registry.yaml (24 drivers, 2 deprecated)
I0003  Processed: 22 active drivers, 386 properties
I0004  Checksum: 0x7A3F12B9
I0005  Output: build/gen/vsc/ (5 files, 48 KiB)
```

---

## 四、Compiler 版本兼容规则

### 4.1 `schema_version` 字段

每个 `.schema.yaml` 必须声明 `schema_version`：

```yaml
# drivers/fpga/crop.schema.yaml
driver: crop
schema_version: 1    # ← Schema 格式版本号
```

### 4.2 Compiler 支持的版本范围

Compiler 自己的版本号独立于 Schema 版本号。Compiler 声明其支持的 Schema 版本范围：

```
Compiler v1.0.0:
    supports schema_version: [1, 1]
```

### 4.3 不匹配时的行为

```
Compiler v1.0.0 遇到 schema_version: 2 的 .schema.yaml:
    → Fatal E6001 "Schema version 2 not supported by Compiler v1.0.0 (supports [1])"
    → 不尝试猜测语义
    → 不产生部分输出
```

### 4.4 Compiler 自身版本号

存储在 `tools/vsc_prop_gen.py` 的 `__version__` 变量中，生成文件头部引用。
Compiler 版本号变更规则：

| 变更类型 | 版本号变化 |
|---------|-----------|
| 新增支持的 schema_version | minor +1 |
| 修改输出格式（破坏性） | major +1 |
| Bug 修复（不改变输出） | patch +1 |

---

## 五、Property 依赖图与环路检测

### 5.1 依赖关系来源

| 来源 | 说明 |
|------|------|
| `max_ref` | 当前 Property 的最大值引用目标 Property |
| `min_ref` | 当前 Property 的最小值引用目标 Property |
| `transform.params.*_ref` | transform_desc 中的边界引用 |

### 5.2 依赖图构建

Compiler 在 Semantic Check 阶段构建有向图：

```
节点 = Property ID (全局 16-bit)
边   = (A → B) 表示 A 的 max_ref/min_ref 指向 B
```

### 5.3 环路检测

使用 Kahn 算法进行拓扑排序。如果存在环路：

```
→ Fatal E4001 "Dependency cycle detected"
   输出环路路径：crop.roi.width → crop.max_width → sensor.active_width → crop.roi.width
```

### 5.4 依赖图产物

`vsc_dependency_map.c` 输出拓扑排序后的依赖顺序（用于 Instance 创建时的 fail-fast 校验）：

```c
/* 拓扑排序后的依赖计算顺序：被依赖的 Property 先求值 */
static const vsc_prop_dep_t _global_dependencies[] = {
    { VSC_PROP_CROP_ROI_WIDTH,  VSC_PROP_CROP_MAX_WIDTH  },
    { VSC_PROP_CROP_ROI_HEIGHT, VSC_PROP_CROP_MAX_HEIGHT },
    /* ... */
};
```

---

## 六、P1.1 实现顺序

| 步骤 | 内容 | 输入 | 输出 | 验证 |
|------|------|------|------|------|
| **Step 1** | 冻结本契约 | — | `docs/vsc-schema-compiler-contract.md` | 团队评审通过 |
| **Step 2** | YAML Parser | `registry.yaml` + `*.schema.yaml` | 内存 AST（DriverAST, PropertyAST, TransformAST） | 单元测试：合法/非法 YAML 输入 |
| **Step 3** | Semantic Checker | AST | 错误列表（E/W/I）+ 校验通过的 AST | 单元测试：每种错误码至少一个用例 |
| **Step 4** | Dependency Graph Builder | 校验通过的 AST | 拓扑排序后的依赖图 | 单元测试：无环/有环/复杂链 |
| **Step 5** | Code Generator | 校验通过的 AST + 依赖图 | 6 个生成文件 | CI checksum 一致性 |
| **Step 6** | Checksum & CI | 生成文件 | `VSC_SCHEMA_CHECKSUM` + CI diff 校验 | `git diff build/gen/` 为空 |

---

## 七、风险 6：Property 依赖环路（已纳入契约）

**风险**：`max_ref` / `min_ref` 的引用链可能形成环路，导致 Instance 创建时的 resolve 步骤死循环。

**等级**：一级（编译期可检测）

**检测位置**：Compiler Step 4（Dependency Graph Builder）

**检测方法**：Kahn 拓扑排序。若存在节点未输出（in_degree > 0），则存在环路。

**错误输出**：
```
E4001: Dependency cycle detected among properties:
  crop.roi.width (0x0304) → crop.max_width (0x0300)
  crop.max_width (0x0300) → sensor.active_width (0x0105)
  sensor.active_width (0x0105) → crop.roi.width (0x0304)
```

**缓解**：在 Compiler 阶段 fail-fast，不产出生成文件，阻止环路进入运行期。

---

## 八、Contract Freeze 声明

本契约一旦冻结，Compiler 实现必须严格遵守：

- **输入边界**：只读取 `registry.yaml` + `*.schema.yaml`，不访问任何外部数据源
- **输出边界**：只产出 6 个指定文件，不创建额外产物
- **错误边界**：E 类错误不产出文件，W 类允许产出但 CI 黄牌，I 类纯信息
- **版本边界**：schema_version 不匹配时 Fatal，不尝试兼容猜测
- **确定性边界**：同一组输入在任何环境下产出字节级完全相同的输出
