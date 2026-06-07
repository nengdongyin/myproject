# VSC Property Resolver — 核心设计文档

**版本**：1.0.0
**定位**：VSC 框架 P0 核心组件
**前置依赖**：
- Pipeline Graph（STREAM/TAP 链路 + 拓扑排序结果）
- Property Schema（Driver transform 函数）
- Instance Value Table

---

## 零、Property Resolver 在整个 VSC 中的位置

```
应用层
  │ vsc_fmt_transaction({w, h, fmt, fps})
  ▼
Property Resolver  ← 本文档
  │
  ├── 读取: Pipeline Graph（拓扑排序执行序）
  ├── 调用: 每个 STREAM entity 的 Driver transform 函数
  ├── 写入: Instance Value Table（最终协商结果）
  │
  ▼
Driver transform 函数
  │ 纯粹的格式计算，不写硬件寄存器
  │ commit 阶段才写寄存器
  ▼
FPGA 硬件寄存器（commit 阶段）
```

**Property Resolver 的职责边界**：
- ✅ 把应用意图翻译为硬件可达的格式配置
- ✅ 沿 STREAM 链路逐级传播格式
- ✅ 协调多分支、TAP、Optional entity
- ❌ 不写任何硬件寄存器（那是 commit 阶段的事）
- ❌ 不管理 Property 的 type/flags/validator（那是 Schema 的事）
- ❌ 不关心 I2C/SPI/寄存器地址（那是 Driver 的事）

---

## 一、Driver Transform 函数接口

Resolver 不直接操作硬件，而是调用每个 Driver 提供的纯函数。

### 1.1 接口定义

每个 Driver 必须实现以下函数（作为 `vsc_ip_ops_t` 虚表的一部分）：

#### try_fmt_sink(drv_ctx, pad_idx, proposed_fmt) → clamped_fmt

语义：给定一个进入此 entity sink pad 的格式提议，返回此 entity 能够接受的最接近格式。

规则：
- 只能收紧约束（缩小范围、去掉不支持的格式），不能放宽
- 如果 proposed 完全在能力范围内，原样返回
- 如果完全不支持，返回错误码
- 不修改硬件寄存器
- 幂等：try_fmt_sink(try_fmt_sink(f)) == try_fmt_sink(f)

#### try_fmt_source(drv_ctx, pad_idx, sink_fmt) → source_fmt

语义：给定此 entity 的 sink pad 接受的格式，计算此 entity 的 source pad 将会输出的格式。

规则：
- Binning entity: source = sink / factor
- Decoder entity: source = sink（尺寸不变，fmt 变）
- Pass-through entity: source = sink（完全不改）
- Crop entity: source = clamp(sink, crop_roi)
- 如果当前 entity 没有 source pad（ENDPOINT），不实现此函数
- 不修改硬件寄存器

#### commit_fmt(drv_ctx, pad_idx, final_fmt) → error_code

语义：将最终协商好的格式写入硬件寄存器。仅在 Pipeline Transaction 的 commit 阶段调用。

规则：
- 必须验证 final_fmt 仍在能力范围内（防御性检查）
- 写失败返回错误码
- 写成功后更新 Instance Value Table

### 1.2 Transform 函数的代数性质（用于可行性预检）

每个 Driver 额外声明一个轻量的格式变换描述符，用于 Phase 1 的快速逆推：

```
vsc_fmt_transform_desc_t {
    vsc_fmt_transform_type_t type;

    union {
        struct { uint8_t factor_x; uint8_t factor_y; } binning;
        struct { uint32_t min_w; uint32_t min_h;
                 uint32_t max_w; uint32_t max_h;
                 uint8_t  align_w; uint8_t align_h; } crop;
        struct { vsc_pixel_format_t fmt_in;
                 vsc_pixel_format_t fmt_out; } pixel_fmt_conv;
    };
};
```

Transform 类型：

| Type | 正向变换 | 逆变换（用于可行性预检） | 示例 |
|------|---------|------------------------|------|
| PASS_THROUGH | out = in | in = out | LVDS_RX |
| BINNING | out = in / factor | in = out × factor | Binning 2×2 |
| CROP | out = clamp(in, [min, max]) | in ≥ out（下限约束） | Crop |
| PIXEL_FMT_CONV | out.fmt = new_fmt, out.size = in.size | in.size = out.size, in.fmt = old_fmt | Decoder |
| MULTI_STAGE | 多个子 transform 串联 | 逆序逆推 | ISP（内部含多个模块） |

