/**
 * @file diag_init.c
 * @brief 诊断设施装配实现 —— 日志 / shell / md-mw 白名单
 *
 * @details
 * 顺序即依赖, 逐条说明 (改顺序前先读):
 *   1. 日志: 无它则后两步的结果不可见;
 *   2. shell: 命令入口, 也是 host 进入应急调试链路的落点;
 *   3. mem_regions 白名单: 只决定 md/mw 是否放行, 失败仅告警 —— 这是有意的
 *      fail-closed 设计, 不做"默认全放开"。
 *
 * 出口统一走本文件的 diag_tx (调试口恒输出 + 通讯口仅 shell 模式): 若改注
 * bsp_comm_write 会让协议模式下的提示符/日志灌进协议流。
 *
 * @note **不含** param_dump 输出接线: `param_dump()` 的第一个调用者是
 *       `param_manager_init()` 尾部, 那条接线属于参数子系统自己的配置
 *       (见 param_manager_init.c 的 dump_to_log)。放这里会引入一个无谓的
 *       顺序依赖 —— 一旦本函数被排到参数初始化之后, 启动 dump 就被静默丢掉。
 */

#include "diag_init.h"
#include "bsp_uart.h" /* 两个物理口的裸写能力 (platform/bsp) */
#include "console_mode.h" /* 选路: 通讯口只在 shell 模式输出 */
#include "log_api.h" /* 门面: LOG_* 不直接用 Zephyr log.h —— 两套都定义 LOG_RAW */
#include "shell_api.h"
#include "mem_regions.h"

/* ================================================================
 *  诊断输出的选路 (应用策略, 不是 bsp 能力)
 * ================================================================ */

/**
 * @brief shell / log 共用的扇出出口
 *
 *  - 调试口: **恒定输出** (与串口模式无关, 排障时总能看到全量);
 *  - 通讯口: **仅 CONSOLE_MODE_SHELL 时输出** —— 协议模式下必须静默, 否则
 *    提示符 / 回显 / 日志都会灌进协议流, 被主机当成帧解析。
 *
 * @note 选路在“写入时刻”判定, 所以模式切换 (0xF003 进 shell、`uart_mode proto`
 *       回协议、file_transfer 借用/归还串口) 都不需要在切换处埋点。
 * @note 为何在 app 而不在 bsp: 这里依赖 core 的 console_mode; 下沉到
 *       platform/bsp 会形成 platform → core 的反向依赖。
 * @note **协议帧不走本函数** —— 它必须只进通讯口且协议模式下必须发。
 */
static void diag_tx(const uint8_t *data, uint32_t len)
{
    bsp_dbg_write(data, len);

    if (console_mode_get() == CONSOLE_MODE_SHELL)
    {
        bsp_comm_write(data, len);
    }
}

/* ================================================================
 *  装配
 * ================================================================ */

bool diag_init(void)
{
    /* 1) 日志: 注入扇出出口并初始化 (EasyLogger elog_init + elog_start) */
    log_sink_set(diag_tx);
    if (log_init() != 0)
    {
        /* 日志是最底层的诊断出口: 起不来就没有任何可报告的通道了, 直接失败。
           此时 LOG_* 不可用, 只能用 bsp 裸写报一句。 */
        static const char msg[] = "[diag] log init failed\r\n";
        bsp_dbg_write((const uint8_t *)msg, sizeof(msg) - 1);
        return false;
    }
    LOG_I("boot", "EasyLogger up");

    /* 2) shell: 只注入出口 + 初始化。字节**入口**由 core/console 的 console_mode
          分流 (协议模式不喂 shell, 见 protocol_parser_thread.c) */
    shell_sink_set(diag_tx);
    if (shell_init() != 0)
    {
        LOG_E("diag", "shell init failed"); /* 日志已可用, 走日志 */
        return false;
    }
    LOG_I("boot", "letter-shell up (WRITE 0xF003 -> shell)");

    /* 3) md/mw 地址白名单: 板级表 → core/debug。失败不中断启动,
          两条命令保持拒绝 (fail-closed) */
    if (mem_regions_configure() != 0)
    {
        LOG_W("diag", "mem whitelist not configured, md/mw will be refused");
    }

    return true;
}

void diag_deinit(void)
{
    /* shell 先关: 之后不再有任何入口能把字节喂进 shellHandler。
       日志不在这里关 —— 退出路径上还要打日志 (由调用方按需处理)。 */
    shell_deinit();
}
