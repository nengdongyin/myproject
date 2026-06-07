# VSC Property Resolver — 子模块深度设计

**版本**：1.0.0
**前置文档**：`docs/vsc-property-resolver-design.md`
**范围**：可行性预检代数逆推 + Optional Entity 自动桥接 + 多分支收敛

---

## 子模块 1：可行性预检 — 代数逆推算法

### 1.1 问题定义

在完整的 try_fmt 传播（BFS 遍历全图 + 每个 entity 调用 Driver 函数）之前，需要快速回答一个问题：

> **给定 intent_fmt，是否存在至少一条硬件配置路径可以达到？**

如果不能，尽早返回错误和可达范围，避免浪费 BFS 遍历，也让应用能快速迭代参数。

### 1.2 核心思想：区间约束 + 反向传播

不用精确值（那是 try_fmt 的事），用**区间约束**做保守估计。从输出端向输入端逐级逆推，每一步将约束放宽到"能产生当前约束区间的输入集合"。

```
正向（精确）：  in ──[entity]──→ out = f(in)           （try_fmt 做这个）
逆向（区间）：  [lo, hi] ──[entity⁻¹]──→ wider [lo', hi']  （feasibility 做这个）
```

关键是逆向函数对每种 transform 类型的定义。

### 1.3 Transform 逆向函数定义

以宽度（width）为例，高度和帧率同理。像素格式单独处理。

#### PASS_THROUGH（LVDS_RX 等透明模块）

```
正向: out = in
逆向: in ∈ [lo, hi]
```
完全不变。

#### BINNING（2×2, 4×4 等）

```
正向: out = in / factor，且 in % factor == 0
逆向: in ∈ [lo × factor, hi × factor]
附加: lo × factor 必须能被 factor 整除（如果不能，向上取整到最近的 factor 倍数）
```

**示例**：Binning 2×2，输出区间 [1920, 1920] → 输入区间 [3840, 3840]。
输出区间 [1919, 1920] → 输入区间 [3838, 3840] → 取整 [3840, 3840]（3838 向上取整到 3840）。

#### CROP

```
正向: out = clamp(in, crop_min, crop_max)，对齐到 align
逆向分析:
  要产生输出 o，裁剪器的输入必须满足:
    - 如果 crop_min < o < crop_max: in 至少为 o（输入可以更大，裁剪会截断）
    - 如果 o == crop_min: in 可以是 [1, crop_min] 中的任意值（都会被 clamp 到 crop_min）
    - 如果 o == crop_max: in 可以是 [crop_max, ∞) 中的任意值（都会被 clamp 到 crop_max）

对区间 [lo, hi]:
  - 如果 lo < crop_min: 输入下界 = lo（更小的输入会被 clamp 到 crop_min，仍满足 ≥ lo）
  - 如果 lo ≥ crop_min: 输入下界 = lo
  - 输入上界 = crop_max（任何大于 crop_max 的输入都 clamp 到 crop_max，不能产生 > crop_max 的输出）

保守简化（便于实现）:
  in_lo = lo               // 至少需要 lo（除非 lo < crop_min，此时取 lo）
  in_hi = crop_max         // 不能超过硬件上限
  in_lo = max(in_lo, crop_min) 对齐到 crop_min（如果 lo < crop_min，输入下界至少 crop_min... 
  
  等等，让我重新想。
  
  如果目标输出是 o = 100，crop_min = 64, crop_max = 1920。
  输入可以是 100（恰好匹配），也可以是 200（裁剪到 1920 以内... 200 < 1920 所以输出 200）。
  实际上：如果输入是 200，输出就是 200，不是 100。所以输入必须恰好是 100 才能输出 100。
  
  修正理解：
  正向: 如果 in < crop_min → out = crop_min
        如果 in > crop_max → out = crop_max  
        否则 out = in (after alignment)
  
  所以 crop 只有在边界处才是"多对一"的。在区间内部是"一对一"的。
  
  严格逆向:
  对于输出 o ∈ (crop_min, crop_max): in = o（唯一）
  对于输出 o = crop_min: in ∈ [1, crop_min]
  对于输出 o = crop_max: in ∈ [crop_max, ∞)
  
  对于区间 [lo, hi]:
  - 区间与 (crop_min, crop_max) 的交集 → 每个点对应唯一点的输入
  - 区间包含 crop_min → 输入下界扩展到 1
  - 区间包含 crop_max → 输入上界扩展到 ∞（实际取 sensor_max）
  
  保守实现（区间表示，允许 false positive，禁止 false negative）:
  
  输入下界 in_lo:
    if lo <= crop_min: in_lo = 1
    else: in_lo = lo
  
  输入上界 in_hi:
    if hi >= crop_max: in_hi = SENSOR_MAX（或无限）
    else: in_hi = hi
```

