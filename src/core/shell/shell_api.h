/**
 * @file shell_api.h
 * @brief 交互式 Shell 抽象门面 —— 全工程唯一的 shell API 表面
 *
 * @par 为什么需要这一层 (对应"是否需要抽象封装"的结论)
 * `src/lib/` 下已经同时存在两套 shell: letter-shell (本后端) 与
 * nr_micro_shell; `prj.conf` 还开着 Zephyr 内核 shell。三者的命令注册宏
 * (`SHELL_EXPORT_CMD` vs `cmd_table[]` vs Zephyr `SHELL_CMD_*`)、初始化方式、
 * 输入投喂方式 (中断投喂 vs 任务自旋读 vs 回调读) 全都不一样。若业务代码
 * 直接依赖某一套, 换后端时所有调用点都要改。
 *
 * 本门面与 `log_api.h` 完全同构: core/ 只认 `shell_api.h`, 换后端只改本文件
 * 的"后端选择宏", 调用点 (命令定义、初始化、投喂、打印) 零修改。与 log 的
 * `LOG_BACKEND_*`、param 的 `PARAM_*_ENABLE` 一样是"编译期互斥": 同一时刻只
 * 有一个 shell 后端生效, 未选中的必须从 ROM 里整体消失。
 *
 * @par 后端选择 (编译期二选一, 可用 -D 覆盖)
 *   - SHELL_BACKEND_LETTER (默认): letter-shell 3.2.4
 *   - SHELL_BACKEND_NONE:            全部编译为空 (等价于关 shell)
 *   (预留) SHELL_BACKEND_NR_MICRO:   将来接入 nr_micro_shell 时在此映射
 *
 * @par 依赖纪律 / RTOS 无关 / 高可移植
 * 门面与移植层只依赖 `port.h` (锁 / 内存 / 时间), 不 include 任何 RTOS 头。
 * 字节 I/O 用"注入"而非"持有物理通道句柄":
 *   - 输出: `shell_sink_set()` 注入字节出口 (与 log_sink_fn / ft_tx_fn 同签名);
 *   - 输入: `shell_rx_feed()` 注入字节入口 (与 protocol_chain_feed 同构)。
 * 不创建线程、不持有 UART: 裸机主循环 or RTOS 任务都通过 `shell_task()`
 * 这个统一周期入口驱动, 具体怎么跑由 app 层决定。
 *
 * @par 通信串口复用 (应急调试链路)
 * `shell_rx_feed()` 是**唯一**字节入口, 所以"shell 从哪条链路来"完全由
 * app 层决定, 门面不做任何假设:
 *   - 独立调试串口: ISR → 管道 → 线程, 线程里直接 shell_rx_feed();
 *   - 通信串口 (本工程): 与协议解析**时分复用** —— core/console 的
 *     console_mode 决定该串口的字节交给协议链还是 shell, 协议侧用
 *     WRITE 0xF003 切入, shell 侧用 `uart_mode proto` 切回。
 * 输出侧同理: 调试口与通信口的 sink 都只是 `shell_sink_fn`, 由 app 决定
 * shell 的回应发往哪条链路。
 *
 * @note **不要**把同一批字节同时喂给协议链和 shell。看起来只是"多解析一遍",
 *       实际上会同时踩三件事: (1) letter-shell 逐字符回显 → 协议二进制数据
 *       被回显到线上; (2) 0x0A/0x0D 在 letter-shell 里是"回车"键 →
 *       二进制垃圾会被当命令行执行; (3) 'R'(0x52)/'W'(0x57) 正是 Imperx
 *       帧头 → 敲几个字符就能凑出完整帧并触发参数读写。必须二选一。
 *
 * @par 装配顺序 (app 层 init)
 * @code
 *   system_mutex_init();              // port.h, 先于依赖锁的子系统 (与 log 一致)
 *   shell_sink_set(dbg_uart_write);   // 注入字节出口 (可为 NULL, 静默丢弃)
 *   shell_init();                     // 初始化 letter-shell 实例
 *   ...
 *   // 收字节处 (调试串口线程 / 协议透传回调):
 *   shell_rx_feed(buf, len);
 *   // 周期维护处 (裸机主循环 / RTOS 任务, 中断投喂模型下可为空操作):
 *   shell_task();
 * @endcode
 *
 * @par 命令定义 (后端无关, 调用点零修改)
 * @code
 *   int reboot(int delay_ms) { ... return 0; }
 *   SHELL_CMD(reboot, reboot, reboot after delay ms);
 *
 *   int dump(int argc, char *argv[]) { ... return 0; }
 *   SHELL_CMD_MAIN(dump, dump, dump diagnostic info);
 * @endcode
 *
 * @par 命令段 (shellCommand) —— 移植时唯一必须动链接脚本的地方
 * `SHELL_EXPORT_CMD` 把命令描述符放进名为 `shellCommand` 的**输入段**,
 * `shellInit()` 靠 `_shell_command_start` / `_shell_command_end` 两个符号
 * 求段边界。除 IAR (`SHELL_SECTION` 自带 `@` 语法) 外, 链接器必须显式
 * 保留该段: GCC 工程普遍开 `-ffunction-sections -fdata-sections +
 * --gc-sections`, 没有 KEEP 会把整段当垃圾回收 —— 现象是"命令编译进去了,
 * 上电后 help 里却看不到", 且链接期毫无报错。
 *
 *   - GCC / FreeRTOS / 裸机: 把同目录的 shell_sections_gcc.ld `INCLUDE`
 *     进工程链接脚本的 SECTIONS 块 (.rodata 之后即可):
 *     @code
 *       SECTIONS
 *       {
 *           .rodata : { *(.rodata .rodata.*) }
 *           INCLUDE "src/core/shell/shell_sections_gcc.ld"
 *           .data   : { *(.data .data.*) }
 *       }
 *     @endcode
 *   - Zephyr: shell_sections_zephyr.ld, 由 CMake 的
 *     `zephyr_linker_sources(SECTIONS ...)` 注入 (本仓库当前形态)。
 *   - ARMCC (Keil): target/Linker 选项加 `--keep shellCommand*`。
 *
 * @par 后端自身的编译单元必须与应用共享同一份配置 (最容易踩的坑)
 * 覆盖项在 shell_config.h, 通过 `SHELL_CFG_USER="shell_config.h"` 让
 * letter-shell 自己 include 它。**这个宏必须全局注入**, 因为 shell.c 会按
 * `SHELL_DEFAULT_USER` 导出默认用户描述符: 若 shell.c 拿不到同一份配置,
 * shellInit() 按 "root" 找用户就会查不到。
 *   - GCC/Makefile: `CFLAGS += -DSHELL_CFG_USER='"shell_config.h"'`
 *   - CMake:        `target_compile_definitions(... "SHELL_CFG_USER=\"shell_config.h\"")`
 *
 * @par 非 Zephyr 构建 (FreeRTOS + GCC) 只需额外给三样东西
 * 门面与移植层只依赖 port.h, 所以换到 FreeRTOS/裸机工程时:
 *   1. port.h 的实现 —— 直接用 platform/port/port_impl.c 的 PORT_OS_FREERTOS
 *      分支 (句柄式互斥 sys_mutex_* / system_malloc / system_free 已就绪);
 *   2. `system_get_time_ms()` —— 供 SHELL_GET_TICK() 用 (FreeRTOS 下
 *      `xTaskGetTickCount() * portTICK_PERIOD_MS`)。该函数由 **app 层**
 *      提供, port_impl.c 不定义 (Zephyr 版在 src/app/main.c), 与 log /
 *      ymodem / protocol_parser 共用同一个实现;
 *   3. 字节出口 (任一 `void (*)(const uint8_t *, uint32_t)`) 注入给
 *      shell_sink_set(); 字节入口 (UART 收数线程/ISR→队列) 调 shell_rx_feed()。
 * 需要参与编译的单元只有三个: letter-shell 的 shell.c + shell_ext.c, 加本
 * 目录的 shell_letter_port.c (shell_companion / shell_cmd_list 在
 * SHELL_USING_COMPANION=0 / SHELL_USING_CMD_EXPORT=1 下不需要)。
 */