关键：try_fmt_sink / try_fmt_source 是精确的（返回确切值），transform_desc 是近似的（返回可达范围）。可行性预检只用近似描述做快速判断，不替代精确的 try_fmt 传播。

---

## 二、Property Resolver 完整状态机

```
                    应用调用
                  vsc_fmt_transaction(intent)
                        │
                        ▼
         ┌──────────────────────────────┐
         │  STATE: VALIDATE              │
         │  校验 intent 合法性           │
         │  失败 → VSC_ERR_INVALID_INTENT│
         └──────────┬───────────────────┘
                    │ OK
                    ▼
         ┌──────────────────────────────┐
         │  STATE: FEASIBILITY_CHECK     │
         │  沿 STREAM 链逆推             │
         │  失败 → VSC_ERR_UNREACHABLE   │
         └──────────┬───────────────────┘
                    │ feasible
                    ▼
         ┌──────────────────────────────┐
         │  STATE: FORWARD_PROPAGATE     │
         │  BFS 沿 STREAM 链路正向传播   │
         │  失败 → VSC_ERR_PROPAGATION   │
         └──────────┬───────────────────┘
                    │ success
                    ▼
         ┌──────────────────────────────┐
         │  STATE: CONVERGENCE           │
         │  汇总所有 ENDPOINT 的最终格式  │
         │  计算 adjustment_trace        │
         └──────────┬───────────────────┘
                    │
                    ▼
         ┌──────────────────────────────┐
         │  STATE: RETURN_RESULT         │
         │  返回 final_fmt + trace       │
         │  应用决定是否接受              │
         └──────────┬───────────────────┘
                    │ 应用接受
                    ▼
         ┌──────────────────────────────┐
         │  STATE: COMMIT                │
         │  逐级写硬件寄存器              │
         │  失败标记 PIPELINE_DIRTY      │
         └──────────────────────────────┘
```

---

## 三、核心算法：正向传播的 BFS 实现

### 3.1 数据结构

```
pipeline = {
    execution_order[]   // STREAM entity 索引的有序列表（拓扑排序结果）
    tap_observers[]     // TAP entity 索引列表
    endpoints[]         // ENDPOINT entity 索引列表
    adjacency[]         // 邻接表：adjacency[src_entity] = [(dst_entity, link_type)]
}

entity.propagation_state = {
    sink_fmt           // 此 entity sink pad 当前看到的格式
    source_fmt         // 此 entity source pad 计算出的格式
    visited            // BFS 标记
    active             // 是否成功参与传播
}
```

### 3.2 正向传播伪代码

```
function forward_propagate(pipeline, intent_fmt):
    // 初始化
    for each entity in pipeline.all_entities:
        entity.propagation_state.visited = false
        entity.propagation_state.active  = true
        entity.propagation_state.sink_fmt = INVALID_FMT

    // 找到所有 SENSOR entity 作为起点
    root_entities = [e for e in pipeline.execution_order if e.class == SENSOR]
    if root_entities is empty:
        return VSC_ERR_NO_SENSOR

    // 设置起点的 source_fmt
    for each root in root_entities:
        root.propagation_state.source_fmt =
            root.driver.try_fmt_source(drv_ctx, SOURCE_PAD, intent_fmt)
        root.propagation_state.visited = true

    // BFS 主循环
    queue = root_entities.copy()
    
    while queue is not empty:
        entity = queue.pop_front()

        if entity.propagation_state.source_fmt is INVALID:
            mark_downstream_failed(entity, pipeline)
            continue

        out_fmt = entity.propagation_state.source_fmt

        for each (dst_entity, link_type) in pipeline.adjacency[entity]:

            if link_type == STREAM:
                // Step A: 传入下游 sink pad
                dst_entity.propagation_state.sink_fmt = out_fmt

                // Step B: 下游 sink pad 校验
                clamped = dst_entity.driver.try_fmt_sink(drv_ctx, SINK_PAD, out_fmt)
                if clamped is INVALID:
                    dst_entity.propagation_state.active = false
                    mark_downstream_failed(dst_entity, pipeline)
                    log_adjustment(entity, dst_entity, "SINK REJECTED")
                    continue

                dst_entity.propagation_state.sink_fmt = clamped

                // Step C: 如果下游有 source pad，计算其输出
                if dst_entity.class in {SENSOR, STREAM}:
                    out_fmt_for_next = dst_entity.driver.try_fmt_source(
                        drv_ctx, SOURCE_PAD, clamped)
                    if out_fmt_for_next is INVALID:
                        dst_entity.propagation_state.active = false
                        mark_downstream_failed(dst_entity, pipeline)
                        continue
                    dst_entity.propagation_state.source_fmt = out_fmt_for_next

                // 加入队列继续传播
                if dst_entity not visited:
                    dst_entity.propagation_state.visited = true
                    queue.push_back(dst_entity)

            elif link_type == TAP:
                // 只做校验，不修改格式，不加入传播队列
                rc = dst_entity.driver.try_fmt_sink(drv_ctx, SINK_PAD, out_fmt)
                if rc == VSC_OK:
                    dst_entity.propagation_state.sink_fmt = out_fmt
                    dst_entity.propagation_state.active = true
                else:
                    dst_entity.propagation_state.active = false
                    log_tap_inactive(dst_entity, rc)

    // 检查所有必选 STREAM entity 是否都已 visited
    for each entity in pipeline.execution_order:
        if entity.class in {STREAM, ENDPOINT} and not entity.propagation_state.visited:
            return VSC_ERR_TOPOLOGY_BROKEN

    return converge_endpoints(pipeline)
```