**示例**：Crop max_w=1920, min_w=64, align=8。目标输出 [1920, 1920]。
- 1920 == crop_max → 输入上界 → ∞ → 取 sensor max (4056)
- 1920 > crop_min → 输入下界 = 1920
- 逆向结果: in ∈ [1920, 4056]

目标输出 [100, 200]（crop_max=1920, crop_min=64）：
- 100 > 64 → in_lo = 100
- 200 < 1920 → in_hi = 200
- 逆向结果: in ∈ [100, 200]

目标输出 [50, 200]（包含了 crop_min=64）：
- 50 ≤ 64 → in_lo = 1
- 200 < 1920 → in_hi = 200
- 逆向结果: in ∈ [1, 200]

#### PIXEL_FMT_CONV（解码器、色彩空间转换）

```
正向: out.fmt = target_fmt, out.size = in.size
逆向: in.fmt = source_fmt, in.size = out.size

如果输出要求 RGB888，而解码器只能从 RAW10 转换:
  逆向后 in.fmt = RAW10
  如果传感器不支持 RAW10 → 不可达
```

**像素格式的区间表示**：用位掩码集合 `fmt_mask`。逆向时，如果当前约束的 fmt 集合与转换器的 target_fmt 有交集，则将 source_fmt 加入输入约束集合。

### 1.4 完整预检算法

```
function feasibility_check(pipeline, intent_fmt):

    // ── 初始化约束 ──
    constraint = {
        w:  { lo: intent_fmt.width,  hi: intent_fmt.width  },
        h:  { lo: intent_fmt.height, hi: intent_fmt.height },
        fmt_mask: pixel_format_to_mask(intent_fmt.pixel_format),
        fps:{ lo: intent_fmt.frame_rate, hi: intent_fmt.frame_rate }
    }
    
    // ── 逆序遍历 STREAM entity ──
    for each entity in REVERSE(pipeline.execution_order):
        
        if entity.class == SENSOR:
            break   // 到达传感器，停止逆推
        
        if not entity.propagation_state.active:
            continue  // 跳过 inactive entity（optional 缺失）
        
        // 获取此 entity 的 transform_desc
        desc = entity.driver.transform_desc
        
        // 对每个维度应用逆向变换
        constraint = desc.inverse(constraint)
        
        // ── 空约束检测 ──
        if constraint.w.lo > constraint.w.hi:
            return { feasible: false, 
                     reason: "width constraint empty",
                     detail: "required [" + constraint.w.lo + "," + 
                             constraint.w.hi + "]" }
        if constraint.h.lo > constraint.h.hi:
            return { feasible: false,
                     reason: "height constraint empty" }
        if constraint.fmt_mask == 0:
            return { feasible: false,
                     reason: "no supported pixel format" }
    
    // ── 逆推完成，检查传感器能力 ──
    sensor = pipeline.get_sensor_entity()
    sensor_caps = sensor.driver.get_capabilities(drv_ctx)
    
    // 检查宽度
    if constraint.w.lo > sensor_caps.max_width or 
       constraint.w.hi < sensor_caps.min_width:
        return {
            feasible: false,
            reason: "width out of sensor range",
            sensor_range: { min: sensor_caps.min_width, 
                            max: sensor_caps.max_width },
            required: constraint.w
        }
    
    // 检查高度
    if constraint.h.lo > sensor_caps.max_height or
       constraint.h.hi < sensor_caps.min_height:
        return {
            feasible: false,
            reason: "height out of sensor range",
            sensor_range: { min: sensor_caps.min_height,
                            max: sensor_caps.max_height },
            required: constraint.h
        }
    
    // 检查像素格式
    if (constraint.fmt_mask & sensor_caps.supported_fmts_mask) == 0:
        return {
            feasible: false,
            reason: "pixel format not supported by sensor",
            sensor_fmts: sensor_caps.supported_fmts_mask,
            required_fmt: constraint.fmt_mask
        }
    
    // ── 检查帧率 ──
    // FPS 约束只对 sensor 有意义（sensor 决定帧率）
    if constraint.fps.lo > sensor_caps.max_fps or
       constraint.fps.hi < sensor_caps.min_fps:
        return {
            feasible: false,
            reason: "frame rate out of sensor range",
            sensor_fps_range: { min: sensor_caps.min_fps,
                                max: sensor_caps.max_fps },
            required: constraint.fps
        }
    
    // ── 通过 ──
    return { feasible: true }
```

