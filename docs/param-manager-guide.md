# param_manager 使用指南

> 版本: 1.0
> 分工: 代码注释讲"为什么", 本文讲"怎么选"。所有 API 签名以
> `src/core/param_manager/core/param_manager.h` 为准。
> 完整背景 (概述/设计思想/架构/移植) 见 `docs/param-manager-user-manual.md`。

---

## 1. 心智模型: 三层状态 + 一条启动时间线

参数值存在于三层, 层间流动由框架管理:

```
      [存储]  Flash/文件 (仅 PARAM_FLAG_PERSIST 参数)
        │ load_all / save_all
        ▼
      [缓存]  param_entry.cache (读写 API 的直接作用对象)
        │ flush (按 dirty)  / write 回调 (IP 直通)
        ▼
      [硬件]  寄存器/业务状态 (IP write 回调 / App flush 回调)
```

启动时间线 (由 `param_boot_standard()` 一次调用完成):

```
存储后端 → param_init → param_service_init(元数据+迁移+旋钮)
→ 模块注册 (自动或手动清单) → param_load_all (存储→缓存, 再逐模块 init→硬件)
→ param_check_module_order (自检一次) → param_validate_all (裁剪越界)
→ param_flush (缓存→硬件)
```

---

## 2. 写入口决策表 (共 5 个具名入口 + 2 个高级入口)

| 你的意图 | 用哪个 | 行为要点 |
|---|---|---|
| 改一个值, 等 flush 批量下发 | `param_write` | App 走 apply 校验; IP 只改缓存标脏。**越界拒绝** (OUT_OF_RANGE) |
| 在 apply 回调里改自己 (防重入) | `param_write_cache` | 只写缓存 + 标脏 + notify, **跳过 apply**; 范围校验仍在 |
| 立刻校验/下发, 不凑 flush 批次 | `param_write_sync` | IP: write 回调直通硬件 + 清脏; App: apply + **标脏** (硬件仍由 flush 下发) |
| 写字节流 (BLOB/协议/上位机) | `param_write_raw` | 唯一带长度入口; BLOB 要求 len == blob_size; EXEC 参数自动路由到命令 |
| 写字符串 | `param_write_string` | 超 max_len 截断, 返回正码 `PARAM_OK_TRUNCATED` (失败判定用 `ret < 0`) |
| 用缓存当前值重走一遍写入路径 | `param_reapply` | 维护操作: 越界先裁剪收敛; 典型场景是运行时收紧 range 后 |
| [高级] 需要"越界裁剪而非拒绝" | `param_write_ex(..., PARAM_WR_CLAMP)` | 显式裁剪旗标; 未定义旗标返回 NOT_SUPPORTED |
| [高级] 字节流 + 策略旗标 | `param_write_bytes` | 值类型解码后走 write_ex; BLOB/STRING 与 write_raw 等价 |

错误语义约定:

- 用户写入越界 = **拒绝** (`PARAM_ERR_OUT_OF_RANGE`), 不做静默裁剪;
  裁剪只发生在显式维护操作 (validate_all / reapply / CLAMP 旗标)。
- App 的 apply 拒绝 = **缓存回滚** (返回错误 = 无任何变化);
  IP 硬件写失败不回滚 (flush 重试模型需要)。
- notify **统一**触发: 所有写入成功路径 + 命令执行 (param_exec /
  write_raw / write_sync 触发 EXEC) + 单条 load_one / reset_one。
  `new_value` 为缓存生效值; EXEC 时携带命令负载 (与 exec 回调收到的 arg 一致)。

typed 包装 (`param_write_u32` / `param_read_f32` 等) **校验类型**, 错配返回
`PARAM_ERR_TYPE_MISMATCH` (`param_write_i32` / `param_read_i32` 对 ENUM 放行)。
读侧: `param_read` 回调失败静默回退缓存; 只信权威源用 `param_read_hw`。
错误码转可读串用 `param_strerror()`。

---

## 3. 新增一个参数: 3 处

以 MODULE_SYS 模块为例 (`param_service.h` / `param_service.c`):

1. 头文件: `#define PID_XXX MAKE_PARAM_ID(MODULE_SYS, 0xNN)`
   —— local_id 在模块内**追加在末尾, 绝不插中间** (与已落盘记录一一对应)。
