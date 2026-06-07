# VSC 视频流控制子系统 — 架构全景复盘

**日期**：2026-06-07
**状态**：P0/P0.5/P1 冻结，P1.1 原型完成
**覆盖**：从原始需求到 Schema Compiler 原型的完整设计与实现

---

## 一、项目背景

### 1.1 物理系统

```
┌─────────────────────────────────────────────────────────┐
│  FPGA + MicroBlaze SoC 工业相机                          │
│                                                         │
│  图像传感器 → LVDS接收器 → Binning → 解码 → 裁剪 → CameraLink输出 │
│                                                         │
│  MicroBlaze (软核CPU)：负责初始化配置 + 参数管理            │
└─────────────────────────────────────────────────────────┘
```

### 1.2 核心需求

1. **传感器多态**：函数虚表(FVT) + 命令ID(CID)实现可替换传感器驱动
2. **链路管理**：V4L2 风格的 Entity/Pad/Link 模型，统一 try_fmt 接口
3. **声明式配置**：静态声明驱动实例 + 连接关系，运行期自动构建
4. **可移植性**：换传感器、换IP、换拓扑——应用代码零修改

### 1.3 关键约束

- **S0 全静态内存**：无 malloc，编译期确定所有缓冲区
- **MicroBlaze 资源限制**：64KB BRAM，100-150MHz，无 Cache
- **FPGA 比特流可变**：IP 模块可能因 FPGA 配置不同而存在/不存在

---

## 二、架构演进路线

### 第一阶段：概念建模（讨论）

从"简化版 V4L2"出发，经过多轮架构评审，核心设计发生了以下关键转向：

| 转向 | 从 | 到 | 触发原因 |
|------|----|----|---------|
| Link 语义 | 单一 vsc_link_t | STREAM / TAP 两种类型 | 统计模块(AE/AWB/Histogram)不参与数据流，不应阻塞主链路 |
| Entity 分类 | 所有模块平等 | SENSOR/STREAM/ANALYZER/ENDPOINT | ANALYZER 无 set_fmt，不参与格式协商 |
| Pipeline 拓扑 | entity[] 线性数组 | Link 集合构成有向图 + Kahn 拓扑排序 | 多分支(ISP→CL_Out + DDR_Writer)无法用线性序列表达 |
| 控制闭环 | AE/AWB 作为 CONTROL Link Entity | Algorithm Service 独立于 Pipeline | 控制环的周期与帧率解耦，不是图遍历节点 |
| 拓扑真相源 | 软件配置表声明 Link | Bitstream Descriptor 自动生成 | RTL 和软件重复维护拓扑必然不同步 |
| 能力表达 | 驱动版本号 | Capability(布尔) + Property(数值) | 版本号→能力映射在长期维护中不可靠 |
| 对象模型 | Entity 直接引用 Driver | Driver → Instance → Entity 三层 | 多实例(crop_0/crop_1)共享同一 Driver，但边界不同 |

### 第二阶段：Resolver 设计（P0）

确立了核心计算引擎——Property Resolver 的四阶段流水线：

```
应用调用 vsc_fmt_transaction({w, h, fmt, fps})
        │
        ▼
Phase 1: FEASIBILITY_CHECK     ← transform_desc.inverse() 代数逆推
        │                         "intent 在硬件能力范围内吗？"
        ▼
Phase 2: FORWARD_PROPAGATE     ← BFS 沿 STREAM 链路逐级 try_fmt
        │                         "如果可以，实际能达到什么格式？"
        ▼
Phase 3: CONVERGENCE           ← 多 ENDPOINT 格式汇总 + adjustment_trace
        │                         "各分支最终格式是什么？哪一级改了什么？"
        ▼
Phase 4: COMMIT                ← 沿执行序逐级写硬件寄存器
                                  "确认，写入硬件。"
```

**关键设计决策**：
- Phase 1 是保守近似（宁可误判可行，不可漏判），Phase 2 兜底精确校验
- 单向 source→sink 传播（无 V4L2 的双向协商，适配 FPGA 单向流水线）
- TAP 链路只校验不修改不阻塞

### 第三阶段：ABI 冻结（P0.5）

补全三个缺口后冻结 P0 接口：