### 1.5 具体推演

#### 推演 1：基本可达

```
Pipeline: Sensor(IMX477) → Binning(2×2) → Decoder(RAW→RGB) → Crop → CL_Out
Intent:   {1920, 1080, RGB888, 30}

逆推过程:
  Start:  w∈[1920,1920], h∈[1080,1080], fmt=RGB888, fps∈[30,30]

  CL_Out (PASS_THROUGH):
    → w∈[1920,1920], h∈[1080,1080], fmt=RGB888, fps∈[30,30]

  Crop (crop_max_w=1920, crop_max_h=1920, crop_min=64):
    1920 == crop_max → hi → SENSOR_MAX(4056)
    1920 > crop_min → lo = 1920
    → w∈[1920,4056], h∈[1080,3040], fmt=RGB888, fps∈[30,30]

  Decoder (PIXEL_FMT_CONV: RAW10→RGB888):
    fmt 逆向: RGB888 → RAW10
    size 不变
    → w∈[1920,4056], h∈[1080,3040], fmt=RAW10, fps∈[30,30]

  Binning (BINNING, factor=2):
    lo = 1920 × 2 = 3840 (divisible by 2 ✓)
    hi = 4056 × 2 = 8112 → 但 4056 是 sensor max，实际 hi = min(8112, 4056) = 4056
    → w∈[3840,4056], h∈[2160,3040], fmt=RAW10, fps∈[30,30]

  到达 Sensor:
    IMX477: max=4056×3040, RAW10 支持, min_fps=1, max_fps=120
    3840 ≤ 4056 ✓, 4056 ≤ 4056 ✓, 2160 ≤ 3040 ✓, 3040 ≤ 3040 ✓
    RAW10 ✓, 30fps ∈ [1,120] ✓
  → FEASIBLE
```

#### 推演 2：超宽分辨率

```
Intent: {5000, 1080, RGB888, 30}

逆推:
  Start: w∈[5000,5000]

  Crop (max_w=1920):
    5000 > 1920 → lo = 5000, hi = 1920
    → lo > hi → EMPTY
  → VSC_ERR_UNREACHABLE: width 5000 exceeds crop max 1920
```

#### 推演 3：传感器不支持 RAW10

```
Intent: {1920, 1080, RAW10, 30}

逆推:
  Decoder 不存在于 pipeline 中（无解码器时 sensor 输出直达 Crop）
  
  ... 逆推至 Sensor ...
  要求: w∈[1920,4056], fmt=RAW10
  
  Sensor 假设只支持 RAW12（不支持 RAW10）
    fmt_mask & sensor_fmts → 0
  → VSC_ERR_UNREACHABLE: RAW10 not supported by sensor (supports RAW12)
```

### 1.6 可行性预检的精度与效率

| 属性 | 说明 |
|------|------|
| **时间复杂度** | O(N)，N = STREAM entity 数量。每个 entity 做常数次区间运算 |
| **空间复杂度** | O(1)，只维护当前约束 |
| **精度** | 保守（false positive 可能，false negative 不可能） |
| **false positive 场景** | 两个 entity 的交互约束（如 Binning 的偶数要求 + Crop 的对齐要求组合后产生不可达的约束），预检可能漏过 |
| **false positive 处理** | 预检通过 → 进入完整 try_fmt 传播 → try_fmt 精确判断。预检的 false positive 在 try_fmt 阶段被捕获 |
| **false negative** | 不允许。如果预检判定不可达，必须确实不可达 |

---

## 子模块 2：Optional Entity 自动桥接算法

### 2.1 问题定义

Bitstream Descriptor 中标记为 `optional: true` 的 entity，在当前 FPGA 比特流中可能不存在（对应的 IP 未被综合进去）。系统初始化时需要：

1. 检测 optional entity 的 Driver 是否存在
2. 如果不存在，从 Pipeline 图中移除该 entity
3. 重建受影响的链路连接
4. 重新计算拓扑排序

### 2.2 可桥接性判定

**规则：只有单输入单输出（SISO）的 STREAM entity 才能自动桥接。**

