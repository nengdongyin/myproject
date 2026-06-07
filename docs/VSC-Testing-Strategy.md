# VSC 测试策略

**版本**：1.0.0

---

## 1. 测试架构

```
测试类型              工具          位置
─────────────────────────────────────────────────
单元测试              Unity         src/test/vsc/test_vsc_resolver.c
集成测试              Unity         同上 (full_pipeline_end_to_end)
CI checksum 校验      Python        tools/ci_check_vsc_gen.py
CI type 校验          Python        tools/check_graph_types.py
```

### 1.1 框架

- **Unity Test** (`src/test/core/unity/`) — 轻量级 C 测试框架
- **独立运行器** (`test_runner_vsc.c`) — 绕过 Zephyr，纯 host 运行

### 1.2 编译与运行

```bash
gcc -std=c11 -Wall -Wextra -Werror -pedantic \
    -Isrc/vsc -Igen/vsc -Isrc/test/core -Isrc/test/core/unity \
    src/vsc/*.c src/modules/**/*.c gen/vsc/*.c \
    src/test/vsc/test_vsc_resolver.c \
    src/test/core/unity/unity.c \
    src/test/core/test_suite.c \
    test_runner_vsc.c -o build/test_vsc

./build/test_vsc
```

---

## 2. 测试覆盖 (47 tests)

### 2.1 Phase 1 — Feasibility Pre-check (8 tests)

| 测试 | 覆盖 |
|------|------|
| `test_feasibility_linear_pass_chain` | PASS_THROUGH 链：intent 在传感器范围内 → FEASIBLE |
| `test_feasibility_binning_chain` | BINNING 链：intent×2 在传感器范围内 → FEASIBLE |
| `test_feasibility_exceeds_sensor_max` | intent 超出传感器最大 → UNREACHABLE |
| `test_feasibility_exceeds_crop_max` | intent 超出 crop 最大 → UNREACHABLE |
| `test_feasibility_crop_boundary` | intent 等于 crop max → FEASIBLE |
| `test_feasibility_format_mismatch` | 格式不兼容 → UNREACHABLE |
| `test_feasibility_binning_odd_width` | 奇数宽度 → FEASIBLE (保守近似) |
| `test_feasibility_empty_pipeline` | 空管线 → UNREACHABLE |

### 2.2 Optional Entity Auto-bridging (5 tests)

| 测试 | 覆盖 |
|------|------|
| `test_bridge_siso_simple` | SISO 桥接：3 实体 → 移除中间 → 2 实体直连 |
| `test_bridge_leaf_removal` | 叶节点移除 (端点) → 无需桥接 |
| `test_bridge_root_removal` | 根节点移除 (传感器) → 剩余链保留 |
| `test_bridge_tap_dies` | TAP 随源消失 + MIMO 拒绝 |
| `test_bridge_across_types` | 跨类型桥接：移除 Binning → sensor 直连 decoder |

### 2.3 Phase 2 — Forward Propagation (7 tests)

| 测试 | 覆盖 |
|------|------|
| `test_forward_linear_exact` | 直通链：格式完全匹配 |
| `test_forward_binning_halves` | Binning 折半尺寸 |
| `test_forward_crop_clamps` | Crop 钳位到 max |
| `test_forward_multi_branch` | 多分支：所有端点收到相同格式 |
| `test_forward_tap_success` | TAP 成功 (格式支持) |
| `test_forward_tap_failure` | TAP 失败 (格式不支持，静默标记) |
| `test_forward_sink_rejection` | Sink 拒绝 → 下游标记 INACTIVE |

### 2.4 Phase 3 — Convergence (4 tests)

| 测试 | 覆盖 |
|------|------|
| `test_converge_identical_branches` | 两个相同分支 → 均报告 |
| `test_converge_trace_populated` | adjustment_trace 正确填充 |
| `test_converge_tap_failure_partial` | TAP 失败 → PARTIAL 状态 |
| `test_converge_branch_isolation` | 分支隔离：binning 变换正确记录 |

### 2.5 Integration — Full Pipeline (4 tests)

