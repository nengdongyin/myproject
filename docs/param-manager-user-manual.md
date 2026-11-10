# param_manager 用户手册

> 版本: 1.0
> 适用范围: `src/core/param_manager/` 全部子系统
> 配套文档: `docs/param-manager-guide.md` (决策表速查) · 本文为完整手册

---

## 1. 概述

param_manager 是一个面向嵌入式 MCU 的**内存参数管理框架**，统一管理三类参数的完整生命周期：

| 参数类别 | 典型例子 | 数据流向 |
|---|---|---|
| 业务参数 (App) | 曝光目标、算法开关、模式选择 | 缓存 → flush 回调 → 业务实例 |
| IP 寄存器参数 (IP) | 传感器曝光寄存器、ROI 配置 | 缓存 → write 回调 → 硬件寄存器 |
| 命令 (EXEC) | 校准触发、UserSet 存/取/复位 | 直接路由到模块 exec 回调 |

核心能力：

- **零动态分配**：参数条目/模块实例全部静态定义，编译期初始化，无 malloc；
- **两级 vtable 多态**（OOP-in-C）：参数级 + 模块级虚函数表，App/IP 两套行为共享一套骨架；
- **类型化参数**：UINT/INT/FLOAT/BOOL/ENUM/BLOB/STRING/EXEC 八种类型，编译期分派表消除 switch-case；
- **统一持久化抽象**：FlashDB / mkv / iniparser 三种后端可换，支持多分区（出厂 + 用户区）与版本迁移；
- **明确的错误语义**：越界写入**拒绝**而非静默裁剪、apply 拒绝**回滚**、notify **统一**、typed 包装**真实校验类型**、模块清单**单点登记**。

---

## 2. 设计思想

### 2.1 单一事实来源

| 事实 | 唯一定义处 |
|---|---|
| 参数 ID | `MAKE_PARAM_ID(module_id, local_id)`，模块内追加分配、**永不改号** |
| 模块 ID + 初始化顺序 | `module_ids.h` 的 `PARAM_MODULE_LIST` 单一清单（X-macro 生成 ID 枚举 + `MODULE_INIT_ORDER`） |
| 条目布局 | `PARAM_ENTRY_HEAD()` 宏（6 个派生结构体的唯一布局来源） |
| 分区索引→分区名 | `param_storage_common.h` 的 `PARAM_PARTITION_NAME_LIST` |
| 后端返回值翻译 | `param_status_from_backend()`（0/非0 → param_status_t 的唯一翻译点） |

### 2.2 依赖方向：派生 → 基类，无反向

- 基类（`param_manager.c`/`param_manager.h`）只命名基类符号；
- 派生侧（`app_param_manager.*` / `ip_param_manager.*`）引用基类实现组装 vtable，并把自己的 vtable 常量注入条目/模块；
- 存储后端只依赖 `param_storage_drv_t` + 注入端口（`param_restore_sink_t`），**禁止**反向包含框架内部头或遍历条目——控制流始终是"框架指挥后端"。

### 2.3 错误语义约定

| 约定 | 含义 |
|---|---|
| 越界写入**拒绝** | 用户直写返回 `PARAM_ERR_OUT_OF_RANGE`；裁剪只发生在显式维护操作（`param_validate_all` / `param_reapply` / `PARAM_WR_CLAMP`） |
| apply 拒绝**回滚** | 返回错误 = 无任何变化（值类型快照回滚；BLOB/STRING 先验后写）；IP 硬件写失败仍不回滚（flush 重试模型需要） |
| notify **统一** | 全部写入成功路径 + EXEC 三触发路径 + 单条 load_one/reset_one 都触发；new_value 为缓存生效值 |

### 2.4 锁纪律（移植者必读）

- 单把全局锁 `SYS_LOCK_PARAM` 保护所有框架全局状态；
- **回调一律锁外执行**（apply/read/write/exec/notify/flush/foreach），回调内可安全调用加锁 API；
- 存储后端的内部可变状态**没有自己的锁**，靠框架锁串行化——后端回调内不得再加锁；
- flush / 遍历类 API 两阶段：锁内只快照指针，锁外执行回调；
- IP 脏位 take-and-clear：锁内取快照+清脏，锁外写硬件，失败回滚脏位（下一轮自动重试）。