```
可自动桥接:
  A(SOURCE) → C(SINK) → C(SOURCE) → B(SINK)   ← C 是 SISO
  移除 C 后: A(SOURCE) → B(SINK)               ← 直接桥接 ✓

不可自动桥接:
  A(SOURCE) → C(SINK_0)                        ← C 是 MISO/MIMO
  D(SOURCE) → C(SINK_1) → C(SOURCE) → B(SINK)
  移除 C 后: 无法确定如何桥接 → 需要手动配置

  A(SOURCE) → C(SINK) → C(SOURCE_0) → B(SINK)  ← C 是 SIMO
                       C(SOURCE_1) → D(SINK)
  移除 C 后: 无法确定 A 应该连 B 还是 D → 需要手动配置
```

**可桥接性的判定依据**：entity 中 STREAM 类型的 sink pad 数量 = 1 且 STREAM 类型的 source pad 数量 = 1。

TAP 类型的 pad 不参与桥接判定。如果 optional entity 带有 TAP 连接，移除 entity 时 TAP 链路自动失效（下游 ANALYZER 失去数据源）。

### 2.3 完整算法

```
function vsc_system_build(config, graph_desc, board_config):

    // ── Phase 1: 创建 entity ──
    created_entities = []
    removed_entities = []
    
    for each node in graph_desc.nodes:
        driver = driver_library_find(node.type)
        
        if driver == NULL:
            if node.optional:
                log_info("Optional entity '%s' driver not found, will remove", node.id)
                removed_entities.append(node)
                continue
            else:
                panic("Required entity '%s' driver not found", node.id)
        
        instance = instance_create(driver, node.base, node.id)
        entity = entity_create(instance, driver, classify_entity(node.type))
        created_entities.append(entity)
    
    // ── Phase 2: 创建 link ──
    links = []
    for each link_desc in graph_desc.links:
        src = find_entity(created_entities, link_desc.src)
        dst = find_entity(created_entities, link_desc.dst)
        
        if src == NULL or dst == NULL:
            // 至少一端 entity 不存在
            // 暂存此 link，Phase 3 用于桥接判断
            links.append({ desc: link_desc, src: src, dst: dst, orphan: true })
            continue
        
        link = create_static_link(src, dst, link_desc.type)
        links.append({ desc: link_desc, link: link, orphan: false })
    
    // ── Phase 3: 自动桥接 ──
    pipeline_entities = created_entities.copy()
    
    for each removed_node in removed_entities:
        result = auto_bridge(removed_node, links, pipeline_entities)
        if result != VSC_OK:
            return result  // 无法自动桥接，报错
    
    // ── Phase 4: 重建拓扑 ──
    active_links = [l.link for l in links if not l.orphan]
    
    // 仅对 STREAM link 做拓扑排序
    execution_order = topo_sort(pipeline_entities, active_links, 
                                link_filter = STREAM)
    
    if execution_order == ERROR_CYCLE:
        return VSC_ERR_TOPOLOGY_CYCLE
    
    if has_unreachable_required_entities(execution_order, pipeline_entities):
        return VSC_ERR_TOPOLOGY_BROKEN
    
    // ── Phase 5: 分类 entity ──
    tap_observers  = find_tap_entities(pipeline_entities, active_links)
    endpoints      = find_endpoint_entities(pipeline_entities, active_links)
    
    // ── Phase 6: 设置传播初始状态 ──
    for each entity in pipeline_entities:
        entity.propagation_state.active = true
        entity.propagation_state.visited = false
    
    // ── Phase 7: 创建 Pipeline ──
    pipeline = {
        entities        = pipeline_entities,
        links           = active_links,
        execution_order = execution_order,
        tap_observers   = tap_observers,
        endpoints       = endpoints,
        adjacency       = build_adjacency(pipeline_entities, active_links),
        state           = VSC_PIPELINE_UNCONFIGURED
    }
    
    return pipeline


// ── 自动桥接核心 ──

function auto_bridge(removed_node, links, entities):
    
    // Step 1: 找到所有与被移除 entity 相关的 link
    incoming_stream = []  // 来源 → removed_node 的 STREAM link
    outgoing_stream = []  // removed_node → 去向 的 STREAM link
    related_links   = []  // 所有相关 link（含 TAP）
    
    for each link_entry in links:
        if link_entry.desc.src == removed_node.id or
           link_entry.desc.dst == removed_node.id:
            
            related_links.append(link_entry)
            
            if link_entry.desc.type == STREAM:
                if link_entry.desc.dst == removed_node.id:
                    incoming_stream.append(link_entry)
                if link_entry.desc.src == removed_node.id:
                    outgoing_stream.append(link_entry)
    
    // Step 2: 可桥接性检查
    if len(incoming_stream) == 0 and len(outgoing_stream) == 0:
        // 孤立的 optional entity，直接移除
        remove_links(links, related_links)
        return VSC_OK
    
    if len(incoming_stream) == 1 and len(outgoing_stream) == 1:
        // SISO 模式 — 可以自动桥接
        in_link  = incoming_stream[0]
        out_link = outgoing_stream[0]
        
        // 验证两端 entity 都还存在
        if in_link.src == NULL or out_link.dst == NULL:
            // 上游或下游 entity 也不存在 → 无法桥接
            remove_links(links, related_links)
            log_warn("Cannot bridge '%s': upstream or downstream missing", 
                     removed_node.id)
            return VSC_OK  // 不算错误，链路自然断裂
        
        // 创建桥接 link
        bridge_link = create_static_link(
            in_link.src, out_link.dst, STREAM
        )
        
        // 标记旧 link 为 orphan，添加新 link
        in_link.orphan  = true
        out_link.orphan = true
        links.append({ desc: null, link: bridge_link, orphan: false })
        
        // 标记其他相关 link (TAP) 为 orphan
        for each link_entry in related_links:
            if link_entry != in_link and link_entry != out_link:
                link_entry.orphan = true
        
        log_info("Auto-bridged: %s → %s（removed optional '%s'）",
                 in_link.src.name, out_link.dst.name, removed_node.id)
        
        return VSC_OK
    
    // 多入多出 — 无法自动桥接
    return VSC_ERR_CANNOT_AUTO_BRIDGE(
        entity   = removed_node.id,
        reason   = "multi-pad optional entity requires manual topology config",
        incoming = len(incoming_stream),
        outgoing = len(outgoing_stream)
    )
```

