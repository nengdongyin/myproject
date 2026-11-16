/**
 * @file file_transfer_shell.c
 * @brief 文件传输 (ymodem) 的 shell 命令 —— 只有一条 `ft` 命令
 *
 * @par 命令
 * @code
 *   ft                 查看会话状态与当前收/发路径
 *   ft recv [dir]      带 dir: 设置接收目录后立即启动; 不带: 用已保存的目录启动
 *   ft send [file]     带 file: 设置发送文件后立即启动; 不带: 用已保存的路径启动
 * @endcode
 *
 * @par 为什么是"一条命令 + 可选参数"而不是 path/start 两条
 * 参数缓存本身就是粘性的: 设过一次的路径会一直留着 (与协议侧 WRITE 走同一份
 * 缓存), 所以"再启动一次"不需要重复给路径; 而首次使用给一次就等于配好 +
 * 启动, 少打一条命令。这样 `recv` / `send` 一个词只有一种含义 —— 不会再出现
 * "`ft recv` 是启动、`ft path recv` 却是选参数"这种同词异义。
 *
 * @note **带参 = 设了还启动**。只想"配置不启动"的话, 路径缓存已经保存下来了,
 *       下次 `ft recv` / `ft send` 不带参数即可复用 —— 但第一次配置不可避免会
 *       起一次会话 (协议链会锁到 ymodem parser 上)。若确实需要"只存不启",
 *       再单独加一个开关即可。
 *
 * @par 为什么走参数系统而不是直接调 file_transfer_start_*
 * 与协议侧 (Imperx WRITE 0xF000/0xF001) 保持**同一条路径**:
 * 地址 → param_id → param_exec → 组件。这样 shell 与上位机触发的行为完全一致,
 * 路径合法性校验也只在一个地方 (param 的 write 回调) —— 不会出现"shell 绕过了
 * 校验"这种分叉。
 *
 * @par 发起后自动切回协议模式
 * 启动会话后本函数立刻把串口模式切回 `CONSOLE_MODE_PROTO`: 之后的字节流就是纯
 * ymodem, 不需要操作者再敲 `uart_mode proto` (少了那段手敲时间就是一个真正的
 * 竞争窗口 —— 对端一回帧就会进 `shellHandler`)。
 *
 * 代价: 切回协议模式后 shell 在这条串口上就**静音**了, 传输结果看不到;
 * 想查结果需重新用 WRITE 0xF003 进 shell 模式再看 `ft` / `ls`。
 * 若 shell 本来就在独立调试串口上 (串口模式本来就是 protocol), 则什么也不做。
 *
 * @par 握手方向 (ymodem: 'C' 由接收方发)
 *   - `ft recv` → 本机是接收方 → 立即发 'C', 对端要跑发送端 (sb / lsz --ymodem)
 *   - `ft send` → 本机是发送方 → 等对端发 'C', 对端要跑接收端 (rb / lrz --ymodem)
 */

#include "shell_api.h"
#include "file_transfer.h"
#include "param_manager.h"
#include "console_mode.h"

#if !defined(SHELL_BACKEND_NONE)

#include <string.h>

/**
 * @brief 会话状态可读名
 * @param s file_transfer_get_state() 的返回值
 * @return 静态字符串
 */
static const char *ft_shell_state_str(ft_state_t s)
{
    switch (s)
    {
    case FT_STATE_RECV: return "recv";
    case FT_STATE_SEND: return "send";
    case FT_STATE_IDLE:
    default:            return "idle";
    }
}

/**
 * @brief 打印会话状态 + 当前收/发路径
 */
static void ft_shell_print_state(void)
{
    char recv_path[FT_PATH_MAX + 1];
    char send_path[FT_PATH_MAX + 1];

    shell_printf("ft state: %s\r\n", ft_shell_state_str(file_transfer_get_state()));

    if (param_read_string(PID_FT_RECV_PATH, recv_path, sizeof(recv_path)) == PARAM_OK)
        shell_printf("  recv dir : %s\r\n", recv_path);
    if (param_read_string(PID_FT_SEND_PATH, send_path, sizeof(send_path)) == PARAM_OK)
        shell_printf("  send path: %s\r\n", send_path);
}