---

## 3. 架构

### 3.1 分层

```
┌─────────────────────────────────────────────────────────┐
│ 应用层    模块文件 (条目+回调) · param_manager_init 胶水   │
│           param_boot_standard (启动序列) · param_service │
├─────────────────────────────────────────────────────────┤
│ 公共 API  param_manager.h (类型/宏/API) · param_strerror  │
├─────────────────────────────────────────────────────────┤
│ 派生壳    app_param_manager (App vtable)                 │
│           ip_param_manager  (IP vtable, 注册)            │
├─────────────────────────────────────────────────────────┤
│ 基类      param_manager.c (两级 vtable 通用实现/哈希/链表) │
│           param_data_ops.c (8 类型分派表: 读写/存取/复位)  │
├─────────────────────────────────────────────────────────┤
│ 存储抽象  param_storage_drv_t (load/save/delete/erase/    │
│           分区/批量事务) — flashdb / mkv / iniparser       │
├─────────────────────────────────────────────────────────┤
│ 平台移植  port.h (锁/内存/时间 6 函数, PORT_OS_* 选择)     │
└─────────────────────────────────────────────────────────┘
```

### 3.2 文件清单

```
src/core/param_manager/
  config/    module_ids.h           模块单一清单 (ID+ORDER)
             param_manager_config.h 全部可调配置 (开关/容量/分区数)
  core/      param_manager.h/.c     公共 API + 基类实现
             param_manager_internal.h  框架内部 API (锁纪律成文于此)
             app_param_manager.h/.c App 派生壳 + 模块宏
             ip_param_manager.h/.c  IP 派生壳 + 模块宏
             param_data_ops.h/.c    8 类型编译期分派表
             param_boot.h/.c        标准启动序列封装
             param_dump.h/.c        参数状态格式化输出
             param_service.h/.c     元数据/UserSet 运维旋钮 (SFNC 16.x)
  storage/   param_storage_drv_t 三个后端 + 共用分区映射
```

### 3.3 关键数据结构

- **param_id**：32 位 = 高 16 位 module_id + 低 16 位 local_id（`MAKE_PARAM_ID`）；
- **param_entry_t**：条目基类（id + vtable 指针），派生结构体带 type/flags/dirty/cache/default_val；
- **param_module_t**：模块实例（链表节点 + 回调组 + ctx），App/IP 共用同一结构体；
- **注册表**：参数→开放寻址哈希表（`PARAM_HASH_SIZE` 槽，2 的幂）；模块→按 `MODULE_INIT_ORDER` 有序的单链表（注册先后无关）；
- **存储记录**：键 = 参数 ID，值 = 字节负载（值类型 4/8 字节，BLOB/STRING 按其容量）。

---

## 4. 接口

### 4.1 生命周期与启动

| API | 说明 |
|---|---|
| `param_init(storage, notify)` | 框架初始化（storage 可为 NULL = 不持久化） |
| `param_boot_standard(&hooks)` | 标准启动序列一次调用（见 §6.1） |
| `param_deinit()` / `param_is_initialized()` | 反初始化 / 状态查询 |
| `param_module_is_initialized(id)` | 查询模块 init 完成标志 |

### 4.2 写入（完整决策表见 guide §2）

| API | 一句话 |
|---|---|
| `param_write(id, value)` | 标准写：apply 校验 + 标脏等 flush；越界**拒绝** |
| `param_write_cache(id, value)` | 只写缓存（跳过 apply，回调内自修改用）；校验/标脏/notify 仍在 |
| `param_write_sync(id, value)` | 立即校验/下发（IP 直通硬件+清脏；App apply+标脏） |
| `param_write_raw(id, data, len)` | 唯一带长度入口（BLOB/字节流/EXEC 路由） |
| `param_write_string(id, str)` | 字符串糖，截断返回正码 `PARAM_OK_TRUNCATED` |
| `param_reapply(id)` | 用缓存当前值重放写入路径（维护操作，越界先裁剪收敛） |
| `param_write_ex(id, value, flags)` | [高级] 策略旗标入口（当前唯一旗标 `PARAM_WR_CLAMP`） |
| `param_write_bytes(id, data, len, flags)` | [高级] 字节流 + 策略旗标 |