### 3.3 多分支收敛算法

```
function converge_endpoints(pipeline):
    endpoint_fmts = []
    for each endpoint in pipeline.endpoints:
        if endpoint.propagation_state.active:
            endpoint_fmts.append(endpoint.propagation_state.sink_fmt)

    if endpoint_fmts is empty:
        return VSC_ERR_NO_ACTIVE_ENDPOINT

    primary_fmt = endpoint_fmts[0]
    adjustment_trace = build_trace(pipeline)

    return {
        .primary_fmt       = primary_fmt,
        .endpoint_fmts     = endpoint_fmts,
        .adjustment_trace  = adjustment_trace,
        .status            = determine_status(primary_fmt, intent_fmt)
    }
```

---

## 四、错误处理与 Optional Entity

### 4.1 错误分类

| 错误码 | 阶段 | 含义 | Resolver 行为 |
|--------|------|------|--------------|
| VSC_ERR_INVALID_INTENT | VALIDATE | intent 参数非法 | 直接返回，不进入传播 |
| VSC_ERR_UNREACHABLE | FEASIBILITY_CHECK | 逆推后发现超出硬件能力 | 返回 + 可达范围提示 |
| VSC_ERR_PROPAGATION_SINK | FORWARD_PROPAGATE | STREAM entity sink pad 拒绝格式 | 标记 INACTIVE，继续传播 |
| VSC_ERR_PROPAGATION_SOURCE | FORWARD_PROPAGATE | STREAM entity source pad 无法输出 | 标记该 entity 及下游 INACTIVE |
| VSC_ERR_TOPOLOGY_BROKEN | FORWARD_PROPAGATE | 必选 entity 不可达 | 整体失败 |
| VSC_WARN_TAP_INACTIVE | FORWARD_PROPAGATE | TAP entity 无法处理格式 | 标记 TAP INACTIVE，不阻塞主链路 |
| VSC_ERR_COMMIT_FAILED | COMMIT | 写硬件寄存器失败 | 标记 PIPELINE_DIRTY，不回滚 |

### 4.2 Optional Entity 处理

Optional entity 在 Bitstream Descriptor 中标记：
```json
{ "type": "scaler", "id": "scaler_0", "base": "0x43C60000", "optional": true }
```

vsc_system_init 时：
- 如果 optional entity 的 driver 不存在 → 跳过 entity 创建
- 自动桥接：移除 entity 后，其上游 source pad 和下游 sink pad 之间的 STREAM link 自动重建
- 拓扑重排序：移除节点后的图重新做拓扑排序

正向传播时：
- Optional entity 如果 active=false，BFS 直接跳过，相当于该 entity 不存在

---

## 五、adjustment_trace 设计

```
vsc_adjustment_trace_t {
    uint8_t num_entries;
    vsc_adjustment_entry_t entries[VSC_MAX_TRACE_ENTRIES];
};

vsc_adjustment_entry_t {
    char     entity_name[32];
    char     pad_name[8];          // "SOURCE", "SINK"
    vsc_fmt_field_t field_changed; // WIDTH / HEIGHT / FORMAT / FRAMERATE / NONE
    vsc_fmt_val_t  original;
    vsc_fmt_val_t  adjusted;
    vsc_adjust_reason_t reason;    // CLAMP / HALVE / FORMAT_CONV / REJECTED
    char     reason_detail[64];
};
```