2. 模块 .c: 定义条目宏 `PARAM_APP_UINT(xxx, PID_XXX, PARAM_FLAG_PERSIST, def, min, max)`
   (要持久化就加 `PARAM_FLAG_PERSIST`; 固定值参数加 `PARAM_FLAG_FIXED` 强制校验)。
3. 模块 .c: 条目指针加入 `PARAM_TABLE`。

不需要动: module_ids.h / 注册调用 / dump / notify / 持久化路由 (按注册表自动生效)。

## 4. 新增一个模块: 6 处

1. `module_ids.h` 的 `PARAM_MODULE_LIST` 登记**一行** (ID + 初始化顺序同时生成;
   行序 = init/flush 顺序, IP 硬件在前)。
2-5. 新模块文件: PID 宏们 + 条目宏们 + `PARAM_TABLE` +
   `PARAM_APP_MODULE_DEFINE(...)` / `PARAM_MODULE_INIT(...)`
   (IP 用 `PARAM_IP_MODULE_DEFINE`; 两者同构, 生成 `_module` 后缀,
   注册统一走 `PARAM_MODULE_INIT` → `param_module_register`)。
6. `src/app/init/param_manager_init.c` 的 `register_modules()` 补一行
   `xxx_module_init();` (或开启 `PARAM_MODULE_AUTO_REGISTER_ENABLE` 免此行,
   需要链接脚本支持 `.rodata.param_modules` 段)。

## 5. 常用陷阱 TOP-10

1. **越界写入被拒绝**是设计语义: 需要裁剪语义时显式用 CLAMP/validate_all。
2. **单缓冲 BLOB/STRING**: `param_reset_all`/`reset_one` 不恢复其初始内容 (自拷贝被跳过)。
3. **param_read 的 STRING/BLOB 返回内部缓冲指针**: 不拷贝、会随并发写改变、禁止释放;
   安全拷贝用 `param_read_string` / `param_read_raw`。
4. **apply 拒绝后缓存保持不变** (回滚): 不要假设"拒绝后缓存=新值"。
5. **App 的 `param_write_sync` 不等于写硬件**: 它标脏, 硬件仍由 `param_flush` 下发。
6. **在 write/apply 回调里改自己**: 用 `param_write_cache` (跳 apply 防重入),
   不要用 `param_write`。
7. **EXEC 参数走 `param_write` 会返回 `PARAM_ERR_EXEC_ONLY`**: 用 `param_exec` /
   `param_write_raw` / `param_write_sync` 触发。
8. **local_id 永不漂移**: 已发布固件的存储兼容依赖 ID 编码; 新增追加、重命名用
   迁移表 (`param_migrate_storage`), 不要改号。
9. **`PARAM_OK_TRUNCATED` 是正码**: 严格判定用 `ret == PARAM_OK`,
   失败判定用 `ret < 0`。
10. **flush 失败会重试**: dirty 保留、下一轮 `param_flush` 自动收敛; 不要因此
    在 write 回调里调 `param_save_one` (每轮重试都写 Flash, 磨损放大)。

---

## 6. 持久化与分区

- 后端三选一 (互斥): `PARAM_MKV_ENABLE` / `PARAM_INIPARSER_ENABLE` /
  `PARAM_FLASHDB_ENABLE` (config/param_manager_config.h)。
- 多分区: `PARAM_PARTITION_COUNT` (factory + 用户区), 经
  `param_get_storage_partition` / `param_set_storage` 运行时切换;
  UserSet SELECTOR/LOAD/SAVE/RESET 旋钮 (param_service) 是高层封装。
- `param_set_storage` 现返回状态码 (缺 load/save 的驱动被拒绝)。
- 单条落盘 `param_save_one` / 全量 `param_save_all`; 无存储后端时
  持久化 API 返回 `PARAM_ERR_NOT_SUPPORTED`。

## 7. 模块初始化顺序

唯一事实源 = `module_ids.h` 的 `PARAM_MODULE_LIST` **行序** (IP 硬件在前、
业务模块在后); 注册先后无关 (注册时按该表有序插入链表)。不在清单内的
模块排末尾, 启动自检 `param_check_module_order` 会告警。