typed 包装（`param_write_u32/i32/f32/bool` 等）运行时校验类型；`param_strerror()` 错误码转可读串。

### 4.3 读取 / 命令 / 刷新 / 持久化 / 重置 / 遍历

| 分组 | API |
|---|---|
| 读 | `param_read`（回调失败回退缓存）/ `param_read_cache`（纯缓存）/ `param_read_hw`（只读权威源）/ `param_read_raw` / `param_read_string` |
| 命令 | `param_exec(cmd_id, user_arg)` |
| 刷新 | `param_flush()`（按 ORDER 遍历 dirty 模块，失败保留 dirty 下轮重试） |
| 持久化 | `param_save_all` / `param_save_one` / `param_load_all` / `param_load_one` / `param_delete_one` / `param_delete_all` |
| 重置 | `param_reset_all` / `param_reset_one`（BLOB/STRING 单缓冲不恢复内容） |
| 遍历/工具 | `param_foreach` / `param_get_stats` / `param_validate_all` / `param_set_range` / `param_get_range` / `param_get_size` / `param_check_module_order` |

### 4.4 模块定义与注册宏

```c
/* 条目: 8 种类型, App 前缀 PARAM_ / IP 前缀 PARAM_IP_ */
PARAM_UINT(name, id, flags, default, min, max);
PARAM_STRING(name, id, flags, writable_buf, max_len);

/* 模块: 回调具名字段, 任意顺序/可省略 */
PARAM_MODULE_DEFINE(name, MODULE_ID, "label",
                    .ctx = &inst, .init = fn, .write = fn, .flush = fn);
PARAM_MODULE_INIT(name, params_table);          /* 生成 name_module_init() */
IP_DRIVER_DEFINE(...) / IP_DRIVER_INIT(...)     /* IP 同构, 生成 _module 后缀 */
```

### 4.5 存储后端接口（移植新后端）

`param_storage_drv_t`（见 param_manager.h）核心槽位：`load`（三态 + read_len 出参）/ `load_all`（批量，可 NULL）/ `save` / `save_begin`+`save_commit`（批量事务，成对可 NULL）/ `delete` / `erase_all` / `deinit` / `get_active_partition`+`set_active_partition` / `get_partition`。契约要点：save/delete 回调**持锁调用**、begin/commit **锁外**调用、load_all 经注入 sink 逐条推送。

---

## 5. 适用场景

1. **设备参数持久化**：出厂默认 + 用户修改 + 掉电保持（多分区实现"出厂集/用户集"）；
2. **FPGA/传感器 IP 寄存器统一管理**：硬件细节封装在 driver 回调里，上层只见参数；
3. **上位机/协议访问**：参数 ID ↔ 寄存器地址映射（如 Imperx 协议经 `param_write_raw` 写寄存器）；
4. **A/B 分区固件升级**：启动分区索引 + 分区热切换；
5. **SFNC (GenICam) UserSet 语义**：Select/Load/Save/Reset/Default 旋钮已内建（param_service）。

不适用：需要高速流式写入（毫秒级频繁变更且需落盘）的场景——请结合批量事务或自行合并落盘；超大 BLOB（KB 级）双备份默认值（单缓冲设计，见 §7）。

---

## 6. 使用说明

### 6.1 快速开始（5 步）

```c
/* 1. module_ids.h 登记一行 (ID + 顺序一次生成) */
X(MODULE_EXAMPLE, 0x04u)

/* 2. 模块文件: 条目 + 表 + 模块 + 注册 (回调具名字段) */
PARAM_UINT(exp_gain, PID_EXP_GAIN, PARAM_FLAG_PERSIST, 10, 1, 100);
PARAM_TABLE(exp_params, &exp_gain.base);
PARAM_MODULE_DEFINE(exp, MODULE_EXAMPLE, "Example",
                    .ctx = &g_inst, .write = exp_write, .flush = exp_flush);
PARAM_MODULE_INIT(exp, exp_params);

/* 3. init 胶水 register_modules() 补一行 exp_module_init(); */

/* 4. 启动: 一次调用 */
param_boot_hooks_t hooks = { .storage_factory = make_storage,
                             .notify = on_changed,
                             .register_modules = register_modules };
param_boot_standard(&hooks);

/* 5. 运行期读写 */
param_write_u32(PID_EXP_GAIN, 42);
param_value_t v; param_read(PID_EXP_GAIN, &v);
```

