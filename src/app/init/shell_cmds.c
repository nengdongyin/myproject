/**
 * @file shell_cmds.c
 * @brief 调试 shell 的业务命令 —— 门面命令注册的唯一样例
 *
 * 本文件只 include `shell_api.h` (门面), 不认识 letter-shell 的任何头文件:
 * 命令怎么注册、参数怎么转换全部在门面后面, 换后端本文件零修改 ——
 * 这正是 shell_api.h 头注释里"调用点零修改"的兑现方式。
 *
 * 目前只放两条"看一眼就够"的诊断命令, 用来验证三件事:
 *   1. `SHELL_EXPORT_CMD` 的 `shellCommand` 段真的被链接器 KEEP 住了
 *      (Zephyr: shell_sections_zephyr.ld; FreeRTOS/GCC: shell_sections_gcc.ld)
 *      —— `help` / `cmds` 能看到它们;
 *   2. 通信串口模式切换 → `shell_rx_feed()` 的输入链路通
 *      (console_mode: WRITE 0xF003 进 shell / `uart_mode proto` 切回);
 *   3. shell 输出能落到 uart1 (与 EasyLogger 共用同一个 sink)。
 *
 * @note 命令在协议接收线程里同步执行 (通信串口切到 shell 模式后, 由
 *       protocol_parser_thread 直接 shell_rx_feed), 所以这里的实现必须短、
 *       不得阻塞、不得等事件 —— 慢活要丢给别的任务去做。
 */

#include "shell_api.h"
#include "shell_parse.h"
#include "app_config.h"
#include "file_transfer.h"
#include "param_dump.h"

/* shell 门面整体关闭时 (SHELL_BACKEND_NONE): 命令注册宏与 shell_printf 都退化为
   空宏, 本文件的命令函数会变成未引用的 static → 干脆整块编译掉, 不占 ROM。 */
#if !defined(SHELL_BACKEND_NONE)

/**
 * @brief 文件传输会话状态的可读名
 *
 * @param state file_transfer_get_state() 的返回值
 * @return 静态字符串 (不释放)
 */
static const char *ft_state_name(ft_state_t state)
{
    switch (state)
    {
    case FT_STATE_RECV: return "recv";
    case FT_STATE_SEND: return "send";
    case FT_STATE_IDLE:
    default:            return "idle";
    }
}

/**
 * @brief status —— 打印累计接收字节数 / 协议链状态 / 文件传输会话状态
 *
 * @note 命令名用 `status` 而不是 `stat`: `stat` 已被 core/vfs/vfs_shell.c 的
 *       文件 stat 占用, 两处同名会在链接期重复定义 `shellCmd_stat`
 *       (SHELL_EXPORT_CMD 按命令名生成全局符号)。
 *
 * @return 0
 */
static int shell_cmd_status(void)
{
    shell_printf("rx bytes : %u\r\n", (unsigned)app_ctx.total_rx_bytes);
    shell_printf("chain    : %s\r\n", app_ctx.chain ? "up" : "down");
    shell_printf("ft state : %s\r\n", ft_state_name(file_transfer_get_state()));
    return 0;
}
SHELL_CMD(status, shell_cmd_status, show rx and file-transfer status);

/**
 * @brief dump —— 打印参数系统的当前值
 *
 * @param argc 参数个数 (含命令名)
 * @param argv argv[1] = 模块 ID (十进制, 省略或 0 = 全部模块)
 * @return 0
 *
 * @note 逐行文本经 param_dump_set_output() 的回调走日志落到 uart1,
 *       因此输出前半段会与 shell 的提示符交错 —— 本工程 log 与 shell
 *       目前各持一把输出锁 (见 shell_config.h 的 SHELL_USING_LOCK 注释)。
 */
static int shell_cmd_dump(int argc, char *argv[])
{
    uint16_t module = 0;

    if (argc > 1)
    {
        uint32_t tmp;
        if (shell_arg_u32(argv[1], &tmp))
            module = (uint16_t)tmp;
    }

    param_dump(module, NULL);
    return 0;
}
SHELL_CMD_MAIN(dump, shell_cmd_dump, dump params of one module or all);

#else /* SHELL_BACKEND_NONE */

/* 编译为空: ISO C 不允许完全空的翻译单元, 放一个 typedef 兜底 */
typedef int shell_cmds_disabled_t;

#endif /* !SHELL_BACKEND_NONE */