### 2.4 桥接场景示例

#### 场景 1：简单桥接（Crop optional）

```
原始拓扑:
  Decoder(SOURCE) → Crop(SINK) → Crop(SOURCE) → CL_Out(SINK)

Crop 的 Driver 不存在（optional）:
  incoming_stream = [Decoder → Crop]  (1 条)
  outgoing_stream = [Crop → CL_Out]   (1 条)

判定: SISO ✓

桥接:
  Decoder(SOURCE) → CL_Out(SINK)

结果:
  Pipeline: Decoder → CL_Out
  应用不感知 Crop 曾经存在
```

#### 场景 2：带 TAP 的 optional entity

```
原始拓扑:
  ISP(SOURCE) → Scaler(SINK) → Scaler(SOURCE) → DDR_Writer(SINK)
  ISP(SOURCE) → [TAP] Scaler(SINK)

Scaler 的 Driver 不存在（optional）:
  incoming_stream = [ISP → Scaler]     (1 条)
  outgoing_stream = [Scaler → DDR_Writer] (1 条)
  另有 TAP link: ISP → Scaler (type=TAP)

判定: SISO ✓（TAP 不计入）

桥接:
  ISP(SOURCE) → DDR_Writer(SINK)
  TAP link ISP→Scaler 标记为 orphan（Histogram 失去数据源）

结果:
  Pipeline: ISP → DDR_Writer
  Histogram 标记为 INACTIVE（无有效 TAP 源）
  应用检测到 Histogram 不可用 → AE 降级
```

#### 场景 3：多路 optional entity（不可桥接）

```
原始拓扑:
  A(SOURCE) → MUX(SINK_0)
  B(SOURCE) → MUX(SINK_1) → MUX(SOURCE) → C(SINK)

MUX 标记为 optional 但有两个输入:
  incoming_stream = [A → MUX, B → MUX]  (2 条)
  outgoing_stream = [MUX → C]           (1 条)

判定: MISO ✗

返回: VSC_ERR_CANNOT_AUTO_BRIDGE
  原因: 2 incoming STREAM links — manual topology config required

解决方案:
  · 不标记 MUX 为 optional
  · 或在 Bitstream Descriptor 中显式声明桥接规则
```

#### 场景 4：Optional entity 缺失导致链路完全断裂