| 修正 | 内容 |
|------|------|
| commit_fmt | Entity 增加 `commit_fmt()` 回调，try/commit 闭环 |
| MULTI_STAGE | 新增复合变换类型，支持 ISP 内部多级变换的逆推 |
| 多分支可行性 | 从线性逆推改为 per-ENDPOINT 独立路径逆推 |

### 第四阶段：Schema 工程化（P1）

将 P0 中手工维护的 Property ID、transform_desc、元数据全部工程化：

```
Driver YAML (Schema Template)
        │ 编译期默认值
        ▼
Codegen → transform_template (Driver 级，只读共享)
        │
        ▼
Instance Create → copy template + prop_overrides → resolved transform_desc
        │
        ▼
entity->transform_desc (Resolver 直接消费)
```

**关键设计决策**：
- Property ID = (Driver_ID << 8) | Property_Index，Driver_ID 永不复用
- 代码生成工具确定性输出（固定排序 + CI checksum 校验）
- Override 只收紧不放宽，依赖一致性在 Instance 创建时 fail-fast

### 第五阶段：Compiler 实现（P1.1）

实现了从 YAML 到 6 个 C 生成产物的完整编译器。

---

## 三、核心设计原则

### 3.1 "应用不感知硬件变化"

```
换传感器 → 改 Board Config 的 sensor_name
换 IP     → 换 FPGA 比特流（system_graph.json 自动更新）
换拓扑    → FPGA 工程重新综合（system_graph.json 自动更新）

应用代码 → 零修改
```

实现机制：
- Bitstream Descriptor 是拓扑唯一真相源
- Capability + Property 替代版本号判断
- Pipeline Graph 自适应拓扑变化
- Optional Entity 自动桥接

### 3.2 Graph 只描述像素流

```
Pipeline Graph:
  · STREAM Link → 像素数据流（参与 try_fmt / set_fmt / stream_on）
  · TAP Link    → 旁路观察（只校验不修改不阻塞）

不在 Graph 中：
  · CONTROL Link（已移除）
  · AE / AWB / AF（独立 Algorithm Service）
```

### 3.3 Resolver 是纯计算引擎

Resolver 不读写硬件寄存器（那是 commit 阶段的事），不管理 Property 元数据（那是 Schema 的事），不关心 I2C/SPI/寄存器地址（那是 Driver 的事）。它只做一件事：把用户意图翻译成硬件可达的格式配置。

### 3.4 三层对象模型

```
Driver      (每种 IP 类型一份)    → capability + ops 虚表 + transform_template
Instance    (每个硬件实例一份)    → base_addr + resolved transform_desc + value table
Entity      (图中节点)           → pads[] + entity_class + propagation_state
```

---

## 四、已实现部分

### 4.1 设计文档（4 份）

```
docs/
├── vsc-property-resolver-design.md        # P0    Resolver 总体设计 + 状态机 + 5 例推演
├── vsc-resolver-submodules.md             # P0.5  可行性预检 + 自动桥接 + 多分支收敛
├── vsc-p1-property-schema-design.md       # P1    Schema YAML + Codegen + Driver 注册 + 治理
└── vsc-schema-compiler-contract.md        # P1.1  Compiler 输入/输出/错误/版本/确定性契约
```

### 4.2 P0 Resolver（C 实现）

```
src/vsc/
├── vsc_types.h          # 全部核心类型（~450 行）
│   ├── vsc_mbus_fmt_t          格式描述符
│   ├── vsc_fmt_transform_desc_t 变换描述符（5 种类型）
│   ├── vsc_entity_t            实体（含 commit_fmt）
│   ├── vsc_link_t              连接（STREAM / TAP）
│   ├── vsc_pipeline_t          管线（图 + 拓扑排序 + 邻接表）
│   └── vsc_resolver_result_t   协商结果（含 adjustment_trace）
│
└── vsc_resolver.c       # Resolver 实现（~850 行）
    ├── Phase 1: feasibility_check（per-endpoint 逆推）
    ├── Phase 2: forward_propagate（BFS 正向传播）
    ├── Phase 3: converge（多分支收敛 + trace 构建）
    ├── Phase 4: commit_fmt（逐级写寄存器）
    ├── vsc_pipeline_build（Kahn 拓扑排序）
    └── vsc_pipeline_remove_optional（自动桥接）
```

