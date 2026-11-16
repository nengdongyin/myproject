# Shell 命令上机测试用例

> 覆盖当前固件里**已注册的全部命令**：17 条业务命令 + 7 条 letter-shell 内置命令。
> 每条用例给出「输入 / 预期输出 / 判定依据」，最后留「实测」栏供现场填写。
>
> 生成依据：`src/core/shell`、`src/core/vfs/vfs_shell.c`、`src/core/debug/*`、
> `src/core/file_transfer/file_transfer_shell.c`、`src/core/console/console_mode.c`、
> `src/app/init/shell_cmds.c`。与代码不一致时以代码为准。

---

## 0. 前置条件

### 0.1 接线与参数

| 项 | 值 |
| --- | --- |
| 串口 | **uart1**（GPIO4=TX / GPIO5=RX），460800 8N1，无流控 |
| 终端 | 普通串口终端（CR 或 LF 均可回车；支持退格、上下键历史） |
| 提示符 | `root:/$ `（默认用户 `root`，`cd` 不改变提示符内容） |

### 0.2 进入 / 离开 shell 模式

uart1 默认是**协议解析模式**。用 Imperx 写命令切入 shell：

```text
PC → 57 F0 03 00 00 00 00        # WRITE 0xF003 = PID_CONSOLE_ENTER_SHELL
设备 → 06                        # Imperx ACK（框架在回调返回后发）
设备 → [shell mode] type 'help' for commands, 'uart_mode proto' to return
```

离开 shell（不传文件时用手敲）：

```text
root:/$ uart_mode proto
设备 → [protocol mode]
```

### 0.3 上电时 uart1 上会出现的一小段字节（**不是缺陷**）

`shell_init()` 会打一次提示符，此时还是协议模式：

```text
\r\nroot:/$ 
```

约 11 字节。协议上位机在上电瞬间会收到它们，属已知产物；发 0xF003 之后提示符才是给操作者看的。

### 0.4 三条通用判定

1. 每条命令执行完，letter-shell 会重新打 `root:/$ `（在 shell 模式下）。
2. 输入行长度上限 ≈85 字符（512/(5+1)）；超了回 `Warning: Command is too long`。
3. 命令不存在的统一回包是 `Command not Found`。

---

## 1. 会话与模式

### T-MODE-01 查询当前模式
```text
输入: uart_mode
预期: uart parse mode: shell
      usage: uart_mode proto    hand uart back to protocol
判定: 第一行是 shell（因为我们正处在 shell 里）
```

### T-MODE-02 未知参数
```text
输入: uart_mode foo
预期: uart_mode: unknown 'foo'
判定: 不切换模式，仍在 shell
```

### T-MODE-03 切回协议 + 协议侧能重新收到应答（**关键**）
```text
步骤: uart_mode proto          → 预期 [protocol mode]
      再发 Imperx 读 0x0010:     52 00 10
预期: 设备回 06 + 4 字节曝光值（证明协议链已恢复解析）
判定: 若没有任何回包 → 模式没切回或链被锁
```

### T-MODE-04 协议模式下敲字符不该有 shell 反应
```text
步骤: uart_mode proto 之后，直接敲 "ls" + 回车
预期: 终端无任何 shell 回显/输出（字节进了协议链，被当无效帧头）
判定: 出现 "Command List" 或 `(empty)` → 模式没切回
```

---

## 2. 系统信息

### T-SYS-01 status（接收统计 / 链状态 / 传输状态）
```text
输入: status
预期: rx bytes : <十进制>
      chain    : up
      ft state : idle
判定: 三项都有；chain 为 up（协议链已创建）
```

### T-SYS-02 dump（打印参数系统）
```text
输入: dump
预期: 多行参数文本，每行带 EasyLogger 前缀（时间/级别/标签），内容形如
      <module>.<param> = <value> [min..max] dirty/falg 列
判定: 能列出 IP_SENSOR / FILE_TRANSFER / CONSOLE 等模块的参数；无 (null) 崩溃
备注: 输出走日志通道，可能与 shell 提示符交错；行数多时按住等它打完
```

### T-SYS-03 dump 指定模块
```text
输入: dump 5                 # MODULE_CONSOLE = 0x05
预期: 只列出 Console 模块的 2 条参数（console/enter_shell 等，dump 显示为 EXEC）
判定: 不再出现其它模块
```

