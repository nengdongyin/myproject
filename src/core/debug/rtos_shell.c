/**
 * @file rtos_shell.c
 * @brief RTOS 线程 / 栈用量查询命令 —— ps 与 stacks
 *
 * @par 两个命令的分工
 *   - `ps`     : 线程全貌 (名字/状态/优先级/栈大小/峰值占用/剩余比例)
 *   - `stacks` : 只看栈, 按**剩余比例升序**排 —— 最危险的排最前, 一眼看溢出风险
 *
 * @par 依赖纪律
 * 本文件只 include port_rtos.h + shell_api.h: 它不认识 Zephyr 也不认识 FreeRTOS。
 * 线程枚举/栈用量由 port 层实现 (见 port_impl.c 的各 OS 分支), 因此换 RTOS
 * 本文件零修改; 后端不支持时 (未开相应 Kconfig / 无 RTOS) 打印一行说明。
 *
 * @par 为什么静态数组
 * 32 行 × sizeof(port_thread_info_t)(≈44B) ≈ 1.4 KB。放栈上会吃掉接收线程
 * (或调试线程) 的栈, 而这条命令的本意就是查栈 —— 不能自己先制造栈压力。
 */

#include "port_rtos.h"
#include "shell_api.h"

#if !defined(SHELL_BACKEND_NONE)

#include <stdint.h>
#include <string.h>

/** @brief 一次最多列出的线程数 (超出打印 truncated) */
#define RTOS_ROWS_MAX 32

/** @brief 行缓冲: 枚举结果 (静态, 避免占用调用线程栈) */
static port_thread_info_t s_rows[RTOS_ROWS_MAX];

/**
 * @brief 收集线程快照
 *
 * @param count 输出实际行数
 * @return 0 成功; <0 后端不支持 (-ENOTSUP)
 */
static int rtos_collect(size_t *count)
{
    size_t i = 0;

    for (; i < RTOS_ROWS_MAX; i++)
    {
        int rc = port_thread_stat(i, &s_rows[i]);
        if (rc < 0)
            return rc;
        if (rc == 0)
            break;
    }
    *count = i;
    return 0;
}

/**
 * @brief 栈剩余比例 (0..100); 后端不提供总大小时返回 100 (视作"无风险/未知")
 */
static unsigned rtos_free_pct(const port_thread_info_t *t)
{
    if (t->stack_size == 0u || t->stack_free_min >= t->stack_size)
        return 100u;
    return (unsigned)(((uint64_t)t->stack_free_min * 100u) / t->stack_size);
}

/**
 * @brief 按栈剩余比例升序排序 (插入排序, n <= 32)
 */
static void rtos_sort_by_free_pct(port_thread_info_t *a, size_t n)
{
    for (size_t i = 1; i < n; i++)
    {
        port_thread_info_t key = a[i];
        unsigned key_pct = rtos_free_pct(&key);
        size_t j = i;

        while (j > 0 && rtos_free_pct(&a[j - 1]) > key_pct)
        {
            a[j] = a[j - 1];
            j--;
        }
        a[j] = key;
    }
}

/**
 * @brief 后端不支持时的统一提示
 */
static void rtos_report_unsupported(const char *cmd)
{
    shell_printf("%s: not supported by this port build\r\n", cmd);
    shell_printf("      (Zephyr: CONFIG_THREAD_MONITOR + THREAD_STACK_INFO + INIT_STACKS;"
                 " FreeRTOS: configUSE_TRACE_FACILITY)\r\n");
}

/**
 * @brief ps —— 列出所有线程及其关键状态
 * @return 0
 */
static int rtos_cmd_ps(void)
{
    size_t n = 0;
    int rc = rtos_collect(&n);

    if (rc < 0)
    {
        rtos_report_unsupported("ps");
        return 0;
    }

    shell_printf("%-12s %-10s %4s %7s %8s %5s\r\n",
                 "name", "state", "prio", "stack", "usedmax", "free%");
    for (size_t i = 0; i < n; i++)
    {
        const port_thread_info_t *t = &s_rows[i];

        if (t->stack_size > 0u)
        {
            uint32_t used = (t->stack_free_min < t->stack_size)
                                ? (t->stack_size - t->stack_free_min)
                                : 0u;
            shell_printf("%-12s %-10s %4d %7u %8u %4u%%\r\n", t->name, t->state,
                         (int)t->prio, (unsigned)t->stack_size, (unsigned)used,
                         rtos_free_pct(t));
        }
        else
        {
            /* 后端不提供总大小 (FreeRTOS): 只给"历史最小剩余字节" */
            shell_printf("%-12s %-10s %4d %7s %8u %5s\r\n", t->name, t->state,
                         (int)t->prio, "n/a", (unsigned)t->stack_free_min, "n/a");
        }
    }
    if (n == 0)
        shell_printf("(none)\r\n");
    if (n >= RTOS_ROWS_MAX)
        shell_printf("... truncated at %u\r\n", (unsigned)RTOS_ROWS_MAX);
    return 0;
}
SHELL_CMD(ps, rtos_cmd_ps, list all threads with state and stack peak);

/**
 * @brief stacks —— 只看栈用量, 剩余比例最小的排最前
 * @return 0
 */
static int rtos_cmd_stacks(void)
{
    size_t n = 0;
    int rc = rtos_collect(&n);

    if (rc < 0)
    {
        rtos_report_unsupported("stacks");
        return 0;
    }

    /* 先把没有栈信息的行排到后面, 再按剩余比例升序 */
    rtos_sort_by_free_pct(s_rows, n);

    shell_printf("%-12s %7s %8s %5s  %s\r\n",
                 "name", "stack", "usedmax", "free", "");
    for (size_t i = 0; i < n; i++)
    {
        const port_thread_info_t *t = &s_rows[i];
        unsigned pct = rtos_free_pct(t);
        const char *flag = (t->stack_size > 0u && pct < 15u) ? "  <-- low" : "";

        if (t->stack_size > 0u)
        {
            uint32_t used = (t->stack_free_min < t->stack_size)
                                ? (t->stack_size - t->stack_free_min)
                                : 0u;
            shell_printf("%-12s %7u %8u %4u%%%s\r\n", t->name,
                         (unsigned)t->stack_size, (unsigned)used, pct, flag);
        }
        else
        {
            shell_printf("%-12s %7s %8u %5s%s\r\n", t->name, "n/a",
                         (unsigned)t->stack_free_min, "n/a", "");
        }
    }
    if (n == 0)
        shell_printf("(none)\r\n");
    if (n >= RTOS_ROWS_MAX)
        shell_printf("... truncated at %u\r\n", (unsigned)RTOS_ROWS_MAX);
    return 0;
}
SHELL_CMD(stacks, rtos_cmd_stacks, stack usage sorted by least free first);

#endif /* !SHELL_BACKEND_NONE */