/**
 * @brief 设置路径参数 (带参形式的公共部分)
 *
 * @param id    PID_FT_RECV_PATH / PID_FT_SEND_PATH
 * @param value 路径字符串 (合法性由 param 的 write 回调校验)
 * @return 0 成功; -1 被拒 (已打印原因)
 *
 * @note 写的是参数**缓存**并标 dirty, 与协议侧 WRITE 同一条路径; 落盘时机由
 *       参数系统的 flush 策略决定 (需要立刻持久化就在调用方补 param_flush())。
 */
static int ft_shell_set_path(uint32_t id, const char *value)
{
    int rc = param_write_string(id, value);

    if (rc != PARAM_OK && rc != PARAM_OK_TRUNCATED)
    {
        shell_printf("ft: path rejected (%s)\r\n", param_strerror(rc));
        return -1;
    }
    return 0;
}

/**
 * @brief 启动一次会话 (收或发)
 *
 * @param is_recv 非 0 = 接收; 0 = 发送
 * @return 0 已发起; -1 失败 (已打印原因)
 *
 * @par 为什么先把模式切回协议链再启动
 * 启动之后线上就是纯 ymodem 字节流 (recv 立刻发 'C', send 收到对端 'C' 后开始
 * 发帧), 任何 shell 打印混进去都属于污染协议流。所以顺序是:
 *   1. 打完所有提示 → 2. 切回 CONSOLE_MODE_PROTO → 3. param_exec 启动
 * 启动成功后**不再打印任何东西**; 失败则切回 shell 再报错 —— 此时没有握手,
 * 线上干净, 且操作者需要留在 shell 里改路径重试。
 *
 * @note 返回成功只代表"握手已布置", 不代表传输完成。
 */
static int ft_shell_start(int is_recv)
{
    int was_shell = (console_mode_get() == CONSOLE_MODE_SHELL);
    int rc;

    /* 1) 提示必须在握手上线之前打完 */
    shell_printf("ft: %s starting%s\r\n", is_recv ? "recv" : "send",
                 was_shell ? ", uart -> protocol mode" : "");

    if (was_shell)
    {
        /* 2) 切回协议链 (内部会打印 [protocol mode], 同样在握手之前) */
        (void)console_mode_set(CONSOLE_MODE_PROTO);
    }

    /* 3) 启动: recv 会在此发 'C'; send 在此进入等 'C' 状态 */
    rc = param_exec(is_recv ? PID_FT_RECV_START : PID_FT_SEND_START, NULL);
    if (rc != PARAM_OK)
    {
        /* 启动失败 → 没有握手上线: 切回 shell 再报错, 让操作者能改路径重试 */
        if (was_shell)
            (void)console_mode_set(CONSOLE_MODE_SHELL);
        shell_printf("ft: start failed (%s)\r\n", param_strerror(rc));
        return -1;
    }

    /* 成功: 这之后一个字都不能再打印 (线上应该只有 ymodem 字节) */
    return 0;
}

/**
 * @brief ft —— 文件传输命令入口
 *
 * @param argc 参数个数
 * @param argv 见文件头命令表
 * @return 0 成功; -1 参数错误或启动失败
 */
static int ft_shell_cmd(int argc, char *argv[])
{
    static const char *usage =
        "usage: ft | ft recv [dir] | ft send [file]\r\n"
        "  with arg: set path then start; without: start with saved path\r\n";

    /* 无参: 只查状态, 不启动任何会话 */
    if (argc < 2)
    {
        ft_shell_print_state();
        return 0;
    }

    if (strcmp(argv[1], "recv") == 0)
    {
        if (argc > 2)
        {
            if (ft_shell_set_path(PID_FT_RECV_PATH, argv[2]) != 0)
                return -1;
            shell_printf("ft: recv dir = %s\r\n", argv[2]);
        }
        return ft_shell_start(1);
    }

    if (strcmp(argv[1], "send") == 0)
    {
        if (argc > 2)
        {
            if (ft_shell_set_path(PID_FT_SEND_PATH, argv[2]) != 0)
                return -1;
            shell_printf("ft: send path = %s\r\n", argv[2]);
        }
        return ft_shell_start(0);
    }

    shell_printf("%s", usage);
    return -1;
}
SHELL_CMD_MAIN(ft, ft_shell_cmd, file transfer: ft | ft recv [dir] | ft send [file]);

#endif /* !SHELL_BACKEND_NONE */