### T-SYS-04 ps —— 当前固件预期是"不支持"
```text
输入: ps
预期: ps: not supported by this port build
            (Zephyr: CONFIG_THREAD_MONITOR + THREAD_STACK_INFO + INIT_STACKS; FreeRTOS: configUSE_TRACE_FACILITY)
判定: 出现上面两行即"未开 Kconfig，按设计降级"；报 HardFault 则为 BUG
说明: 本固件未开那三个 Kconfig，所以走 -ENOTSUP 分支。开了之后应输出线程表
      name / state / prio / stack / usedmax / free%
```

### T-SYS-05 stacks —— 同上
```text
输入: stacks
预期: stacks: not supported by this port build
            (Zephyr: ... 同上 ...)
判定: 同 T-SYS-04
```

---

## 3. 内存读写（白名单保护）

> 白名单在启动时由 `mem_regions_configure()` 注入：`flash 0x10000000 +4MiB` 只读、
> `sram 0x20000000 +0x82000(520KiB)` 可读写。外设段未开放。

### T-MEM-01 md 读 SRAM（正常读）
```text
输入: md 0x20000000 16
预期: 20000000: <16 个两位十六进制，空格分隔> |<16 个 ASCII 或 .>|
判定: 恰好 1 行；地址前缀等于 20000000
```

### T-MEM-02 md 多行 + 默认长度
```text
输入: md 0x20000000
预期: 4 行（默认 len=64，16 字节/行），地址 20000000/10/20/30
```

### T-MEM-03 md 长度上限截断（不报错）
```text
输入: md 0x20000000 600
预期: 32 行（被夹到 512 字节），无错误提示
判定: 行数 = 32；不出现 truncated 以外的报错
```

### T-MEM-04 md 读 flash（只读段允许读）
```text
输入: md 0x10000000 16
预期: 1 行十六进制；首字节通常为 00（RP2350 向量表/镜像头）
判定: 允许读（writable=0 只影响写）
```

### T-MEM-05 md 越界 —— 白名单拒绝
```text
输入: md 0x0 16
预期: md: 0x00000000 + 16 not allowed (out of whitelist)
判定: 无任何内存内容输出（没有读，所以不会 fault）
```

### T-MEM-06 md 跨段边界 —— 必须拒绝
```text
输入: md 0x20081FF0 16      → 允许（正好到 SRAM 末尾）
       md 0x20081FF0 17      → 拒绝
预期: 第一条 1 行；第二条 md: 0x20081ff0 + 17 not allowed (out of whitelist)
判定: 差 1 字节就从"允许"变"拒绝"，说明整段同区间校验生效
```

### T-MEM-07 md 参数错误
```text
输入: md
预期: usage: md <addr> [len]    (addr supports 0x)
输入: md zzz
预期: md: bad address 'zzz'
输入: md 0x20000000 zz
预期: md: bad length 'zz'
```

### T-MEM-08 mw 写只读段 —— 必须拒绝（**安全**）
```text
输入: mw 0x10000000 0x12345678
预期: mw: 0x10000000 + 4 not allowed (out of whitelist)
判定: flash 不可写；此用例不会破坏任何东西，可放心跑
```

### T-MEM-09 mw 未对齐 —— 必须拒绝
```text
输入: mw 0x20080001 0x12345678
预期: mw: 0x20080001 not aligned to 4
输入: mw 0x20080001 0x1234 h
预期: mw: 0x20080001 not aligned to 2
```

### T-MEM-10 mw 宽度参数错误
```text
输入: mw 0x20080000 1 x
预期: mw: bad width 'x' (b|h|w)
输入: mw 0x20080000
预期: usage: mw <addr> <value> [b|h|w]
```

### T-MEM-11 mw 真正写入 + 回读（**可选，需先确认目标地址安全**）

两种安全做法，任选其一：

**做法 A（严格安全，推荐）**：写 `rtos_shell.c` 的静态数组 `s_rows`（只被 ps/stacks 使用，写坏无副作用）
```text
PC 侧: arm-zephyr-eabi-nm build/zephyr/zephyr.elf | findstr /c:" s_rows"
       → 得到形如 2000xxxx 的地址（记作 <ADDR>）
输入:  md <ADDR> 16            # 先看一眼原值
输入:  mw <ADDR> 0xA5A5A5A5
预期:  <ADDR> <- a5a5a5a5 (readback a5a5a5a5)
输入:  md <ADDR> 4
预期:  首 4 字节为 a5 a5 a5 a5
恢复:  mw <ADDR> <原值>
判定: readback 与原值一致，且 ps/stacks 仍正常报"不支持"
```

