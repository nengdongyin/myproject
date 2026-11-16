/**
 * @file shell_letter_port.c
 * @brief letter-shell 移植层 —— 门面与 letter-shell 后端之间的唯一桥梁
 *
 * 本文件是 letter-shell 与平台之间的唯一桥梁:
 *   - 输出: 经可注入的 sink (shell_sink_fn) 交给 UART/文件/Flash, 不持有句柄;
 *   - 输入: 经 shell_rx_feed() 逐字节送入 shellHandler() (中断投喂, 不设 read);
 *   - 锁:   复用 port.h 的句柄式互斥锁 (sys_mutex_*, 对 NULL 安全);
 *   - 时间/内存: 复用 port.h 的 system_get_time_ms() / system_malloc/free。
 *
 * 依赖方向: letter-shell(后端) → 本文件(移植) → port.h(平台)。换 RTOS 只改
 * port_impl.c, 本文件与 core/shell 业务代码零修改 —— 与 log_elog_port.c 的
 * "不认识 UART、字节出口经 sink 注入" 是同一纪律。
 */

#include "shell_api.h"
#include "port.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ================================================================
 *  letter-shell 实例: 静态分配, 零堆依赖
 *
 *  shellInit 把 buffer 切成 (SHELL_HISTORY_MAX_NUMBER + 1) 份:
 *  1 份当前命令行 + HISTORY 份历史, 单行最大长度 = 512 / (5 + 1)。
 * ================================================================ */

static Shell s_shell;
static char  s_shell_buffer[512];

/** @brief 字节出口 sink: shell_sink_set() 注入; 未注入时静默丢弃(不死机) */
static shell_sink_fn s_sink = NULL;

/** @brief 输出锁: 复用 port.h 句柄式锁 (对 NULL 安全) */
static sys_mutex_t *s_output_lock = NULL;

/* ================================================================
 *  letter-shell 移植桩 (通过 Shell 对象字段注入, 签名不可改)
 * ================================================================ */

/**
 * @brief shell 写函数 (letter-shell 3.1+ 原型)
 * @return 实际写入字符数
 */
static signed short s_write(char *data, unsigned short len)
{
    if (s_sink)
    {
        s_sink((const uint8_t *)data, (uint32_t)len);
    }
    return (signed short)len;
}

#if SHELL_USING_LOCK == 1
/** @brief shell 加锁 (letter-shell 原型: 接收 Shell*, 返回 0) */
static int s_lock(struct shell_def *shell)
{
    (void)shell;
    sys_mutex_lock(s_output_lock);
    return 0;
}

/** @brief shell 解锁 */
static int s_unlock(struct shell_def *shell)
{
    (void)shell;
    sys_mutex_unlock(s_output_lock);
    return 0;
}
#endif /* SHELL_USING_LOCK == 1 */

/* ================================================================
 *  门面生命周期
 * ================================================================ */

void shell_sink_set(shell_sink_fn fn)
{
    s_sink = fn;
}

int shell_init(void)
{
    if (s_output_lock == NULL)
    {
        s_output_lock = sys_mutex_create();
    }

    /* 中断投喂模型: 不设 read, 字节由 shell_rx_feed → shellHandler 驱动。
     * letter-shell 官方支持此用法 (README: "对于中断方式使用shell,
     * 不用定义 shell->read, 但需要在中断中调用 shellHandler")。 */
    s_shell.read  = NULL;
    s_shell.write = s_write;
#if SHELL_USING_LOCK == 1
    s_shell.lock   = s_lock;
    s_shell.unlock = s_unlock;
#endif

    shellInit(&s_shell, s_shell_buffer, sizeof(s_shell_buffer));
    return 0;
}

void shell_deinit(void)
{
    shellRemove(&s_shell);
    sys_mutex_destroy(s_output_lock);
    s_output_lock = NULL;
    s_sink = NULL;
}

/* ================================================================
 *  字节入口 / 出口 / 执行 / 周期
 * ================================================================ */

void shell_rx_feed(const uint8_t *data, uint32_t len)
{
    if (data == NULL)
    {
        return;
    }
    for (uint32_t i = 0; i < len; i++)
    {
        /* letter-shell 用 0x00 作为按键匹配哨兵, 该字节会被丢弃;
           对 ASCII 交互无影响, 但透传二进制时需注意。 */
        if (data[i] != 0x00u)
        {
            shellHandler(&s_shell, (char)data[i]);
        }
    }
}

void shell_write(const uint8_t *data, uint32_t len)
{
    if (data == NULL || len == 0)
    {
        return;
    }
    s_write((char *)data, (unsigned short)len);
}

void shell_printf(const char *fmt, ...)
{
    char buffer[SHELL_PRINT_BUFFER];
    va_list args;
    int len;

    if (fmt == NULL || SHELL_PRINT_BUFFER == 0)
    {
        return;
    }

    va_start(args, fmt);
    len = vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    if (len <= 0)
    {
        return;
    }
    if ((size_t)len >= sizeof(buffer))
    {
        /* vsnprintf 被截断: 它只写了 sizeof-1 个字符 + '\0', 所以真正有效的
           字节数是 sizeof-1; 若按 sizeof 送出去会多送那个终止符。 */
        len = (int)sizeof(buffer) - 1;
    }
    s_write(buffer, (unsigned short)len);
}

int shell_exec(const char *cmd)
{
    if (cmd == NULL)
    {
        return -1;
    }
    return shellRun(&s_shell, cmd);
}

void shell_task(void)
{
    /* SHELL_TASK_WHILE=0 且 read==NULL → 单次调用为空操作。
       保留此入口: 换"任务自旋读"后端时调用点不变。 */
    shellTask(&s_shell);
}