### 4.3 P0 单元测试（28 个，全部通过）

```
src/test/vsc/test_vsc_resolver.c

Phase 1 — Feasibility (8 tests)
  ✅ 直通链 / Binning链 / 超传感器上限 / 超Crop上限 / 边界 / 格式不匹配 / 奇数宽度 / 空管线

Phase 1.5 — Optional Bridge (5 tests)
  ✅ SISO桥接 / 叶节点移除 / 根节点移除 / TAP消亡 / 跨类型桥接

Phase 2 — Propagation (7 tests)
  ✅ 直通精确匹配 / Binning折半 / Crop钳位 / 多分支 / TAP成功/失败 / Sink拒绝

Phase 3 — Convergence (4 tests)
  ✅ 相同分支 / Trace填充 / TAP失败Partial / 分支隔离

Integration (4 tests)
  ✅ 精确匹配 / 调整后匹配 / 无效Intent / 多分支+TAP
```

### 4.4 P1.1 Schema Compiler（Python 实现）

```
tools/
├── vsc_prop_gen.py               # 主入口（70 行）
└── vsc_compiler/
    ├── ast_nodes.py               # AST 节点（70 行）
    ├── parser.py                  # YAML → AST（115 行）
    ├── checker.py                 # 语义检查（230 行）
    │   ├── E1001-E5001 (18 Fatal)
    │   ├── W1001-W2002 (5 Warning)
    │   └── I0001-I0005 (5 Info)
    ├── depgraph.py                # 依赖图 + Kahn（55 行）
    ├── codegen.py                 # 6 文件生成（355 行）
    └── checksum.py                # SHA256（25 行）

drivers/
├── registry.yaml                  # Driver_ID 注册表
├── fpga/
│   ├── crop.schema.yaml           # Crop 驱动（6 属性 + CROP 变换）
│   └── binning.schema.yaml        # Binning 驱动（3 属性 + BINNING 变换）
└── sensor/  analyzer/             # 待扩展
```

**验证结果**：
- ✅ 两次独立运行产生相同 checksum `0x610740D4`（确定性）
- ✅ 人为注入重复 Property index → `E2001` 正确捕获，无文件产出
- ✅ 6 个生成产物结构正确，`*_ref` 解析为 default 值

---

## 五、计划实现部分

### 5.1 P1.1 剩余工作

| 步骤 | 内容 | 状态 |
|------|------|------|
| Step 1-6 | Compiler 核心实现 | ✅ 完成 |
| — | CMake 集成（`add_custom_command` 自动触发 Codegen） | 待实现 |
| — | CI checksum 校验脚本 | 待实现 |
| — | 更多 Driver 的 Schema YAML 编写 | 待实现 |
| — | 从测试中移除手工 `VSC_PROP_*` 宏，改用生成产物 | 待实现 |

### 5.2 P1.1 → P2 路线图

```
P1.1 Complete
    │
    ├── CMake 集成 → Codegen 作为构建前置步骤
    ├── CI checksum → git diff build/gen/ 为空
    └── Driver 迁移 → crop/binning 测试用例切换到生成产物
          │
          ▼
P2 Feature System
    │
    ├── VSC_FEATURE_AUTO_EXPOSURE 等推导逻辑
    ├── vsc_has_feature() 应用 API
    └── Feature ← Hardware Capability 映射表
          │
          ▼
P2 Algorithm Service
    │
    ├── Statistics Provider 接口（vsc_stat_read）
    ├── AE/AWB/AF 作为独立 task
    └── 通过 param_manager 与 Pipeline 解耦
          │
          ▼
P2 Property 持久化
    │
    ├── Flash 存储后端
    ├── 出厂默认 vs 用户配置分层
    └── 配置迁移（Schema 升级时自动适配）
```

### 5.3 不在当前路线图中的功能

- MULTI_STAGE 的分叉/条件路径（保持线性组合）
- Control Batch 的原子事务（传感器批量写入）
- 统计缓冲区的共享内存布局
- Algorithm Service 的网络远程执行（AE 在 PC/PLC 上）

---

## 六、关键数据流

### 6.1 系统初始化