**做法 B（省事，先验空再写）**
```text
输入: md 0x20080000 16
判定: 若 16 字节全为 00 或 ff（说明该处未被使用）才继续；
      否则换一个地址或改用做法 A
输入: mw 0x20080000 0x12345678
预期: 20080000 <- 12345678 (readback 12345678)
输入: mw 0x20080000 0xAB b
预期: 20080000 <- ab (readback ab)
输入: mw 0x20080000 0xBEEF h
预期: 20080000 <- beef (readback beef)
备注: 若写后系统异常 → 复位即可（SRAM 内容不持久）
```

### T-MEM-12 白名单未注入时的行为（可选，需改代码验证）
```text
说明: 若启动日志出现 "mem whitelist not configured, md/mw will be refused"
输入: md 0x20000000 16
预期: md: no whitelist configured
判定: fail-closed 生效（不配置 = 全拒，而不是全放开）
```

---

## 4. 文件系统（VFS）

> 挂载点：`/lfs`（littlefs，可读写）、`/raw`（rawflash 伪 FS）。
> 根 `/` 是 **shell 层虚拟节点**，只能用于 `ls /` 与 `cd /`，不是能打开文件的挂载点。

### T-FS-01 pwd
```text
输入: pwd
预期: /
判定: 上电后 cwd 就是根
```

### T-FS-02 ls 根 —— 列挂载卷
```text
输入: ls /
预期: d /lfs     rw fstype=1
      d /raw     rw fstype=0
判定: 两行；fstype=1 是 littlefs、=0 是 rawflash
```

### T-FS-03 cd + pwd
```text
输入: cd /lfs
预期: （无任何输出）
输入: pwd
预期: /lfs
```

### T-FS-04 cd 失败分支
```text
输入: cd /nope
预期: cd: ENOENT
输入: cd /raw/boot_log      # 是文件不是目录
预期: cd: not a directory
输入: cd
预期: （无输出，回到根）
输入: pwd
预期: /
```

### T-FS-05 mkdir + ls（顺序依赖）
```text
输入: mkdir /lfs/t1
预期: （无输出）
输入: ls /lfs
预期: d        0  t1        # 目录类型 d，size 0
输入: mkdir /lfs/t1
预期: mkdir: EEXIST
输入: mkdir /nope/x
预期: mkdir: ENOENT
```

### T-FS-06 write + cat（顺序依赖）
```text
输入: write /lfs/t1/a.txt hello world
预期: （无输出 —— 该命令成功时不打印任何东西）
输入: cat /lfs/t1/a.txt
预期: hello world
判定: 内容 + 自动补换行；若为空则写失败
```

### T-FS-07 stat
```text
输入: stat /lfs/t1/a.txt
预期: /lfs/t1/a.txt
        type: file
        size: 12
判定: size 与上一步写入内容长度一致（"hello world\n" = 12）
输入: stat /lfs/t1
预期: /lfs/t1 / type: dir / size: 0
输入: stat /lfs/none
预期: stat: ENOENT
```

### T-FS-08 cat 错误分支
```text
输入: cat
预期: usage: cat <file>
输入: cat /lfs/none
预期: cat: ENOENT
输入: cat /lfs/t1
预期: cat: EISDIR
```

### T-FS-09 hexdump
```text
输入: hexdump /lfs/t1/a.txt
预期: 00000000  68 65 6c 6c 6f 20 77 6f 72 6c 64 0a          |hello world.|
判定: 十六进制区 16 列（不足补空格）+ 右侧 ASCII（不可打印显示 .）
输入: hexdump /lfs/t1/a.txt 6 5
预期: 00000006  77 6f 72 6c 64 ...   |world|
输入: hexdump /lfs/t1/a.txt zz
预期: hexdump: bad offset
输入: hexdump
预期: usage: hexdump <file> [off] [len]
```

### T-FS-10 cat 大文件截断（顺序依赖）
```text
准备: 先传一个 >4KiB 的文件（见第 5 节），或 write 多次
输入: cat /lfs/big.bin
预期: 输出停止后出现 ... truncated at 4096 bytes
判定: 出现该行即封顶生效
```

### T-FS-11 rm（顺序依赖）
```text
输入: rm /lfs/t1/a.txt
预期: （无输出）
输入: cat /lfs/t1/a.txt
预期: cat: ENOENT
输入: rm /lfs/t1
预期: （无输出；littlefs 的空目录可删）
输入: ls /lfs
预期: 不再有 t1
输入: rm /lfs/none
预期: rm: ENOENT
```

