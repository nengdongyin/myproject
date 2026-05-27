# 参考材料目录 (docs/ref/)

本目录存放嵌入式开发所需的参考材料。`embedded-design` Skill 在设计新模块前会**自动检测并读取**相关文件。

## 目录结构

```
docs/ref/
├── README.md              # 本文件
├── coding-style.md        # 团队编码规范（优先级高于 Skill 默认规范）
├── devices/               # 外设器件手册摘要
├── protocols/             # 通信协议 / 第三方库 API 速查
├── patterns/              # 团队设计模式 / 代码模板
└── datasheets/            # MCU/FPGA 芯片手册关键页摘要
```

## 文件格式

所有文件使用 Markdown，器件参数和 API 签名使用代码块标注。

### coding-style.md 示例

```markdown
# 团队编码规范

## 命名（覆盖默认规范）
- 模块前缀：小写 + 下划线（如 `uart_`, `spi_`）
- 枚举值：模块前缀_含义（如 `UART_BAUD_115200`）

## 缩进
- 4 空格，不用 Tab
```

### devices/ 器件手册摘要示例

```markdown
# AT24C02 EEPROM

## 关键参数
| 参数 | 值 |
|------|-----|
| 容量 | 2 Kbit (256 × 8) |
| I2C 地址 | 0x50-0x57 (A0/A1/A2 决定低 3 位) |
| 页大小 | 8 字节 |
| 写周期 | 最大 5 ms |
| 擦写寿命 | 100 万次 |

## I2C 时序
- 标准模式：100 kHz
- 快速模式：400 kHz

## 写操作流程
1. 发送 START + 设备地址(W)
2. 发送内存地址（1 字节）
3. 发送数据（≤ 8 字节，不跨页）
4. 发送 STOP
5. 等待 5 ms（ACK 轮询或固定延迟）
```

### protocols/ API 速查示例

```markdown
# lwIP TCP API (v2.1.x)

## 核心函数签名
\```c
struct tcp_pcb *tcp_new(void);
err_t tcp_bind(struct tcp_pcb *pcb, const ip_addr_t *ipaddr, u16_t port);
struct tcp_pcb *tcp_listen(struct tcp_pcb *pcb);
void tcp_accept(struct tcp_pcb *pcb, err_t (*accept)(void *arg, struct tcp_pcb *newpcb, err_t err));
err_t tcp_write(struct tcp_pcb *pcb, const void *dataptr, u16_t len, u8_t apiflags);
err_t tcp_output(struct tcp_pcb *pcb);
err_t tcp_close(struct tcp_pcb *pcb);
\```

## 回调类型
\```c
typedef err_t (*tcp_accept_fn)(void *arg, struct tcp_pcb *newpcb, err_t err);
typedef err_t (*tcp_recv_fn)(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err);
typedef void  (*tcp_err_fn)(void *arg, err_t err);
\```
```

### patterns/ 设计模式模板示例

```markdown
# 驱动模块标准模板

每个外设驱动模块必须包含以下文件：
- `drv_xxx.h`   — 公共 API
- `drv_xxx.c`   — 实现
- `drv_xxx_conf.h` — 编译期配置

## .h 必须包含
\```c
typedef struct drv_xxx_ctx_t drv_xxx_ctx_t;  // 不透明指针

int drv_xxx_init(drv_xxx_ctx_t *ctx, const drv_xxx_cfg_t *cfg);
int drv_xxx_deinit(drv_xxx_ctx_t *ctx);
// ... 核心操作 API
\```
```
```

## 维护

- 新增器件时，从 datasheet 中提炼关键参数写入 `devices/`，不要直接放 PDF
- 协议/库升级时，更新 `protocols/` 中的 API 签名
- `coding-style.md` 的优先级高于 Skill 内置规范，有冲突以此为准
