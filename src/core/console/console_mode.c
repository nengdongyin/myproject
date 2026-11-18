/**
 * @file console_mode.c
 * @brief 通信串口复用模式的实现 —— 状态 + 参数旋钮 + shell 命令
 *
 * 两条入口, 状态因此只有一个真相源:
 *   - 协议 → shell: PID_CONSOLE_ENTER_SHELL 一条 EXEC
 *     (地址映射在 protocols/imperx/imperx_cmd_map.c 的 0xF003);
 *   - shell → 协议: shell 命令 `uart_mode proto`。
 *
 * 为什么不提供"协议 → 协议"的旋钮: 进入 shell 模式后 RX 线程不再调用
 * protocol_chain_feed (见 app/init/protocol_parser_thread.c), 协议帧根本
 * 到不了 EXEC 回调 —— 那条命令要么永远走不到, 要么只是"把自己设成已经是
 * 模式的模式", 是空操作。回程因此只有 shell 命令一条路。
 *
 * 依赖纪律: 只依赖 shell_api.h (门面) + param 框架, 不认识 UART、不认识协议链。
 */

#include "console_mode.h"
#include "shell_api.h"
#include "app_param_manager.h"

#include <string.h>

/* ================================================================
 *  状态
 *
 *  单线程持有: 写发生在协议接收线程 (EXEC 回调 / shell 命令), 读也在同一线程。
 *  因此不需要锁, 也不需要 volatile —— 加锁反而会与 shellHandler 已持的锁
 *  形成嵌套 (见 shell_config.h 的 SHELL_USING_LOCK 注释)。
 * ================================================================ */

static console_mode_t s_mode = CONSOLE_MODE_PROTO;

/**
 * @brief 借用槽 —— 谁 save_set 谁 restore。
 *
 * 只有一份: 同一时刻只允许一个"临时占用串口的活动" (目前只有 ymodem 文件
 * 传输)。槽为空时 save_set 抢到并返回 true; 已占用则什么都不做并返回 false。
 */
static struct
{
    console_mode_t saved; /**< save_set 前的模式 (仅在 valid 时有效) */
    bool valid;           /**< 槽是否被占用 */
} s_slot;

console_mode_t console_mode_get(void)
{
    return s_mode;
}
int console_mode_set(console_mode_t mode)
{
    if (mode != CONSOLE_MODE_PROTO && mode != CONSOLE_MODE_SHELL)
    {
        return PARAM_ERR_OUT_OF_RANGE;
    }

#if defined(SHELL_BACKEND_NONE)
    /* shell 被编译关闭: 切过去没有意义, 明确拒绝而不是静默成功 */
    if (mode == CONSOLE_MODE_SHELL)
    {
        return PARAM_ERR_NOT_SUPPORTED;
    }
#endif

    if (mode == s_mode)
    {
        return PARAM_OK; /* 幂等 */
    }

    /* 先落状态再打印: shell 的出口按"写入时刻"的模式选路 (见 app 层的
       log_shell_tx_fanout), 若先打印后赋值, 进入 shell 的那句欢迎语会被判成
       协议模式而只送到调试口 —— 操作者在自己敲命令的那个口上就看不到它。 */
    s_mode = mode;

    /* 提示: letter-shell 只在每条命令之后打提示符, 切换瞬间不会自己输出,
       操作者需要一处可见的落脚点。切回协议时也打一行, 便于确认已离开。 */
    if (mode == CONSOLE_MODE_SHELL)
    {
        shell_printf("\r\n[shell mode] 'help' for commands, "
                     "'uart_mode proto' to return\r\n");
    }
    else
    {
        shell_printf("\r\n[protocol mode]\r\n");
    }

    return PARAM_OK;
}

bool console_mode_save_set(console_mode_t target)
{
    if (s_slot.valid)
        return false; /* 已有借用者: 不覆盖它的原始模式 */

    s_slot.saved = s_mode;
    s_slot.valid = true;

    /* 切换失败 (target 非法 / shell 被编译关闭) 也照样占槽:
       restore 会把状态还回去, 串口不会停在半切换状态 */
    (void)console_mode_set(target);
    return true;
}

void console_mode_restore(void)
{
    if (!s_slot.valid)
        return; /* 幂等: 没有借用就别动串口 */

    const console_mode_t saved = s_slot.saved;
    s_slot.valid = false;
    (void)console_mode_set(saved); /* 幂等: 已是该模式则不打印、不改状态 */
}

/* ================================================================
 *  shell 侧: 查询模式 / 切回协议
 *
 *  只用一条命令而不是两条 (`mode` + `proto`): 两个动词太宽泛, 在 shell 里
 *  有 dump/stat/ls 这些命令的场合下见名不知意。`uart_mode [proto]` 一眼
 *  能读出"这是在管通信串口的解析模式"。
 * ================================================================ */

#if !defined(SHELL_BACKEND_NONE)
/**
 * @brief uart_mode —— 查询通信串口解析模式, 或切回协议解析
 *
 * @param argc 参数个数
 * @param argv argv[1] 省略 = 查询; "proto"/"protocol" = 交还协议链
 * @return 0 成功; -1 参数不认识
 */
static int console_cmd_uart_mode(int argc, char *argv[])
{
    if (argc < 2)
    {
        shell_printf("uart parse mode: %s\r\n",
                     (s_mode == CONSOLE_MODE_SHELL) ? "shell" : "protocol");
        shell_printf("usage: uart_mode proto    hand uart back to protocol\r\n");
        return 0;
    }

    if (strcmp(argv[1], "proto") == 0 || strcmp(argv[1], "protocol") == 0)
    {
        return console_mode_set(CONSOLE_MODE_PROTO);
    }

    shell_printf("uart_mode: unknown '%s'\r\n", argv[1]);
    return -1;
}
SHELL_CMD_MAIN(uart_mode, console_cmd_uart_mode, query or restore uart parse mode);
#endif /* !SHELL_BACKEND_NONE */

/* ================================================================
 *  参数侧: EXEC 旋钮 (协议地址 → param_id → 本回调)
 * ================================================================ */

/**
 * @brief EXEC 回调 —— 唯一落点是切到 shell 模式
 *
 * @param ctx      模块上下文 (未使用)
 * @param param_id PID_CONSOLE_ENTER_SHELL
 * @param arg      EXEC 无载荷, 忽略
 * @return PARAM_OK / PARAM_ERR_NOT_SUPPORTED / PARAM_ERR_NOT_FOUND
 *
 * @note 本回调在协议的 frame_ready 里执行; 框架随后才 encode + tx_ready,
 *       所以协议应答 (ACK) 一定在模式生效之后才发出, 主机不会漏看应答。
 */
static int console_exec_cb(void *ctx, uint32_t param_id, param_value_t arg)
{
    (void)ctx;
    (void)arg;

    switch (param_id)
    {
    case PID_CONSOLE_ENTER_SHELL:
        return console_mode_set(CONSOLE_MODE_SHELL);
    default:
        return PARAM_ERR_NOT_FOUND;
    }
}

/* ---- 参数表 + 模块注册 ---- */
PARAM_EXEC(console_enter_shell, PID_CONSOLE_ENTER_SHELL);

PARAM_TABLE(console_params, &console_enter_shell.base);

PARAM_MODULE_DEFINE(console_mode, MODULE_CONSOLE, "Console",
                    .exec = console_exec_cb);

PARAM_MODULE_INIT(console_mode, console_params);