完整模板见 `examples/param_example_module.c`（8 类型全覆盖，可直接复制）。

### 6.2 回调契约速记

| 回调 | 语义 | 注意 |
|---|---|---|
| `init(ctx)` | load_all 的 INIT 阶段调用，面对完整恢复后的缓存 | 在此读缓存批量下发 |
| `read(ctx,id,&v)` | 只读权威源（硬件寄存器/实时计算） | 取不到必须返回错误（否则污染缓存）；可 NULL |
| `write(ctx,id,v)` | App=apply 校验/转换；IP=直通硬件 | 拒绝=错误码；回调内 `param_read` 反映新值 |
| `flush(ctx)` | 批量下发（App）/ 聚合操作（IP） | 失败保留 dirty 下轮重试；回调内禁止再调 param_flush |
| `exec(ctx,id,arg)` | EXEC 命令 | 三触发路径的 arg 形状见头文件文档 |

### 6.3 多分区 / UserSet / 迁移

- `param_storage_set_active_partition(idx)` 写启动分区索引，下次启动生效；
- `param_get_storage_partition(idx)` + `param_set_storage(drv)` 运行时切换（状态码化）；
- 版本迁移：`param_migrate_storage(storage, table, count)`，在 param_init 之后、load_all 之前调用；破坏性格式变更递增 `PARAM_SCHEMA_VERSION`。

---

## 7. 注意事项

1. **local_id 永不漂移**：已发布固件的存储记录按 ID 寻址；新增追加、重命名走迁移表；
2. **越界写入被拒绝**是设计语义：要裁剪语义显式用 CLAMP/validate_all/reapply；
3. **BLOB/STRING 单缓冲**：缓存与默认值共享缓冲，reset 不恢复其初始内容；缓冲必须静态可写（严禁字符串字面量）；
4. **param_read 的 BLOB/STRING 返回内部指针**：非拷贝、随并发写变化、禁止释放；安全拷贝用 read_raw/read_string；
5. **App 的 write_sync 不等于写硬件**：标脏等 flush；
6. **回调内改自己**用 `param_write_cache`（跳 apply 防重入），回调内禁止 `param_flush`；
7. **EXEC 误用**：`param_write` 对 EXEC 返回 `PARAM_ERR_EXEC_ONLY`；
8. **`PARAM_OK_TRUNCATED` 是正码**：失败判定用 `ret < 0`；
9. **flush 失败自动重试**：不要在 write 回调里做 `param_save_one`（重试 = 反复写 Flash，磨损放大）；
10. **notify 回调锁外执行**：自行处理防重入；
11. **min==max 不再是"无范围"捷径**：需要固定值校验用 `PARAM_FLAG_FIXED`；
12. **模块 ID 范围约定**：App 0x01~0x7F、IP 0x80~0xFF（分配约定，非语义——行为由 vtable 决定）。

---

## 8. 移植指南

### 8.1 平台层（必做）

实现 `port.h` 声明的 6 个函数（参考 `src/platform/port/port_impl.c`）：

| 函数 | 说明 |
|---|---|
| `system_lock/unlock(SYS_LOCK_PARAM)` | 互斥锁（RTOS）或关中断/空操作（裸机，`PORT_LOCK_IRQ=1`） |
| `system_mutex_init()` | 锁初始化（启动时调一次） |
| `system_malloc/free` | 框架本身零动态分配；供关联子系统使用 |
| `system_get_time_ms()` | 毫秒时间（超时/统计用） |

OS 选择：编译参数定义 `PORT_OS_FREERTOS/ZEPHYR/RTTHREAD/LIBC/CUSTOM` 之一为 1（互斥校验）。