```
Bitstream Descriptor (system_graph.json)
        │  IP 实例列表 + 连接关系
        ▼
Board Config (board.json)
        │  传感器型号 + I2C 地址
        ▼
vsc_system_init()
        │
        ├── 1. 遍历 nodes[] → vsc_driver_find(type) → Instance 创建
        │      └── copy transform_template + apply prop_overrides
        │      └── 依赖一致性校验 (fail-fast)
        │
        ├── 2. 遍历 links[] → Pipeline Link 创建
        │
        ├── 3. vsc_pipeline_build()
        │      └── Kahn 拓扑排序 → execution_order
        │      └── 分类 TAP observers + ENDPOINTs
        │
        └── 4. Optional entity 自动桥接
```

### 6.2 格式协商

```
应用: vsc_fmt_transaction({1920, 1080, RGB888, 30})
        │
        ▼
Phase 1: feasibility_check()
        │  沿每个 ENDPOINT 逆推到 SENSOR
        │  区间传播: [1920,1920] → [1920,1920] → [3840,3840] → 检查 ≤ 4056 ✓
        │
        ▼
Phase 2: forward_propagate()
        │  BFS: Sensor.try_fmt_source → Binning.try_fmt_sink → Binning.try_fmt_source → ...
        │  Sensor: {4056,3040,RAW10} → Binning: {1920,1080,RAW10} → Decoder: {1920,1080,RGB888}
        │
        ▼
Phase 3: converge()
        │  汇总 CL_Out: {1920,1080,RGB888}, DDR_Writer: {640,480,RGB888}
        │  primary_fmt = {1920,1080,RGB888}, status = ADJUSTED
        │  adjustment_trace: [BINNING.width 3840→1920, DECODER.format RAW10→RGB888]
        │
        ▼
应用审查 → 接受 → Phase 4: commit_fmt()
                        Sensor.commit → Binning.commit → ... → CL_Out.commit
```

### 6.3 Schema 变更流程

```
开发者编辑 crop.schema.yaml
        │
        ▼
CI pre-commit hook:
  ├── check_driver_ids.py → ID 唯一性 + 生命周期
  ├── vsc_prop_gen.py → 重新生成 6 个产物
  ├── checksum diff → 与 expected_checksum.sha256 对比
  └── compatibility report → BREAKING / NON_BREAKING 变更清单
        │
        ▼
开发者审查 compatibility report
  ├── 非破坏性 → 提交
  └── 破坏性 → 填写 compatibility.breaks → 提交
```

---

## 七、风险全景图

| # | 风险 | 等级 | 状态 |
|---|------|------|------|
| 1 | Feasibility ≠ Guaranteed（Phase 1 通过但 Phase 2 拒绝） | 一级 | 设计契约已记录 |
| 2 | MULTI_STAGE 只能表达线性组合 | 一级 | 设计冻结，不扩展 |
| 3 | transform_desc 归属（Driver template vs Instance resolved） | 一级 | P1 架构修正完成 |
| 4 | Schema 演化兼容性（type/range/enum 变更） | 二级 | CI 兼容性报告 |
| 5 | Generated Artifact 漂移（手改生成文件） | 二级 | Checksum 运行时校验 + CI diff |
| 6 | Property 依赖环路（max_ref 成环） | 一级 | Compiler Kahn 拓扑排序，编译期消除 |

---

## 八、当前状态总览

```
P0  Resolver           ██████████  frozen    vsc_types.h + vsc_resolver.c + 28 tests
P0.5 ABI               ██████████  frozen    commit_fmt + MULTI_STAGE + 多分支可行性
P1  Schema Model       ██████████  frozen    YAML 语义 + ID ABI + Driver 注册
P1.1 Compiler Contract ██████████  frozen    输入/输出/错误/版本/确定性
P1.1 Compiler Impl     ████████░░  prototype  6 文件生成 + 错误检测 + 确定性验证
P1.1 CMake/CI 集成     ░░░░░░░░░░  planned
P2  Feature System     ░░░░░░░░░░  planned
P2  Algorithm Service  ░░░░░░░░░░  planned
```

**P1.1 下一步**：将 Compiler 产物接入 CMake 构建链 + CI checksum 校验 + 示例 Driver 迁移到生成产物。