#ifndef CORE_SHELL_API_H
#define CORE_SHELL_API_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 *  后端选择
 * ================================================================ */

#ifndef SHELL_BACKEND_LETTER
    #define SHELL_BACKEND_LETTER 1
#endif

/* ================================================================
 *  字节出口 sink —— 与 log_sink_fn / ft_tx_fn 同构
 * ================================================================ */

/** @brief 字节出口回调 (与 log_sink_fn / ft_tx_fn 同签名, 可互传) */
typedef void (*shell_sink_fn)(const uint8_t *data, uint32_t len);

/* ================================================================
 *  门面 API —— 调用面唯一入口
 * ================================================================ */

#if SHELL_BACKEND_LETTER
    /* ---- letter-shell ---- */
    /* SHELL_CFG_USER 通常由 CMake 全局注入 (letter-shell 官方 overlay 方式),
       确保 shell.c / shell_ext.c 等后端自身 TU 也能拿到配置; 此处兜底,
       使本头文件在不经 CMake 编译时 (如 PC 单测) 依然自洽。 */
    #ifndef SHELL_CFG_USER
        #define SHELL_CFG_USER "shell_config.h"
    #endif
    #include "shell.h"

    /** @brief 注入字节出口 (写入 sink); 传 NULL 表示静默丢弃 */
    void shell_sink_set(shell_sink_fn fn);

    /** @brief 初始化 shell (依赖: port.h 锁可用、sink 已注入)
     *  @return 0 成功; -1 失败 (当前 letter-shell 后端恒成功, 保留返回) */
    int  shell_init(void);

    /** @brief 反初始化并释放锁资源 */
    void shell_deinit(void);

    /** @brief 字节入口投喂 (调试串口主链路 / 通信串口透传应急链路共用)
     *  @note 请在**线程上下文**调用 (内部走互斥锁, 不可在 ISR 里持锁);
     *        中断场景先入管道, 由线程消费后投喂 —— 与本工程 protocol_parser
     *        的 uart_pipe + 线程 模式一致。 */
    void shell_rx_feed(const uint8_t *data, uint32_t len);

    /** @brief 直接写字节 (命令内可用; 不额外加锁) */
    void shell_write(const uint8_t *data, uint32_t len);

    /** @brief 格式化输出 (命令内可用; 等价 letter-shell shellPrint 语义) */
    void shell_printf(const char *fmt, ...);

    /** @brief 程序化执行一条命令 (等价在终端输入一行) */
    int  shell_exec(const char *cmd);

    /** @brief 周期维护入口 —— 裸机主循环或 RTOS 任务中调用。
     *  中断投喂模型 (SHELL_TASK_WHILE=0) 下退化为空操作, 但保留该入口,
     *  以便将来切换到"任务自旋读"后端时调用点不变。 */
    void shell_task(void);

    /* ================================================================
     *  命令注册宏 (后端无关映射 → letter-shell SHELL_EXPORT_*)
     *
     *  覆盖 95% 场景; 需要权限/用户/按键/函数签名等高级特性时, 可在业务
     *  模块里直接使用 letter-shell 原生宏 (代价: 该模块与 letter 后端耦合)。
     * ================================================================ */

    /** @brief 普通 C 函数形式命令 (自动参数转换) */
    #define SHELL_CMD(_name, _func, _desc) \
        SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0)|SHELL_CMD_TYPE(SHELL_TYPE_CMD_FUNC), \
                         _name, _func, _desc)

    /** @brief main 形式命令 (argc/argv) */
    #define SHELL_CMD_MAIN(_name, _func, _desc) \
        SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0)|SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN), \
                         _name, _func, _desc)

    /** @brief int 型变量导出 */
    #define SHELL_VAR_INT(_name, _var, _desc) \
        SHELL_EXPORT_VAR(SHELL_CMD_PERMISSION(0)|SHELL_CMD_TYPE(SHELL_TYPE_VAR_INT), \
                         _name, _var, _desc)

    /** @brief string 型变量导出 (需保证空间充足) */
    #define SHELL_VAR_STRING(_name, _var, _desc) \
        SHELL_EXPORT_VAR(SHELL_CMD_PERMISSION(0)|SHELL_CMD_TYPE(SHELL_TYPE_VAR_STRING), \
                         _name, _var, _desc)
#else
    /* ---- 关 shell: 全部编译为空 ----
     * 命令注册宏在文件作用域使用, 必须展开为空 (而非 ((void)0) 表达式),
     * 与 letter-shell 自身 SHELL_USING_CMD_EXPORT=0 分支的空宏一致。 */
    #define SHELL_CMD(_name, _func, _desc)
    #define SHELL_CMD_MAIN(_name, _func, _desc)
    #define SHELL_VAR_INT(_name, _var, _desc)
    #define SHELL_VAR_STRING(_name, _var, _desc)

    #define shell_sink_set(fn)                       ((void)(fn))
    #define shell_init()                             (0)
    #define shell_deinit()                           ((void)0)
    #define shell_rx_feed(data, len)                 ((void)(data), (void)(len))
    #define shell_write(data, len)                   ((void)(data), (void)(len))
    #define shell_printf(...)                        ((void)0)
    #define shell_exec(cmd)                          (0)
    #define shell_task()                             ((void)0)


#endif /* SHELL_BACKEND_NONE */

#ifdef __cplusplus
}
#endif

#endif /* CORE_SHELL_API_H */