### T-FS-12 未注册命令（mv / df 本固件未编译）
```text
输入: mv a b
预期: Command not Found
输入: df
预期: Command not Found
判定: VFS_ENABLE_RENAME / VFS_ENABLE_STATVFS 默认 0，命令未注册（不是坏了）
```

---

## 5. 文件传输（ymodem）

> 命令只有一个 `ft`：
> `ft`=查状态、`ft recv [dir]`=设目录并启动收、`ft send [file]`=设路径并启动发。
> **`ft recv/send` 成功会立刻把 uart 切回协议模式**（之后 shell 静音），
> 要回 shell 需重新发 `57 F0 03 00 00 00 00`。
> 握手方向：`'C'` 由**接收方**发出 —— 设备收、PC 发；设备发、PC 收。

### T-FT-01 查状态（初始）
```text
输入: ft
预期: ft state: idle
        recv dir : /lfs
        send path: /lfs/upload.bin
判定: 三项齐全；两条路径为编译期默认值
```

### T-FT-02 设路径被拒（参数校验在最前面拦）
```text
输入: ft send /nope/x.bin
预期: ft: path rejected (out of range)
判定: 父目录不存在 → 拒绝，且**不启动**会话（ft 再查仍是 idle）
输入: ft send relative/path.bin
预期: ft: path rejected (out of range)      # 必须绝对路径
```

### T-FT-03 未知子命令
```text
输入: ft bogus
预期: usage: ft | ft recv [dir] | ft send [file]
        with arg: set path then start; without: start with saved path
```

### T-FT-04 启动接收（PC → 设备 /lfs）
```text
步骤1: ft recv /lfs
预期:  ft: recv dir = /lfs
       ft: recv starting, uart -> protocol mode
       [protocol mode]
       ← 之后 shell 静音（已回协议模式）
步骤2: PC 侧跑 ymodem 发送端：
       sb /path/to/foo.bin          # 或 lsz --ymodem /path/to/foo.bin
预期:  PC 侧显示传输完成；设备侧无线上升输出（仅 ymodem 字节）
步骤3: 重新进 shell： 57 F0 03 00 00 00 00
步骤4: ls /lfs          → 出现 f <size> foo.bin
       ft               → ft state: idle
判定: 文件名以 PC 给的为准（设备用 FILE_INFO 里的名字），落在 /lfs 下
```

### T-FT-05 启动发送（设备 → PC）
```text
步骤1: ft send /lfs/foo.bin
预期:  ft: send path = /lfs/foo.bin
       ft: send starting, uart -> protocol mode
       [protocol mode]
步骤2: PC 侧先起 ymodem 接收端（它负责发 'C'）：
       rb /tmp/          # 或 lrz --ymodem -O /tmp
       （设备收到 'C' 后开始发帧）
预期:  PC 侧收到 foo.bin，内容与 /lfs/foo.bin 一致
步骤3: 重新进 shell → ft → ft state: idle
判定: 若 PC 侧迟迟收不到数据：检查是否先发了 'C'（设备在等它）
```

### T-FT-06 不带参数复用上次路径
```text
前置: 已成功执行过 T-FT-04
步骤: ft recv            # 不带目录
预期: ft: recv starting, uart -> protocol mode
判定: 不再打印 "recv dir = ..."（没写参数），但会话照常启动 → 说明路径缓存粘性生效
```

### T-FT-07 传输中 / 结束后的状态观察
```text
输入: (传输结束后回到 shell) ft
预期: ft state: idle      # 会话正常收尾后链解锁
说明: 若长时间停在 recv/send，多为对端静默消失（ymodem 固有问题）；
      重新 ft recv/send 会整块复位上一会话（设计上不拒绝重启）
```

---

## 6. letter-shell 内置命令（顺带验证后端集成）

### T-BI-01 help
```text
输入: help
预期: 打印命令列表（含 md/mw/ps/stacks/ft/ls/cat/... 与内置命令）
判定: 能看到本次新增的 17 条命令 → 证明 shellCommand 段被正确 KEEP
```

### T-BI-02 help 单条
```text
输入: help md
预期: 打印 md 的用法/描述
```

### T-BI-03 cmds / users / vars / keys
```text
输入: cmds      → Command List: + 逐条命令
输入: users     → User List: + root
输入: vars      → Var List: （无导出变量时只有标题行）
输入: keys      → Key List: + 若干按键项
判定: 四条都能正常列出，无 HardFault
```

### T-BI-04 setVar（无导出变量时）
```text
输入: setVar
预期: 参数不足提示或 not a var 类提示（无 var 可设置）
判定: 不崩溃即可（本固件未导出 var，属预期）
```