```
原始拓扑:
  Sensor → LVDS_RX → Binning → Decoder → CL_Out

Binning 和 Decoder 之间的 link 是间接的（没有中间 optional entity）。
如果 Binning 是 optional 且缺失:

  incoming_stream = [LVDS_RX → Binning]  (1 条)
  outgoing_stream = [Binning → Decoder]  (1 条)

判定: SISO ✓

桥接:
  LVDS_RX(SOURCE) → Decoder(SINK)

结果:
  Pipeline: Sensor → LVDS_RX → Decoder → CL_Out
  
  try_fmt 传播时:
    Decoder 直接收到 LVDS_RX 的输出（未经 Binning）
    如果 Decoder 能处理全分辨率 → 正常
    如果 Decoder 不能 → try_fmt SINK 拒绝，传播失败
    
  应用看到: 格式协商失败（"Decoder cannot accept 4056×3040"）
  应用调整: 使用 sensor 内部 binning 或降低输出分辨率
```

### 2.5 设计决策记录

| 决策 | 选择 | 理由 |
|------|------|------|
| 自动桥接范围 | 仅 SISO entity | MIMO/MISO 的桥接规则需要 domain knowledge，框架无法推断 |
| TAP link 处理 | 随 entity 移除而失效 | TAP 是纯观察，不存在桥接概念 |
| 桥接后的拓扑验证 | 重新做 topo_sort | 确保移除后的图仍是 DAG |
| 桥接失败处理 | 返回错误，由应用/配置决定下一步 | 避免静默错误 |

---

## 子模块 3：多分支收敛算法

### 3.1 问题定义

FPGA 视频管线通常是树形结构（Sensor 为根，多个 ENDPOINT 为叶）。当 ISP 的同一个 SOURCE pad 驱动多个下游分支时：

```
                 ┌──→ Crop_1(1920×1080) → CL_Out(1920×1080 RGB888)
  Sensor → ISP ──┤
                 └──→ Crop_2(640×480)   → DDR_Writer(640×480 RGB888)
```

两个分支从 ISP 接收到**相同的输入格式**，但经过各自的 Crop 处理后产生**不同的输出格式**。收敛算法的职责是：

1. 正确地并行传播两个分支
2. 分别记录每个分支的最终格式
3. 报告所有结果，而非强制统一

### 3.2 关键洞察：分叉不合并

与 V4L2 的复杂拓扑不同，FPGA 视频管线通常**只分叉不合并**。合并场景（两个输入进入一个 MUX）极少，且通常由显式的 MUX entity 处理。

因此收敛算法不需要解决"两个不同格式在汇合点如何统一"的问题——这个问题留给 MUX entity 自己的 Driver 解决。

### 3.3 分叉传播的核心：BFS 天然支持

前文定义的正向传播 BFS 算法已经天然支持分叉——当 entity 有多个下游 STREAM link 时，BFS 将它们**并行地加入队列**。

但这里有一个微妙之处：**格式在分叉点是共享的**。

```
ISP.SOURCE 输出: {W=1920, H=1080, FMT=RGB888}

Crop_1.SINK 收到: {1920, 1080, RGB888}  ← 同一个 fmt 对象
Crop_2.SINK 收到: {1920, 1080, RGB888}  ← 同一个 fmt 对象
```

如果 `try_fmt_sink` 修改了 `fmt`（合法的——比如钳位到能力范围），必须确保不会影响另一个分支。

**修正：分叉点做格式快照（snapshot）。**

```
// 当 entity 有多个下游 STREAM link 时:
if len(pipeline.adjacency[entity].stream_links) > 1:
    // 分叉点：每个下游获得一份独立的格式快照
    for each downstream in pipeline.adjacency[entity].stream_links:
        downstream.sink_fmt = copy_of(entity.source_fmt)  // 独立副本
    
    // 只有一个下游时:
    downstream.sink_fmt = entity.source_fmt  // 可以共享引用
```

### 3.4 分支特定格式的追溯

每个 ENDPOINT 的最终格式需要附带**分支路径**，帮助应用理解：

```json
{
  "endpoints": [
    {
      "name": "cl_out_0",
      "final_fmt": { "width": 1920, "height": 1080, "fmt": "RGB888", "fps": 30 },
      "branch_path": ["sensor_0", "lvds_rx_0", "isp_0", "crop_1", "cl_out_0"],
      "adjustments": [
        { "entity": "isp_0",   "change": "FORMAT: RAW10→RGB888" },
        { "entity": "crop_1",  "change": "NONE" }
      ]
    },
    {
      "name": "ddr_writer_0",
      "final_fmt": { "width": 640, "height": 480, "fmt": "RGB888", "fps": 30 },
      "branch_path": ["sensor_0", "lvds_rx_0", "isp_0", "crop_2", "ddr_writer_0"],
      "adjustments": [
        { "entity": "isp_0",   "change": "FORMAT: RAW10→RGB888" },
        { "entity": "crop_2",  "change": "SIZE: 1920×1080→640×480" }
      ]
    }
  ],
  "primary_endpoint": "cl_out_0"
}
```