| 测试 | 覆盖 |
|------|------|
| `test_integration_exact_match` | intent 与 output 完全匹配 |
| `test_integration_adjusted` | Binning 调整尺寸 → ADJUSTED 状态 |
| `test_integration_invalid_intent` | 无效 intent → VSC_ERR_INVALID_INTENT |
| `test_integration_multi_branch_tap` | 多分支 + TAP 全链路 |

### 2.6 P2 Feature System (5 tests)

| 测试 | 覆盖 |
|------|------|
| `test_feature_streaming_always_true` | STREAMING 始终可用 |
| `test_feature_default_all_false` | 未 derive 时全部为 false；derive 后基于注册表 |
| `test_feature_manual_set_and_query` | 手动 set/get 往返 |
| `test_feature_boundary_invalid_id` | 越界 ID 安全返回 false/NULL |
| `test_feature_reset_clears_all` | Reset 清空全部 |

### 2.7 Bitstream Loader (4 tests)

| 测试 | 覆盖 |
|------|------|
| `test_loader_generated_init_builds_pipeline` | 生成初始化构建完整 Pipeline |
| `test_loader_optional_node_skipped` | 可选节点 unknown type → 跳过 |
| `test_loader_override_tightens_crop` | Override 收紧 crop max_width |
| `test_loader_override_refuses_relax` | Override 拒绝放宽约束 |

### 2.8 驱动测试 (10 tests)

| 测试 | 覆盖 |
|------|------|
| `test_crop_vsc_init` / `_try_fmt_sink` / `_try_fmt_source` | Crop VSC 适配器 |
| `test_crop_hw_standalone` | Crop 纯 HW 驱动独立测试 (零 VSC 依赖) |
| `test_binning_vsc_halves` | Binning 折半 |
| `test_decoder_vsc_format_conv` | Decoder RAW→RGB + 格式拒绝 |
| `test_histogram_vsc_format_filter` | Histogram 格式过滤 + ANALYZER 无 source |
| `test_histogram_hw_standalone` | Histogram 纯 HW 独立测试 |
| `test_sensor_vsc_init_and_source` | Sensor VSC 适配器 (回退模式) |

### 2.9 端到端集成 (1 test)

| 测试 | 覆盖 |
|------|------|
| `test_full_pipeline_end_to_end` | 5 个真实驱动全链路 try_fmt + adjustment_trace + Feature 验证 |

---

## 3. 如何添加新测试

### 3.1 添加测试函数

```c
// 在 test_vsc_resolver.c 中
static void test_my_new_feature(void)
{
    // 1. 设置 (构建 pipeline, 注册驱动...)
    // 2. 执行
    // 3. 断言
    TEST_ASSERT_EQUAL_INT(VSC_OK, rc);
}
```

### 3.2 注册测试

```c
// 在 resolver_run_all_tests() 中添加
RUN_TEST(test_my_new_feature);
```

### 3.3 测试命名规范

```
test_<module>_<scenario>
```

示例：`test_feasibility_linear_pass_chain`, `test_crop_vsc_init`, `test_full_pipeline_end_to_end`

---

## 4. Mock 策略

### 4.1 格式变换 Mock

使用 `static const vsc_ip_ops_t` 预定义 ops 表：

```c
static const vsc_ip_ops_t g_ops_pass    = { NULL, mock_sink_pass,  mock_source_pass,  NULL };
static const vsc_ip_ops_t g_ops_binning = { NULL, mock_sink_pass,  mock_source_binning, NULL };
static const vsc_ip_ops_t g_ops_crop    = { NULL, mock_sink_pass,  mock_source_crop,   NULL };
```

### 4.2 ISC Mock

```c
// isc_bridge.c — null_read 返回 IMX477 chip ID (0x0477)
// isc_open() 成功完成 probe
// 其他 isc_*() 调用通过弱引用 → no-op
```

### 4.3 寄存器 Mock

```c
// 所有纯 HW 驱动的 reg_write() 为空实现
// 测试验证 API 行为而非寄存器值
```

---

## 5. CI 集成

```bash
# 1. Checksum 校验 (Schema YAML 变更时)
python tools/ci_check_vsc_gen.py

# 2. Type 校验 (system_graph.json 变更时)
python tools/check_graph_types.py drivers/system_graph.json drivers

# 3. 单元测试 (每次提交)
gcc ... -o build/test_vsc && build/test_vsc
# 预期: 47 Tests 0 Failures
```