---

## 六、Commit 阶段

```
function commit_fmt(pipeline, final_fmt):
    if pipeline.state == VSC_PIPELINE_STREAMING:
        return VSC_ERR_BUSY

    for each entity in pipeline.execution_order:
        rc = entity.driver.commit_fmt(drv_ctx, final_fmt)
        if rc != VSC_OK:
            pipeline.state = VSC_PIPELINE_DIRTY
            return VSC_ERR_COMMIT_FAILED(entity=entity.name, code=rc)

        entity.instance.value_table.update(entity.propagation_state)

    pipeline.state = VSC_PIPELINE_CONFIGURED
    return VSC_OK
```

---

## 七、五个具体例子推演

### 例 1：基本链路，格式完全匹配
### 例 2：分辨率被 Crop 钳位
### 例 3：多分支 + TAP
### 例 4：TAP 无法处理格式
### 例 5：Optional Entity 缺失

（详见主文档对应章节）

---

## 八、Resolver 设计原则

| 原则 | 说明 |
|------|------|
| 单向无回溯 | 格式沿 source→sink 传播，不反向修改上游 |
| 幂等性 | try(try(f)) == try(f) |
| TAP 隔离 | TAP 不参与传播、不阻塞主链路、不修改格式 |
| 分支独立 | 多分支各自独立传播，在 ENDPOINT 处收敛 |
| Optional 透明 | Optional entity 缺失时自动桥接 link |
| 硬件延迟到 commit | try 阶段零硬件写入 |
| 失败不阻塞 | TAP 失败不影响 STREAM，部分 entity 失败不影响其他分支 |
| 完整可追溯 | adjustment_trace 记录每一步的格式变化及原因 |

---

## 九、已知风险与技术债（P0.5 freeze 记录）

### 风险 1：Feasibility 与 Propagation 是"两套世界"

Phase 1（feasibility）使用 `transform_desc.inverse()` — 纯代数逆推，保守近似。
Phase 2（propagation）使用 `try_fmt_sink()` / `try_fmt_source()` — Driver 真实能力。

两者数据源不同，因此可能出现：

```
Phase 1: FEASIBLE（区间检查通过）
Phase 2: REJECTED（Driver 精确校验失败，如对齐约束）
```

**这是设计允许的，不是 bug。** `FEASIBLE ≠ GUARANTEED_SUCCESS`。Phase 1 的语义
是"存在潜在可达路径，值得尝试 Phase 2"，而非"保证 Phase 2 一定成功"。

Phase 1 采用保守近似的原因：
- 区间传播是 O(N) 的代数运算，适合快速否决明显不可达的 intent
- 精确校验需要 Driver 回调，只能放在 Phase 2
- 宁可 Phase 1 误判为可行（Phase 2 兜底），不可 Phase 1 漏判导致应用放弃合法配置

**Driver 作者须知**：`transform_desc` 和 `try_fmt_xxx()` 不需要严格等价。
`transform_desc` 应描述硬边界（max_w, max_h, format 转换方向），可以比实际能力宽松。
对齐约束、帧率精细限制等放在 `try_fmt_xxx()` 中处理。

### 风险 2：MULTI_STAGE 只能表达线性组合

当前 `VSC_TRANSFORM_MULTI_STAGE` 的 `subs[]` 数组表达的是 A→B→C 线性级联。
它**不能**表达：
- 内部分叉（ISP → Crop 和 ISP → Scaler 并存）
- 条件路径（RAW 模式 bypass decoder，RGB 模式 enable decoder）

**这是有意为之。** `transform_desc` 的定位是 Phase 1 可行性近似，不是迷你 Graph。
如果持续扩展 MULTI_STAGE 的表达能力，会逐渐把 Pipeline Graph 复制到
`transform_desc` 内部，造成双重拓扑维护。

**处理策略**：复杂 ISP 有两种建模方式：
1. 拆成多个独立 Entity（Binning、Decoder、Crop 各自一个），让 Pipeline Graph 承载拓扑
2. 用一个 MULTI_STAGE 描述线性简化模型，Phase 2 由 Driver 内部处理分叉/条件

**P0 freeze 决定**：MULTI_STAGE 保持线性组合语义，不扩展为 mini-graph。
若未来确实需要条件路径表达能力，通过 Property System 的 Feature 机制解决
（根据 `vsc_has_feature(VSC_FEATURE_RAW_BYPASS)` 选择不同的 transform_desc）。