### 3.5 分支传播的完整算法

```
function forward_propagate(pipeline, intent_fmt):

    // ── 初始化 ──
    for each entity in pipeline.all_entities:
        entity.propagation_state = {
            visited: false,
            active: true,
            sink_fmt: INVALID,
            source_fmt: INVALID,
            branch_id: -1  // 属于哪个分支（多分支时使用）
        }

    // ── 找到根节点 ──
    roots = [e for e in pipeline.execution_order if e.class == SENSOR]
    if roots is empty:
        return VSC_ERR_NO_SENSOR

    // ── 设置根的 source_fmt ──
    for each root in roots:
        root.propagation_state.source_fmt = 
            root.driver.try_fmt_source(drv_ctx, SOURCE_PAD, intent_fmt)
        root.propagation_state.visited = true

    // ── BFS 主循环 ──
    queue = roots.copy()
    
    while queue is not empty:
        entity = queue.pop_front()

        if entity.propagation_state.source_fmt is INVALID:
            mark_downstream_failed(entity, pipeline)
            continue

        out_fmt = entity.propagation_state.source_fmt

        // 获取此 entity 的所有 STREAM 下游
        stream_downstreams = []
        tap_downstreams = []
        for each (dst, link_type) in pipeline.adjacency[entity]:
            if link_type == STREAM:
                stream_downstreams.append(dst)
            elif link_type == TAP:
                tap_downstreams.append(dst)

        // ── 分叉点处理 ──
        is_fork = (len(stream_downstreams) > 1)

        for each dst in stream_downstreams:
            // 分叉时做格式快照
            fmt_for_dst = copy_fmt(out_fmt) if is_fork else out_fmt

            dst.propagation_state.sink_fmt = fmt_for_dst

            // SINK 校验
            clamped = dst.driver.try_fmt_sink(drv_ctx, SINK_PAD, fmt_for_dst)
            if clamped is INVALID:
                dst.propagation_state.active = false
                mark_downstream_failed(dst, pipeline)
                continue

            dst.propagation_state.sink_fmt = clamped

            // SOURCE 变换
            if dst.class in {SENSOR, STREAM}:
                source_fmt = dst.driver.try_fmt_source(drv_ctx, SOURCE_PAD, clamped)
                if source_fmt is INVALID:
                    dst.propagation_state.active = false
                    mark_downstream_failed(dst, pipeline)
                    continue
                dst.propagation_state.source_fmt = source_fmt

            if not dst.propagation_state.visited:
                dst.propagation_state.visited = true
                queue.push_back(dst)

        // ── TAP 旁路 ──
        for each tap in tap_downstreams:
            rc = tap.driver.try_fmt_sink(drv_ctx, SINK_PAD, copy_fmt(out_fmt))
            if rc == VSC_OK:
                tap.propagation_state.active = true
                tap.propagation_state.sink_fmt = out_fmt
            else:
                tap.propagation_state.active = false

    // ── 分支追溯 ──
    endpoint_results = []
    for each endpoint in pipeline.endpoints:
        if endpoint.propagation_state.active:
            branch = trace_branch(pipeline, endpoint)
            endpoint_results.append({
                entity:     endpoint,
                final_fmt:  endpoint.propagation_state.sink_fmt,
                branch:     branch
            })

    // ── 确定 primary ──
    primary = endpoint_results[0] if endpoint_results else null

    // ── 构建 adjustment_trace ──
    trace = build_trace(pipeline, endpoint_results)

    return {
        status:           determine_status(primary, intent_fmt),
        primary_fmt:      primary ? primary.final_fmt : INVALID,
        endpoint_fmts:    endpoint_results,
        adjustment_trace: trace
    }


// ── 分支追溯（从 ENDPOINT 反向追溯到 SENSOR）──

function trace_branch(pipeline, endpoint):
    branch = [endpoint]
    current = endpoint

    while true:
        // 找到 upstream STREAM link
        upstream = null
        for each link in pipeline.links:
            if link.type == STREAM and link.sink_entity == current:
                upstream = link.source_entity
                break
        
        if upstream == null:
            break
        
        branch.prepend(upstream)
        current = upstream

        if current.class == SENSOR:
            break
    
    return branch
```