### T-BI-05 clear
```text
输入: clear
预期: 终端清屏（发送 ANSI 清屏序列），随后重新出现 root:/$ 
```

### T-BI-06 超长行
```text
输入: 连续输入 >85 个字符再回车
预期: Warning: Command is too long
判定: 不越界、不崩溃
```

### T-BI-07 历史与行编辑
```text
输入: 按 ↑ 取回上一条命令；按退格删除字符
预期: 能回显并执行；退格正常删字符
判定: 证明 0x1B/0x7F 等按键映射与回显正常
```

---

## 7. 已知非缺陷现象（实测中不要当 BUG 报）

| 现象 | 原因 |
| --- | --- |
| 上电瞬间 uart1 出现 `\r\nroot:/$ ` | `shell_init()` 打了一次提示符（见 0.3） |
| `md`/`cat`/`dump` 输出中间夹着日志行 | shell 与 EasyLogger 各持一把输出锁，不互斥 |
| `ft recv/send` 之后 shell 没反应了 | 已自动切回协议模式；重新发 `57 F0 03 ...` 回来 |
| `ps`/`stacks` 永远报 not supported | 未开 `CONFIG_THREAD_MONITOR / THREAD_STACK_INFO / INIT_STACKS`（按设计） |
| `mv` / `df` 报 Command not Found | `VFS_ENABLE_RENAME / VFS_ENABLE_STATVFS` 默认 0，命令未注册 |
| `cat` 大文件时出现 `... truncated at 4096 bytes` | 输出封顶（命令跑在接收线程里） |
| `cat /lfs`（目录）报 EISDIR | 打开目录不是 cat 能做的事 |
| `ls /` 只有两行、看不到文件 | `/` 是虚拟根，只列挂载卷；文件在 `/lfs` 里 |

---

## 8. 失败时的最快定位

| 现象 | 先查什么 |
| --- | --- |
| 发 0xF003 没任何回应 | 是否在协议模式？地址字节序对不对（`57 F0 03 ...`）？uart1 波特率 |
| 进了 shell 但敲键盘没回显 | `shell_rx_feed` 是否被调（模式是否真的是 SHELL）；`uart_pipe` 是否在收 |
| 所有命令都 Command not Found | `shellCommand` 段没被 KEEP → 查链接脚本 include（`shell_sections_zephyr.ld`） |
| `md`/`mw` 全被拒 | 启动日志有没有 `mem whitelist not configured`；白名单内容是否包含该地址 |
| `help` 里缺命令 | 对应 `.c` 是否在 `CMakeLists` 源列表；`VFS_ENABLE_SHELL` 是否为 1 |
| `ft` 启动即失败 | `ft` 先看两条路径；`path rejected` 说明参数校验没过（父目录必须存在） |

---

## 9. 实测记录表

| 用例 | 结果 (PASS/FAIL) | 实际输出摘要 | 备注 |
| --- | --- | --- | --- |
| T-MODE-01 | | | |
| T-MODE-02 | | | |
| T-MODE-03 | | | |
| T-MODE-04 | | | |
| T-SYS-01 | | | |
| T-SYS-02 | | | |
| T-SYS-03 | | | |
| T-SYS-04 | | | |
| T-SYS-05 | | | |
| T-MEM-01 | | | |
| T-MEM-02 | | | |
| T-MEM-03 | | | |
| T-MEM-04 | | | |
| T-MEM-05 | | | |
| T-MEM-06 | | | |
| T-MEM-07 | | | |
| T-MEM-08 | | | |
| T-MEM-09 | | | |
| T-MEM-10 | | | |
| T-MEM-11 | | | |
| T-MEM-12 | | | |
| T-FS-01 | | | |
| T-FS-02 | | | |
| T-FS-03 | | | |
| T-FS-04 | | | |
| T-FS-05 | | | |
| T-FS-06 | | | |
| T-FS-07 | | | |
| T-FS-08 | | | |
| T-FS-09 | | | |
| T-FS-10 | | | |
| T-FS-11 | | | |
| T-FS-12 | | | |
| T-FT-01 | | | |
| T-FT-02 | | | |
| T-FT-03 | | | |
| T-FT-04 | | | |
| T-FT-05 | | | |
| T-FT-06 | | | |
| T-FT-07 | | | |
| T-BI-01 | | | |
| T-BI-02 | | | |
| T-BI-03 | | | |
| T-BI-04 | | | |
| T-BI-05 | | | |
| T-BI-06 | | | |
| T-BI-07 | | | |