### 8.2 新存储后端（选做，不持久化可跳过）

1. 实现 `param_storage_drv_t` 全部回调（可 NULL 的槽位：load_all/save_begin+commit/delete/erase_all/分区对）；
2. 键命名建议：参数 ID 的十进制/十六进制字符串（参考 flashdb 的 `"p%lu"`）；
3. 记录格式：字节负载 = 值类型 sizeof(param_value_t) / BLOB blob_size / STRING max_len+1；
4. 遵守锁契约（§2.4）：save 持锁、begin/commit 锁外、load_all 经 sink 推送、**不得**包含框架内部头；
5. `param_manager_config.h` 加 `PARAM_XXX_ENABLE` 开关，`param_storage_create_selected()` 加分支；
6. 多分区：实现 `get_partition` 工厂 + 分区名映射（`param_storage_common.h`）。

### 8.3 配置清单（param_manager_config.h）

| 宏 | 含义 | 默认 |
|---|---|---|
| `PARAM_DEBUG_NAME_ENABLE` | 条目调试名（4B/条） | 1 |
| `PARAM_DUMP_ENABLE` | dump 输出（拉入 stdio） | 1 |
| `PARAM_SERVICE_LOG_ENABLE` | 运维日志 | 1 |
| `PARAM_MODULE_AUTO_REGISTER_ENABLE` | linker 段自动注册（需链接脚本支持 `.rodata.param_modules` 段） | 0（手动注册） |
| `PARAM_HASH_SIZE` | 参数哈希槽数（2 的幂，全系统参数总数上限） | 256 |
| `PARAM_PARTITION_COUNT` | 分区总数（factory + 用户区，同步 FAL 分区表/MAX_INSTANCES/名称表） | 5 |
| `PARAM_FLASHDB/INIPARSER/MKV_ENABLE` | 后端三选一（互斥，选中必须编入固件） | mkv=1 |

### 8.4 启动集成（两选一）

```c
/* A. 推荐: 标准序列 */
param_boot_standard(&hooks);

/* B. 手动: 等价序列 (参考 param_boot.c, 用于特殊启动时序) */
param_init(storage, notify); param_service_init(NULL, 0);
register_modules(); param_load_all(); param_check_module_order();
param_validate_all(); param_flush();
```

### 8.5 验收清单

- [ ] `param_boot_standard` 返回 OK，dump 输出全部参数；
- [ ] 写越界值返回 OUT_OF_RANGE 且缓存不变；
- [ ] 掉电重启后 PERSIST 参数恢复；
- [ ] 分区切换后 load/save 落在目标分区；
- [ ] `param_check_module_order` 无遗漏告警；
- [ ] 多线程并发写 + flush 无死锁（回调锁外约定）。

---

## 附录 A：错误码表

| 码 | 含义 |
|---|---|
| `PARAM_OK (0)` | 成功 |
| `PARAM_OK_TRUNCATED (1)` | 成功但值被截断（STRING 超长，正码） |
| `-1 INVALID_ID` / `-2 OUT_OF_RANGE` / `-3 READONLY` / `-4 TYPE_MISMATCH` / `-5 NOT_FOUND` | 参数/值/类型错误 |
| `-6 FLASH_FAIL` / `-7 ALREADY_REG` / `-8 NO_MEMORY` | 存储/注册/容量 |
| `-9 TIMEOUT` / `-10 BUSY` | 时序错误 |
| `-11 NOT_SUPPORTED` | 本固件不具备该能力（配置/后端缺回调） |
| `-12 NOT_INITIALIZED` | 框架未初始化 |
| `-13 EXEC_ONLY` | 命令参数走了普通写路径 |

## 附录 B：快速决策表（写）

| 想做什么 | API |
|---|---|
| 改值等 flush | `param_write` |
| 回调内改自己 | `param_write_cache` |
| 立即校验/下发 | `param_write_sync` |
| 字节流/BLOB | `param_write_raw` |
| 字符串 | `param_write_string` |
| 缓存重放 | `param_reapply` |
| 越界裁剪而非拒绝 | `param_write_ex(..., PARAM_WR_CLAMP)` |