### 3.6 收敛报告的三种格式

根据应用需求，收敛结果以三种粒度提供：

**粒度 1：仅 primary_fmt（最简单，90% 应用场景）**
```c
vsc_fmt_t result;
vsc_fmt_transaction(&intent, &result);
// result = {1920, 1080, RGB888, 30}  — 第一个 ENDPOINT 的格式
```

**粒度 2：primary + endpoint 列表（多输出应用）**
```c
vsc_fmt_transaction_ext(&intent, &ext_result);
// ext_result.primary_fmt — 主输出
// ext_result.endpoints[0] — CL_Out: {1920, 1080, RGB888}
// ext_result.endpoints[1] — DDR_Writer: {640, 480, RGB888}
```

**粒度 3：primary + endpoint + adjustment_trace（调试/UI 显示）**
```c
vsc_fmt_transaction_full(&intent, &full_result);
// full_result.adjustment_trace — 每个 entity 的格式变化路径
// "ISP changed FORMAT: RAW10→RGB888"
// "Crop_2 changed SIZE: 1920×1080→640×480"
```

### 3.7 边界情况

#### 空分支

某一分支的所有 entity 都标记为 INACTIVE（如 Crop_1 拒绝格式）→ 该 ENDPOINT 不在 `endpoint_fmts` 中。

#### 全分支失败

所有 ENDPOINT 都 INACTIVE → `VSC_ERR_NO_ACTIVE_ENDPOINT`。

#### 分支中 entity 缺失

Optional entity 在系统初始化时就已经被桥接移除，不会出现在传播阶段。到达传播阶段的 entity 都是 confirmed active 的。

#### 分叉后再分叉

```
                 ┌──→ Crop_1 ──→ CL_Out
  Sensor → ISP ──┤
                 │         ┌──→ Scaler_1 → HDMI
                 └──→ ISP_2 ──┤
                           └──→ Scaler_2 → DDR_Writer
```

BFS 自然处理——ISP_2 有两个下游（Scaler_1, Scaler_2），各自获得独立的格式快照。

---

## 四、三个子模块的交互

```
应用调用 vsc_fmt_transaction(intent)
        │
        ▼
┌──────────────────────────────────────┐
│  FEASIBILITY_CHECK（子模块 1）        │
│                                      │
│  · 使用 transform_desc 逆推约束       │
│  · O(N) 区间运算                      │
│  · 不可达 → 直接返回错误 + 范围提示     │
│  · 可达 → 进入正向传播                │
└──────────────┬───────────────────────┘
               │ feasible
               ▼
┌──────────────────────────────────────┐
│  FORWARD_PROPAGATE（BFS）             │
│                                      │
│  · 多分支: 分叉点格式快照（子模块 3）  │
│  · 每个 entity 调用 try_fmt_sink/source│
│  · TAP: 只校验不传播                  │
│  · Optional: 已在 init 阶段桥接移除    │
│            （子模块 2，此时不可见）     │
└──────────────┬───────────────────────┘
               │ success
               ▼
┌──────────────────────────────────────┐
│  CONVERGENCE（子模块 3）              │
│                                      │
│  · 分支追溯（从 ENDPOINT 反向遍历）    │
│  · 按粒度返回（primary / list / trace）│
│  · 构建 adjustment_trace             │
└──────────────────────────────────────┘
```

注意：子模块 2（自动桥接）发生在 **系统初始化阶段**，不在每次 try_fmt 的路径上。这保证了 try_fmt 的热路径不受拓扑变更影响。

---

## 五、总结

| 子模块 | 执行时机 | 时间复杂度 | 关键约束 |
|--------|---------|-----------|---------|
| 可行性预检 | 每次 try_fmt，Phase 1 | O(N) | false positive 允许，false negative 不允许 |
| 自动桥接 | 系统初始化，一次性 | O(N+M) | 仅 SISO entity 可自动桥接 |
| 多分支收敛 | 每次 try_fmt，Phase 3 | O(N×B)，B=分支数 | 分叉点格式快照，BFS 天然支持 |

三个子模块都经过具体场景推演验证。可行性预检的逆推算法优先保证"不可达一定报错"，允许少量 false positive 在后续 BFS 中被精确捕获。自动桥接严格限定在 SISO 场景，避免框架承受不属于它的复杂度。多分支收敛以 BFS 为骨架，仅增加格式快照和分支追溯两个轻量扩展。
