# VFS 用户手册

> 嵌入式轻量级虚拟文件系统（Virtual File System）
> 版本：1.0　|　代码位置：`src/core/vfs/`（含 `storage/` 装配与 FAL 驱动）

---

## 目录

1. [概述与设计思想](#1-概述与设计思想)
2. [架构设计](#2-架构设计)
3. [核心概念](#3-核心概念)
4. [快速上手](#4-快速上手)
5. [使用说明](#5-使用说明)
6. [配置选择](#6-配置选择)
7. [后端使用](#7-后端使用)
8. [移植说明](#8-移植说明)
9. [并发模型](#9-并发模型)
10. [API 参考](#10-api-参考)
11. [内存与 ROM 预算](#11-内存与-rom-预算)
12. [限制与注意事项](#12-限制与注意事项)
13. [故障排查](#13-故障排查)

---

## 1. 概述与设计思想

### 1.1 是什么

VFS 是一个面向嵌入式（MCU）场景的轻量级虚拟文件系统：对上提供 **POSIX 子集** 的统一文件接口，对下通过 **虚函数表** 适配不同的文件系统实现（littlefs / FatFs / 裸 flash 静态注册）。

它是原 `src/core/file_process` 子系统（单文件会话 + 策略后端）的升级替代。

### 1.2 设计目标

| 目标 | 落地方式 |
| --- | --- |
| **POSIX 子集** | `vfs_open/read/write/lseek/stat/...`，语义对齐，返回**负 errno** |
| **RTOS 无关** | 只依赖项目 `port` 层（`system_malloc` / `system_lock` / `sys_mutex`），不 include 任何 RTOS 头 |
| **对修改封闭，对扩展开放** | 核心只认 `vfs_fs_ops_t` 虚函数表；新增文件系统 = 新增一个 `.c` + 注册一行，核心零改动 |
| **轻量精简** | 全部静态池、零堆；特性编译期开关；只保留嵌入式常见操作 |
| **裸 flash 支持** | `rawflash` 后端：基于分区、编译期静态注册文件名与容量 |
| **编译链适配** | `stdio/` 提供 newlib（`_open_r` 等）与 ARMCC（`_sys_open` 等）两个适配层 |
| **多任务安全** | 三层锁（核心锁 / 每挂载点锁 / 后端池锁）+ 可选同文件独占锁 |
| **多存储器** | `vfs_storage → vfs_bdev(offset+size) → mount` 三级模型，资源集中在 `storage_config.c` |

### 1.3 设计思想要点

- **挂载点命名空间**：路径先经"最长前缀匹配"定位挂载点，再去掉前缀得到**相对路径**交给后端。**"属于哪个分区"由挂载时绑定决定，不在打开时解析路径字符串**。这是与旧模型（用 `分区名/文件名` 前缀让后端自己 `strchr('/')` 抠分区）的本质区别。
- **策略模式（Strategy）+ 开闭原则**：`vfs_fs_ops_t` 是唯一扩展点；后端实现它并注册即可接入。
- **能力协商（caps）**：后端声明自己支持哪些操作；核心在调用**之前**统一拦截不支持的操作并返回 `-ENOTSUP`——"不支持返回对应错误码"是契约保证，而非各后端自觉。
- **零堆**：句柄、挂载点、后端对象全部静态池（契合 S0 内存策略）。
- **资源与代码分离**：设备、分区几何、挂载点在 `storage_config.c` 定义（纯数据），换板只改这一处。

---

## 2. 架构设计

### 2.1 分层总览

```
应用层（协议 / 业务 / stdio fopen 等）
        │   vfs_open / vfs_read / vfs_write / vfs_lseek / vfs_stat / ...
        │                      （统一负 errno 契约）
┌───────▼─────────────────────────────────────────────────────────┐
│ vfs_core.c  —— 中枢，与具体文件系统完全解耦                        │
│   · 文件系统注册表      · 挂载表 + 最长前缀解析                    │
│   · 文件/目录句柄池     · caps + RO 前置校验                       │
│   · 每挂载点锁 / 同文件独占锁                                      │
├─────────────────────────────────────────────────────────────────┤
│ vfs_fs_ops_t   后端虚函数表（唯一扩展点）                          │
├──────────────┬──────────────┬────────────────┬──────────────────┤
│ vfs_littlefs │ vfs_fatfs    │ vfs_rawflash   │ (未来: fcb / ...) │
├──────────────┴──────────────┴────────────────┴──────────────────┤
│ vfs_bdev_t（分区切片：offset + 长度）                              │
│ vfs_storage_t（物理设备：整块 flash / 整张 SD）                    │
├───────────────────────────────┬─────────────────────────────────┤
│ FAL 分区表（内部 flash）        │ SD 卡块设备（512B 扇区）          │
└───────────────────────────────┴─────────────────────────────────┘
   资源装配 → src/core/vfs/storage/storage_config.c（纯数据，无逻辑）
```

### 2.2 目录结构

```
src/core/vfs/
├── vfs.h                     # 公共 API + 全部对外类型 + 后端虚函数表
├── vfs_config.h              # 编译期上限与特性开关
├── vfs_core.c                # 核心实现（注册/挂载/解析/句柄池/分派/锁）
├── vfs_bdev.h / vfs_bdev.c   # 分区切片抽象（表驱动注册 + 块/字节访问）
├── rawflash/
│   └── rawflash.h/.c         # 掉电安全块区存储核心（可独立于 VFS 使用）
├── backend/
│   ├── vfs_rawflash.h/.c     # 裸 flash 静态注册伪 FS（rawflash 核心的适配层）
│   ├── vfs_littlefs.h/.c     # littlefs 后端（原生 lfs_* API）
│   └── vfs_fatfs.h/.c        # FatFs 后端（默认编译关）
├── storage/                  # 装配/驱动层（FAL 仅出现在此目录，可剔除）
│   ├── storage_drv.h         # 介质驱动接口（vfs_storage_t）
│   ├── storage_drv_fal.h/.c  # FAL 介质驱动（vfs_storage_t 实现）
│   └── storage_config.h/.c   # 设备 / 分区 / 挂载点资源定义（表驱动）
└── stdio/
    ├── vfs_stdio_newlib.c    # newlib/glibc 系统调用钩子
    └── vfs_stdio_armcc.c     # ARMCC/Keil 重定向钩子
```

**边界约束**：`vfs_core.c`、`vfs.h`、`vfs_bdev.*` **不 include 任何后端头**，只认 `vfs_fs_ops_t` / `vfs_bdev_t`；核心与介质无关，`fal.h` 仅被 `storage/` 子目录引用（不编入 `storage_drv_fal.c` 即零 FAL 依赖）。

### 2.3 数据流（以 `vfs_open("/lfs/foo.bin")` 为例）

```
vfs_open("/lfs/foo.bin", VFS_O_RDONLY)
  │
  ├─(1) vfs_resolve：最长前缀匹配 → 命中挂载点 "/lfs"，相对路径 rel = "foo.bin"
  ├─(2) 读只读挂载点判定 / caps 前置校验（APPEND/TRUNC）
  ├─(3) handle_alloc：从 g_files[] 取一个空闲句柄槽
  ├─(4) mp_lock(mp)：加"每挂载点锁"
  ├─(5) mp->fs->open(&h->fobj, "foo.bin", flags)   ← 分派到 littlefs 后端
  │        └─ 后端从静态池取 lfs_file_t，调 lfs_file_opencfg
  ├─(6) mp_unlock(mp)
  └─(7) 返回 fd（= 句柄池下标）
```

---

## 3. 核心概念

### 3.1 挂载点（Mount Point）

- 形如 `"/lfs"`、`"/raw"`、`"/sd0"` 的绝对路径前缀。
- 一个挂载点 = 一个"绑定关系"：`挂载点 → 后端实例(fs_ctx) → 块设备(storage_dev)`。
- 路径解析取**最长前缀**匹配：`"/lfs/a/b"` 命中 `"/lfs"`（而非更短的 `"/l"`）。

### 3.2 文件描述符（fd）

- `vfs_fd_t`（`int32_t`）= 句柄池下标。
- 文件 fd = `[0, VFS_MAX_OPEN_FILES)`；目录 fd = `0x4000 | 下标`，两者互不冲突。
- 失败返回**负 errno**；`VFS_FD_INVALID (-1)` 为非法哨兵。

### 3.3 句柄池（零堆）

`vfs_file_t` / `vfs_dir_t` 是"会话对象"，由核心池持有；后端在 `open` 时填充 `priv` 指向自己的私有句柄。因此**同一个 `vfs_file_t` 不能并发复用**——但不同 fd 可在不同任务使用。

### 3.4 能力位（caps）与错误码前置

后端在 `vfs_fs_ops_t.caps` 中声明能力。核心在 `vfs_open` 中**先**校验：

- 传了 `VFS_O_APPEND` 但无 `VFS_CAP_APPEND` → `-ENOTSUP`
- 传了 `VFS_O_TRUNC` 但无 `VFS_CAP_TRUNCATE` → `-ENOTSUP`
- 可选回调为 `NULL`（如 `stat`/`unlink`）→ 分派前返回 `-ENOTSUP`

### 3.5 块设备三层模型

| 层 | 结构 | 类比 | 含义 |
| --- | --- | --- | --- |
| 物理设备 | `vfs_storage_t` | `fal_flash_dev` | 整块 flash / 整张 SD，按字节偏移 read/prog/erase |
| 分区切片 | `vfs_bdev_t` | `fal_partition` | storage 内的一段：`offset` + `blk_count`×`blk_size` |
| 挂载点 | `vfs_mount_t` | `fs_mount_t` | 绑定一个后端实例 + 一个 bdev |

---

## 4. 快速上手

### 4.1 最小可用流程

```c
#include "vfs.h"
#include "vfs_littlefs.h"
#include "vfs_rawflash.h"

/* 1) 定义块设备（几何可来自 FAL 分区表，或自定义 offset/size） */
static vfs_bdev_t bd_lfs;
static vfs_bdev_t bd_raw;

/* 2) rawflash 静态注册表 */
static const vfs_rawflash_entry_t raw_tbl[] = {
    { "boot_log", &bd_raw, 32 * 1024, 0 },
};
static const vfs_rawflash_vol_t raw_vol = { raw_tbl, 1 };

void storage_init(void)
{
    /* 3) 初始化核心 + FAL */
    vfs_init();
    fal_init();

    /* 4) 构造分区 */
    vfs_bdev_fal_flash_init(&bd_lfs, "flash_lfs", "rp2350_flash", 0x200000, 0x200000);
    vfs_bdev_fal_flash_init(&bd_raw, "flash_raw", "rp2350_flash", 0x170000, 0x10000);

    /* 5) 注册块设备 */
    vfs_bdev_register("flash_lfs", &bd_lfs);
    vfs_bdev_register("flash_raw", &bd_raw);

    /* 6) 注册文件系统类型 */
    vfs_register(VFS_FSTYPE_LITTLEFS, &vfs_littlefs_ops);
    vfs_register(VFS_FSTYPE_RAWFLASH, &vfs_rawflash_ops);

    /* 7) 挂载 */
    static const vfs_mount_cfg_t mounts[] = {
        { "/lfs", VFS_FSTYPE_LITTLEFS, "flash_lfs", 0, NULL },
        { "/raw", VFS_FSTYPE_RAWFLASH, NULL,        0, &raw_vol },
    };
    vfs_mount_table(mounts, 2);
}

/* 8) 读写文件 */
void demo(void)
{
    vfs_fd_t fd = vfs_open("/lfs/foo.bin", VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
    if (fd < 0) { /* 处理错误：fd 即负 errno */ return; }
    vfs_write(fd, "hello", 5);
    vfs_close(fd);

    vfs_stat_t st;
    if (vfs_stat("/lfs/foo.bin", &st) == 0)
        printf("size=%u\n", st.size);
}
```

### 4.2 关键顺序

1. `system_mutex_init()`（平台互斥锁初始化，**必须在任何 `system_lock` 使用之前**）
2. `vfs_init()`（核心锁）
3. `fal_init()`（FAL，幂等）
4. 构造 bdev → `vfs_bdev_register` → `vfs_register` → `vfs_mount_table`

> 本仓库中步骤 1–4 已封装在 `storage_config_init()`（由 `file_storage_init()` 调用），并已在 `init_thread_entry()` 起始处调用 `system_mutex_init()`。

---

## 5. 使用说明

### 5.1 打开标志

| 宏 | 值 | 含义 |
| --- | --- | --- |
| `VFS_O_RDONLY` | `0x0000` | 只读 |
| `VFS_O_WRONLY` | `0x0001` | 只写 |
| `VFS_O_RDWR` | `0x0002` | 读写 |
| `VFS_O_CREAT` | `0x0040` | 不存在则创建 |
| `VFS_O_EXCL` | `0x0080` | 与 CREAT 合用，已存在则 `-EEXIST`（由后端实现） |
| `VFS_O_TRUNC` | `0x0200` | 打开即截断/擦除（**必须带写意图**） |
| `VFS_O_APPEND` | `0x0400` | 追加写（rawflash 见 §7.1；开启 `VFS_RAWFLASH_ENABLE_APPEND` 后支持） |

- 访问模式（`flags & 0x3`）只能是 0/1/2；`3`（WRONLY|RDWR 位）非法 → `-EINVAL`。
- `O_TRUNC` 配 `O_RDONLY` 非法 → `-EINVAL`（防止误擦）。

### 5.2 定位原点

`VFS_SEEK_SET=0`、`VFS_SEEK_CUR=1`、`VFS_SEEK_END=2`。

### 5.3 典型用法

```c
/* 顺序写 */
vfs_fd_t fd = vfs_open("/lfs/log.txt", VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
for (int i = 0; i < n; i++)
    vfs_write(fd, buf[i], len[i]);   /* 同一会话内多次写：游标续写 */
vfs_close(fd);

/* 按偏移读（等价 lseek+read，会移动当前偏移） */
vfs_fd_t rf = vfs_open("/lfs/log.txt", VFS_O_RDONLY);
ssize_t n = vfs_pread(rf, buf, 1024, seq * 1024);
vfs_close(rf);

/* 枚举目录 */
vfs_fd_t d = vfs_opendir("/lfs");
vfs_dirent_t ent;
while (vfs_readdir(d, &ent) == 1)
    printf("%s (%u)\n", ent.name, ent.size);
vfs_closedir(d);
```

### 5.4 错误处理规范

所有 API 返回 `>= 0` 表示成功（读/写返回字节数），`< 0` 为负 errno。**不要**用 `-1` 判错，而应 `< 0`：

```c
ssize_t n = vfs_read(fd, buf, 100);
if (n < 0) { int e = -n; /* e 为 errno 值，如 ENOSPC */ }
```

---

## 6. 配置选择

通用开关位于 `vfs_config.h`；后端几何/特性宏位于各自后端头文件（littlefs 见 §6.3，rawflash 见 §7.1）。所有宏均可用编译参数 `-D` 覆盖。

### 6.1 容量上限

| 宏 | 默认 | 说明 |
| --- | --- | --- |
| `VFS_MAX_MOUNTS` | 8 | 最大挂载点数（每点占 `vfs_mount_t` + 一把锁） |
| `VFS_MAX_TYPES` | 6 | 文件系统类型注册槽 |
| `VFS_MAX_OPEN_FILES` | 6 | 文件句柄池 |
| `VFS_MAX_OPEN_DIRS` | 2 | 目录句柄池 |
| `VFS_MAX_BDEVS` | 8 | 块设备（分区切片）注册数 |
| `VFS_NAME_MAX` | 63 | 文件名最大长度 |
| `VFS_PATH_MAX` | 128 | 路径/挂载点最大长度 |

### 6.2 特性开关

| 宏 | 默认 | 置 1 的代价 | 何时开启 |
| --- | --- | --- | --- |
| `VFS_ENABLE_DIR` | 1 | 目录句柄池 + 目录回调 | 需要遍历目录时 |
| `VFS_ENABLE_DYNAMIC_MOUNT` | 0 | `vfs_mount/unmount`，支持运行时挂载 | SD 热插拔、动态卷 |
| `VFS_ENABLE_RENAME` | 0 | `vfs_rename` | 需要重命名 |
| `VFS_ENABLE_STATVFS` | 0 | `vfs_statvfs` | 需要查询卷剩余空间 |
| `VFS_ENABLE_FILE_LOCK` | 0 | 同文件独占锁表 | 多任务可能并发写同一文件 |
| `VFS_ENABLE_STDIO` | 0 | stdio 适配层（另见 §8.4） | 需要 `fopen/fprintf` 走 VFS |
| `VFS_ENABLE_MP_LOCK` | 1 | 每挂载点互斥锁 + 核心锁 | 多任务环境（强烈建议开） |
| `VFS_ENABLE_FAL_BDEV` | 1 | FAL 适配器（依赖 `fal.h`） | 使用 FAL 分区 |
| `VFS_ENABLE_FATFS` | 0 | 引入 FatFs 后端（依赖 `ff.h`） | 使用 SD + FatFs |
| `VFS_RAWFLASH_ENABLE_APPEND` | 0 | rawflash 尾部续写追加写（定义于 `vfs_rawflash.h`，见 §7.1） | rawflash 需追加写 |

> **注意**：关闭 `VFS_ENABLE_DIR` 时 `vfs_opendir` 等不编译；关闭某后端特性时对应源文件编译为空。

### 6.3 后端几何参数（littlefs）

| 宏 | 默认 | 约束 |
| --- | --- | --- |
| `VFS_LFS_READ_SIZE` | 16 | ≥1，`prog_size % read_size == 0` |
| `VFS_LFS_PROG_SIZE` | 256 | ≤ `blk_size`，`blk_size % prog_size == 0` |
| `VFS_LFS_CACHE_SIZE` | 512 | ≥ prog_size，且为 read/prog 的整数倍 |
| `VFS_LFS_LOOKAHEAD` | 32 | 为 8 的倍数 |
| `VFS_LFS_BLOCK_CYCLES` | 512 | 磨损均衡周期 |
| `VFS_LFS_FILE_CACHE` | = PROG_SIZE | ≥ prog_size 且为 prog_size 整数倍 |

> 上述几何在 `vfs_littlefs.c` 的 `lfs_vfs_mount` 中统一校验，非法值返回 `-EINVAL`。

---

## 7. 后端使用

### 7.1 rawflash（裸 flash，静态注册）

**适用**：无文件系统、需要按固定分区读写"文件"的场景（配置块、日志区、出厂数据）。

**布局**：

```
┌──────────────────────────────┬───────────────────────┐
│        数据区 [0, L-B)         │   元数据块 [L-B, L)     │
│   write 顺序落此处，cursor 递增  │  footer 主份 + 冗余份    │
└──────────────────────────────┴───────────────────────┘
  B = 一个擦除块（blk_size），元数据独占最后一块
```

**footer 格式**（共 16 字节）：`magic='RFLH'`、`version`、`flags`（保留）、`data_len`、`crc32`（覆盖 `magic..data_len` 共 12 字节）；主份在块首，冗余份在块中部（`blk_size/2`）。

**语义**：

| 打开方式 | 行为 |
| --- | --- |
| 只读 | 绝不擦/写；数据长度来自 footer |
| 写意图（WRONLY / 任一 TRUNC） | 打开即擦**整个分区** → 从 0 流式写 → close 落 footer |
| 读-改-写（RDWR，无 TRUNC） | 打开**不擦**，先可读旧内容；**首次 write** 才擦整区并从 0 重写，此后本会话只写（read → `-EBADF`）；从不 write 则 close 不改动分区 |
| 同一会话多次 write | 游标续写（支持） |
| `O_APPEND`（开 `VFS_RAWFLASH_ENABLE_APPEND=1`） | 尾部续写：读 footer 得长度 `L` → 校验 `L` 所在块尾部是否全 `0xFF` → 擦其后整块 → 从 `L` 续写 → close 落 footer |
| `O_APPEND`（开关关，默认） | `-ENOTSUP`（caps 不含 `VFS_CAP_APPEND`，核心前置拦截） |
| rename / mkdir | `-ENOTSUP`（caps 不含） |
| 写模式 lseek | `-ENOTSUP`（RDWR 读阶段可 lseek，进入写阶段后同写模式）|
| 未注册文件名 | `-ENOENT` |

**掉电安全**：数据写一半时 footer 仍为 `0xFF` → footer 无效 → 文件视为**空**，绝不读到半截脏数据。

**追加写（可选，`VFS_RAWFLASH_ENABLE_APPEND=1` 时启用）**：

- 打开方式 `O_WRONLY | O_APPEND`：**不擦**数据区，从上次提交长度 `L` 之后续写，close 提交新 footer。
- **核心不变式**：绝不向非 `0xFF` 空间编程（flash 对非 `0xFF` 区域编程只做位与、会静默损坏）。因此打开时先校验 `L` 所在块的尾部（不可擦段，≤1 块）全为 `0xFF`，干净才续写。
- **脏尾拒绝**：若该段存在非 `0xFF`（上次写数据掉电、未提交 footer 的残留），返回 `-EIO` 并**拒绝追加**；此时**只读仍然正确**（长度取 `L`，残留字节不可见），可用普通写意图打开整分区擦除重写。
- **可擦段**：`L` 之后的整块不含已提交数据，打开时**先擦除**再顺序写（写路径零改动）。
- **写-写互斥**：同一文件的写意图打开（含 append 与整擦写）由**后端自锁**串行化，第二个返回 `-EBUSY`；**不影响 littlefs/FatFs**，也无需开启全局 `VFS_ENABLE_FILE_LOCK`。
- **容量**：追加仍受 `entry.size` 约束，超限返回 `-ENOSPC`（该次 write 全有或全无）。
- `O_TRUNC | O_APPEND`：截断优先，走整擦路径。

**静态注册**：

```c
static const vfs_rawflash_entry_t tbl[] = {
    { "boot_log", &bd_log,     32*1024, 0 },          /* 可读写 */
    { "factory",  &bd_factory, 64*1024, VFS_RAWRO },  /* 只读 */
};
static const vfs_rawflash_vol_t vol = { tbl, 2 };
```

> `size` 必须 ≤ 数据区容量 `(blk_count-1) * blk_size`（挂载时校验，否则 `-EINVAL`）。

**追加写示例**：

```c
/* 需要 -DVFS_RAWFLASH_ENABLE_APPEND=1；空分区 L=0，可直接 append */
vfs_fd_t fd = vfs_open("/raw/boot_log", VFS_O_WRONLY | VFS_O_APPEND);
if (fd < 0) {
    /* -EIO = 脏尾拒绝；-EBUSY = 已被写会话占用；-ENOTSUP = 开关未开 */
} else {
    vfs_write(fd, "\n", 1);   /* 续写到上次长度之后 */
    vfs_close(fd);            /* 提交新 footer */
}
```

### 7.2 littlefs

**适用**：需要目录/长文件名/掉电一致性的一般文件存储，且底层是块设备（flash 分区 / SD）。

**要点**：

- 直接调用原生 `lfs_*` API（RTOS 无关）；每个挂载点持有独立 `lfs_t`（多分区 = 多 `lfs_t`）。
- `lfs_t` / `lfs_file_t` / `lfs_dir_t` 及每文件 cache 全部来自**静态池**。
- 本后端**关闭** littlefs 自带线程安全，由 VFS 每挂载点锁统一串行化（若构建开了 `LFS_THREADSAFE` 需在 core 侧关闭，避免双重加锁）。
- 首次使用未格式化的分区，`mount` 会失败（`-EIO`）；可调用：

```c
int vfs_littlefs_format(vfs_mount_t *mp);   /* 破坏性：格式化 */
```

### 7.3 FatFs（可选，默认编译关）

**适用**：SD 卡（512B 扇区）+ FatFs 库。

**要点**：

- 每个挂载点占用一个 FatFs 逻辑盘号 `pdrv`，路径自动加盘符前缀（`"0:/..."`）。
- 自带 `diskio`：`disk_read/write/ioctl` 转发到 `vfs_bdev_*`，扇区约定 **512 字节**。
- 启用前需：工程引入 FatFs（`ff.h`/`diskio.h`）+ 提供 SD `vfs_bdev`（512B 块），并置 `VFS_ENABLE_FATFS=1`。

---

## 8. 移植说明

### 8.1 移植到新 RTOS / 裸机

VFS 只依赖 `port` 层的少量接口：

```c
void    *system_malloc(size_t size);
void     system_free(void *ptr);
void     system_lock(sys_lock_id_t id);      /* 固定 ID 锁 */
void     system_unlock(sys_lock_id_t id);
void     system_mutex_init(void);            /* ★初始化固定 ID 锁 */
sys_mutex_t *sys_mutex_create(void);         /* 句柄式锁 */
void     sys_mutex_destroy(sys_mutex_t *m);
void     sys_mutex_lock(sys_mutex_t *m);
void     sys_mutex_unlock(sys_mutex_t *m);
```

在 `port_impl.c` 中按 `PORT_OS_*` 分支实现（FreeRTOS / Zephyr / RT-Thread / LIBC 已内置）。裸机单线程可用 `PORT_OS_LIBC`（空锁 + `malloc`）。

> **务必在启动早期调用 `system_mutex_init()`**，否则固定 ID 锁（`SYS_LOCK_*`）为空操作，后端池锁失效。

### 8.2 移植到新存储器（新 flash / 新 SD）

1. 定义一个 `vfs_storage_t`：填写 `blk_size`/`blk_count` 与 `read/prog/erase` 三个字节偏移回调。
2. 为每个分区构造 `vfs_bdev_t`：
   - 走 FAL：`vfs_bdev_fal_init(&bd, name, "分区名")` 或 `vfs_bdev_fal_flash_init(&bd, name, "flash_dev", offset, size)`；
   - 直接定义：`vfs_bdev_init(&bd, name, &storage, offset, blk_size, blk_count)`。
3. `vfs_bdev_register(name, &bd)`。
4. 在挂载表里把 bdev 名绑定到某个文件系统类型。

### 8.3 新增文件系统后端（开闭原则）

只需 3 步，**不改核心**：

1. 新建 `backend/vfs_myfs.c`，实现 `vfs_fs_ops_t`（填充函数指针 + `caps`）。
2. 在头文件 `extern const vfs_fs_ops_t vfs_myfs_ops;`。
3. 应用侧 `vfs_register(VFS_FSTYPE_EXTERNAL_BASE + n, &vfs_myfs_ops)`（或自定义 ID），挂载即可。

**后端实现约束**：

- 回调的路径参数 `rel` 是**挂载点内相对路径**（核心已剥离前缀），后端不要再假设带前缀。
- `mount()` 中用自己的 `mp->storage_dev` 建立 FS 实例，存入 `mp->fs_ctx`。
- 句柄从静态池取；池扫描必须用 `system_lock(SYS_LOCK_FILE)` 保护（因为池是后端全局的，可能被多个挂载点并发访问）。
- 不支持的操作：可选回调置 `NULL`，或从 `caps` 中省略对应位。

### 8.4 stdio 适配（编译链）

两个文件按工具链**自动择一**编译（都在 `VFS_ENABLE_STDIO=1` 前提下）：

| 工具链 | 文件 | 钩子 |
| --- | --- | --- |
| GCC/newlib/picolibc | `vfs_stdio_newlib.c` | `_open`/`_open_r`、`_read`/`_read_r`、`_write`/`_write_r`、`_lseek`/`_lseek_r`、`_stat`/`_stat_r`、`_fstat`/`_fstat_r`、`_close`/`_close_r`、`_unlink`/`_unlink_r`、`_isatty`/`_isatty_r`（成对） |
| ARMCC/Keil | `vfs_stdio_armcc.c` | `_sys_open`、`_sys_read`、`_sys_write`、`_sys_seek`、`_sys_istty`、`_sys_flen` |

**要点**：

- 用一张 `fd → vfs_fd_t` 小映射表；**0/1/2 保留**给 stdin/stdout/stderr，应用 fd 从 3 起。
- 内部把 `-errno` 转为 newlib 约定（返回 `-1` 并置 `errno`）。
- 这些是**全局系统调用钩子**，会与平台 libc 的其它实现冲突；仅当你确实要让标准 I/O 走 VFS 时才开启。
- **加锁提醒**：stdio 的 `g_fdtable` 目前无锁，多线程 `fopen/fclose` 需外部同步。

---

## 9. 并发模型

三层锁，加锁顺序**恒定**（`mp_lock → SYS_LOCK_FILE`），无 ABBA 死锁：

| 锁 | 实现 | 保护对象 | 临界区 |
| --- | --- | --- | --- |
| 核心锁 | `g_core_lock`（`sys_mutex`） | 注册表 / 挂载槽 / 句柄池 alloc-free / `g_flocks` | 极短 |
| 每挂载点锁 | `mp->lock`（`sys_mutex`） | 单次后端调用（littlefs/fatfs 非线程安全） | 一次调用 |
| 后端池锁 | `SYS_LOCK_FILE`（固定 ID） | 各后端**全局**静态池的扫描分配 | 极短 |
| 同文件锁（可选） | `g_flocks[]` | 同一 `(挂载点, 相对路径)` 的写意图打开 | 打开期 |

**使用约定**：

- 不同任务使用**不同 fd** 访问同一卷 → 由 `mp->lock` 串行化，安全。
- **同一个 fd 被多任务并发读写** → 未定义（同 POSIX，需调用者同步）。
- 开启 `VFS_ENABLE_FILE_LOCK` 后，第二个写打开同一文件返回 `-EBUSY`。
- `vfs_mount_table` / `vfs_deinit` 应在**单线程初始化/反初始化阶段**调用。

---

## 10. API 参考

> 头文件：`vfs.h`。所有 `int` 返回：`>=0` 成功，`<0` 负 errno。

### 10.1 类型与常量

```c
typedef int32_t vfs_fd_t;              /* 文件/目录描述符，失败为负 errno */
#define VFS_FD_INVALID (-1)

typedef enum { VFS_TYPE_FILE = 0, VFS_TYPE_DIR = 1 } vfs_type_t;

typedef struct vfs_stat    { uint8_t type; uint32_t size; }               vfs_stat_t;
typedef struct vfs_dirent  { uint8_t type; char name[VFS_NAME_MAX+1]; uint32_t size; } vfs_dirent_t;
typedef struct vfs_statvfs { uint32_t total_bytes; uint32_t free_bytes; } vfs_statvfs_t;

typedef enum {
    VFS_FSTYPE_RAWFLASH = 0, VFS_FSTYPE_LITTLEFS, VFS_FSTYPE_FATFS,
    VFS_FSTYPE_EXTERNAL_BASE
} vfs_fstype_t;

#define VFS_MNT_RO (1u << 0)   /* 只读挂载 */
```

### 10.2 生命周期

| 函数 | 说明 | 返回 |
| --- | --- | --- |
| `int vfs_init(void)` | 清空所有池 + 建核心锁（幂等） | 0 / `-ENOSPC` |
| `void vfs_deinit(void)` | 关闭所有句柄、卸载所有挂载点、释放锁 | — |
| `int vfs_register(uint8_t fstype, const vfs_fs_ops_t *ops)` | 注册文件系统类型 | 0 / `-EINVAL` / `-EALREADY` / `-ENOSPC` |
| `int vfs_bdev_register(const char *name, const vfs_bdev_t *bdev)` | 注册块设备 | 0 / `-EINVAL` / `-EEXIST` / `-ENOSPC` |
| `int vfs_mount_table(const vfs_mount_cfg_t *tbl, size_t n)` | 批量挂载静态表 | 0 / 最后一个失败码 |
| `int vfs_mount(const vfs_mount_cfg_t *cfg)` | 动态挂载（需 `VFS_ENABLE_DYNAMIC_MOUNT`） | 0 / `-ENOTSUP`（未开） |
| `int vfs_unmount(const char *mnt_point)` | 动态卸载（需 `VFS_ENABLE_DYNAMIC_MOUNT`） | 0 / `-ENOENT` / `-EBUSY` |
| `const vfs_fs_ops_t *vfs_fstype_lookup(uint8_t fstype)` | 查后端（诊断） | 指针 / NULL |

### 10.3 文件操作

| 函数 | 说明 | 返回 |
| --- | --- | --- |
| `vfs_fd_t vfs_open(const char *path, int flags)` | 打开/创建 | fd≥0 / `-ENOENT`/`-EINVAL`/`-EROFS`/`-ENOTSUP`/`-EEXIST`/`-ENFILE`/`-EBUSY` |
| `int vfs_close(vfs_fd_t fd)` | 关闭 | 0 / `-EBADF` |
| `ssize_t vfs_read(vfs_fd_t fd, void *buf, size_t n)` | 读 | 字节数（0=EOF）/ `<0` |
| `ssize_t vfs_write(vfs_fd_t fd, const void *buf, size_t n)` | 写 | 字节数 / `<0` |
| `off_t vfs_lseek(vfs_fd_t fd, off_t off, int whence)` | 定位 | 新偏移 / `<0` |
| `off_t vfs_tell(vfs_fd_t fd)` | 当前偏移 | 偏移 / `<0` |
| `int vfs_sync(vfs_fd_t fd)` | 刷缓存 | 0 / `<0` |
| `ssize_t vfs_pread(vfs_fd_t fd, void *buf, size_t n, off_t off)` | 定位读（**会移动偏移**） | 字节数 / `<0` |

### 10.4 文件系统级 / 目录

| 函数 | 说明 | 返回 |
| --- | --- | --- |
| `int vfs_stat(const char *path, vfs_stat_t *st)` | 查状态（挂载点根返回目录） | 0 / `<0` |
| `int vfs_unlink(const char *path)` | 删除 | 0 / `<0` |
| `int vfs_mkdir(const char *path)` | 建目录 | 0 / `<0`（`-ENOTSUP` 不支持） |
| `int vfs_rename(const char *from, const char *to)` | 重命名（`VFS_ENABLE_RENAME`） | 0 / `<0` |
| `int vfs_statvfs(const char *path, vfs_statvfs_t *st)` | 卷容量（`VFS_ENABLE_STATVFS`） | 0 / `<0` |
| `vfs_fd_t vfs_opendir(const char *path)` | 打开目录（`VFS_ENABLE_DIR`） | fd / `<0` |
| `int vfs_readdir(vfs_fd_t fd, vfs_dirent_t *ent)` | 读目录项 | 1 有项 / 0 结束 / `<0` |
| `int vfs_closedir(vfs_fd_t fd)` | 关闭目录 | 0 / `-EBADF` |

### 10.5 块设备 API（`vfs_bdev.h`）

| 函数 | 说明 |
| --- | --- |
| `void vfs_bdev_init(d, name, storage, byte_offset, blk_size, blk_count)` | 初始化分区切片 |
| `uint32_t vfs_bdev_size(const vfs_bdev_t *d)` | 分区总字节数 |
| `int vfs_bdev_read/prog/erase(d, blk, buf/nblk, nblk)` | 按**块号**访问 |
| `int vfs_bdev_read_bytes/prog_bytes(d, byte_off, buf, n)` | 按**字节偏移**访问 |
| `int vfs_bdev_fal_init(d, name, fal_part)` | 从 FAL 分区表构造（复用 offset/len） |
| `int vfs_bdev_fal_flash_init(d, name, fal_flash, offset, size)` | 从 FAL flash 设备 + 自定义几何构造 |

### 10.6 后端接口 `vfs_fs_ops_t`（`vfs.h`）

扩展点。字段：`name`、`caps`，以及回调：

- **必需**：`open`、`read`、`write`、`lseek`、`close`、`stat`、`unlink`
- **可选**：`mount`、`unmount`、`tell`、`sync`、`truncate`、`rename`、`mkdir`、`statvfs`、`opendir`、`readdir`、`closedir`
- 路径参数 `rel` 均为**挂载点内相对路径**。

能力位：`VFS_CAP_READ/WRITE/APPEND/SEEK/TRUNCATE/DIR/RENAME/MKDIR/STATVFS`。

### 10.7 挂载配置 `vfs_mount_cfg_t`

```c
typedef struct vfs_mount_cfg {
    const char *mnt_point;   /* "/lfs" */
    uint8_t     fstype;      /* VFS_FSTYPE_* */
    const char *bdev_name;   /* 绑定的块设备名，rawflash 可 NULL */
    uint8_t     flags;       /* VFS_MNT_* */
    const void *fs_data;     /* 后端私有：rawflash vol / littlefs cfg 等 */
} vfs_mount_cfg_t;
```

---

## 11. 内存与 ROM 预算

### 11.1 RAM（静态，零堆）

| 项 | 估算 |
| --- | --- |
| 核心池 | `VFS_MAX_MOUNTS×sizeof(vfs_mount_t)` + `VFS_MAX_OPEN_FILES×sizeof(vfs_handle_t)` + 目录池 |
| littlefs | 每挂载点 `sizeof(lfs_t)` + 3 个缓冲（read/prog=cache_size，lookahead）；每文件 `sizeof(lfs_file_t)` + `FILE_CACHE` |
| rawflash | 每文件游标结构；每目录迭代器 |
| FatFs | 每挂载点 `sizeof(FATFS)`；每文件 `sizeof(FIL)` |

> 例：默认几何（cache 512、lookahead 32、file cache 256、6 文件）× 1 个 littlefs 挂载约 **2–4 KB** 静态 BSS。按板子调整 `VFS_MAX_OPEN_FILES` / cache 大小。

### 11.2 ROM

核心约 2–3 KB；各后端随具体文件系统库而定。

---

## 12. 限制与注意事项

1. **单分区 ≤ 4GB**：`vfs_bdev_t` 的 `offset`/`blk_count` 为 `uint32_t`。>4GB 介质需拆分为多个切片。
2. **`vfs_pread` 非 POSIX 语义**：等价 `lseek+read`，**会改变当前偏移**。
3. **`vfs_deinit` / `vfs_mount_table` 非并发安全**：只在初始化/反初始化阶段调用。
4. **`vfs_deinit` 后须重新 `vfs_init`** 才能再用。
5. **rawflash 写打开即整分区擦除**：只读打开零副作用；`O_RDONLY|O_TRUNC` 被核心拦截为 `-EINVAL`。（追加写打开 `O_APPEND` 除外——它不擦数据区。）
6. **rawflash** 不支持 rename / mkdir / 写模式 lseek（caps 声明，返回 `-ENOTSUP`）；**跨会话 append** 仅在 `VFS_RAWFLASH_ENABLE_APPEND=1` 时支持，脏尾返回 `-EIO`、写-写并发返回 `-EBUSY`。
7. **stdio 适配层是全局钩子**：与平台 libc 可能冲突；0/1/2 保留；`g_fdtable` 无锁。
8. **FatFs 后端默认关**：需自行引入 FatFs 库与 SD 块设备。
9. **`mount` 不做重复挂载点**：重复挂载点返回 `-EEXIST`；bdev 重名返回 `-EEXIST`。

---

## 13. 故障排查

| 现象 | 可能原因 | 排查 |
| --- | --- | --- |
| `vfs_open` 返回 `-ENOENT` | 挂载点未挂 / 路径写错 / rawflash 未注册文件名 | `vfs_fstype_lookup` 查注册；确认 `vfs_mount_table` 返回 0 |
| `-ENOTSUP` | 后端不支持该操作 / 特性被编译关 | 查 `caps`；查 `vfs_config.h` 开关 |
| `-EROFS` | 写只读挂载点或只读 rawflash 条目 | 检查 `VFS_MNT_RO` / `VFS_RAWRO` |
| rawflash append 返回 `-EIO` | 尾部含非 `0xFF`（上次掉电残留，脏尾） | 用普通写意图（`O_WRONLY`）打开整擦重写 |
| rawflash 写返回 `-EBUSY` | 同文件已有写会话未关闭 | 先 `vfs_close` 旧写 fd；查是否有 fd 泄漏 |
| `-ENFILE` | 句柄池耗尽 | 增大 `VFS_MAX_OPEN_FILES/DIRS`，或检查是否有 fd 泄漏（未 close） |
| `-ENOSPC` | 空间不足 / 挂载槽或锁池耗尽 | 检查介质剩余；增大 `VFS_MAX_MOUNTS` / `SYS_MUTEX_POOL_SIZE` |
| 挂载返回 `-EIO` | littlefs 分区未格式化/损坏 | 调用 `vfs_littlefs_format(mp)` 后重试 |
| 挂载返回 `-EINVAL` | 分区几何非法（对齐/容量/参数） | 检查 `vfs_bdev_fal_flash_init` 的 offset/size 是否块对齐、≤ 设备容量 |
| 并发下数据错乱 | 固定锁未初始化 | 确认启动早期调用了 `system_mutex_init()` |
| 挂载 `-ENOSPC` | `SYS_MUTEX_POOL_SIZE` 不足 | 需 ≥ `VFS_MAX_MOUNTS + 1`（含核心锁） |

---

*本手册随代码演进；如与代码注释不一致，以头文件 Doxygen 注释为准。*
