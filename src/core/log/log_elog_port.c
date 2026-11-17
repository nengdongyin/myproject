/**
 * @file log_elog_port.c
 * @brief EasyLogger 移植层 —— 8 个 port 桩 + sink 注入
 *
 * 本文件是日志后端与平台之间的唯一桥梁:
 *   - 输出: 经可注入的 sink (log_sink_fn) 交给 UART/Flash, 不持有句柄
 *   - 落盘: 转交 log_file.c (双文件轮换, 与日志库无关的通用模块), 本文件
 *           只在自己的字节出口处调它一行
 *   - 锁:   复用 port.h 的句柄式互斥锁 (sys_mutex_*, 对 NULL 安全)
 *   - 时间: 复用 port.h 的 system_get_time_ms()
 *
 * 依赖方向: elog.c(后端) → 本文件(移植) → port.h(平台)。换 RTOS 只改
 * port_impl.c, 本文件与 core/ 业务代码零修改 —— 与 file_transfer 的
 * "不认识 UART、字节出口经 ft_tx_fn 注入" 是同一纪律。
 *
 * 落盘为什么不在本文件: 轮换策略与日志库无关 (只依赖 VFS + 时间), 写在
 * 这里会让"换日志后端 (LOG_BACKEND_*) 就丢文件日志"。见 log_file.h。
 */

#include <elog.h>
#include <stdio.h>
#include "log_api.h"
#include "log_file.h"
#include "port.h"

/* 输出 sink: 由 log_sink_set() 注入; 未注入时静默丢弃(不死机) */
static log_sink_fn s_sink = NULL;

/* 输出锁: 复用 port.h 句柄式锁 (sys_mutex_lock/unlock 对 NULL 安全) */
static sys_mutex_t *s_output_lock = NULL;

/* 时间字符串缓冲: elog 直接用 strcpy 拷贝返回指针, 必须是静态 */
static char s_time_buf[16];

/* ================================================================
 *  门面生命周期
 * ================================================================ */

void log_sink_set(log_sink_fn fn)
{
    s_sink = fn;
}

int log_init(void)
{
    if (elog_init() != ELOG_NO_ERR)
    {
        return -1;
    }
    elog_start();
    return 0;
}

/* ================================================================
 *  EasyLogger 移植桩 (elog.c 通过 extern 引用, 签名不可改)
 * ================================================================ */

ElogErrCode elog_port_init(void)
{
    s_output_lock = sys_mutex_create();
    log_file_init(); /* 复位轮换状态 (LOG_FILE_ENABLE=0 时为空宏); 打开是懒的 */
    return ELOG_NO_ERR;
}

void elog_port_deinit(void)
{
    /* 落盘不持有句柄 (每段写完即 close), 这里无需收尾 */
    sys_mutex_destroy(s_output_lock);
    s_output_lock = NULL;
}

void elog_port_output(const char *log, size_t size)
{
    if (!log || size == 0)
    {
        return;
    }

    /* 先给 sink (串口等实时通道, 延迟最低), 再落盘 */
    if (s_sink)
    {
        s_sink((const uint8_t *)log, (uint32_t)size);
    }

    /* 文件镜像: 与日志库无关的通用模块, 失败只丢本段 (见 log_file.h) */
    log_file_write((const uint8_t *)log, (uint32_t)size);
}

void elog_port_output_lock(void)
{
    sys_mutex_lock(s_output_lock);
}

void elog_port_output_unlock(void)
{
    sys_mutex_unlock(s_output_lock);
}

const char *elog_port_get_time(void)
{
    uint32_t ms = system_get_time_ms();
    uint32_t s  = ms / 1000u;
    snprintf(s_time_buf, sizeof(s_time_buf), "%02u:%02u:%02u",
             (unsigned)(s / 3600u),
             (unsigned)((s % 3600u) / 60u),
             (unsigned)(s % 60u));
    return s_time_buf;
}

const char *elog_port_get_p_info(void)
{
    return "";   /* 单进程, 无进程信息 */
}

const char *elog_port_get_t_info(void)
{
    return "";   /* 输出即当前上下文, 无额外线程信息 */
}
