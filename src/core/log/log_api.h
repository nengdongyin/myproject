/**
 * @file log_api.h
 * @brief 日志抽象门面 —— 全工程唯一的日志 API 表面
 *
 * @par 为什么需要这一层
 * 日志是全工程最横切的 API。若业务代码直接调用某个日志库
 * (EasyLogger 的 elog_i / rxi 的 log_info), 换后端时所有调用点都要改,
 * 且两库的宏名、参数个数、初始化方式都不一致。
 *
 * 本门面与 port.h 同构: core/ 只认 log_api.h, 换后端只改本文件的
 * 后端选择宏, 调用点零修改。与 param 存储后端同一套"编译期互斥"
 * 思路 (param_manager_config.h 的 PARAM_*_ENABLE), 而非 VFS 的
 * "运行期注册" —— 因为同一时刻只会有一个 logger 生效, 未选中的
 * 必须从 ROM 里整体消失。
 *
 * @par 后端选择 (编译期三选一, 可用 -D 覆盖)
 *   - LOG_BACKEND_EASYLOGGER (默认): tag 过滤 + hexdump + 缓冲输出
 *   - LOG_BACKEND_RXI:            仅级别 + 线程安全, tag 被丢弃
 *   - LOG_BACKEND_NONE:           全部编译为空 (等价 FT_LOG_ENABLE=0)
 *
 * @par 用法
 * @code
 *   #include "log_api.h"
 *   LOG_I("param", "flush ok, module=%d", id);
 *   LOG_HEXDUMP("rx", 16, buf, len);
 * @endcode
 *
 * @par 装配顺序 (app 层 init)
 * @code
 *   system_mutex_init();          // port.h, 先于日志 (输出锁依赖)
 *   log_sink_set(uart_tx_write);  // 注入字节出口 (可为 NULL)
 *   log_init();                   // 初始化并开启输出
 * @endcode
 */

#ifndef CORE_LOG_API_H
#define CORE_LOG_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 *  输出 sink —— 与 file_transfer 的 ft_tx_fn 同构的字节出口
 *
 *  后端格式化好一行后把字节交给 sink, 落到 UART/文件/Flash 由实现方
 *  决定。core 不持有任何物理通道句柄 (和 file_transfer "不认识 UART"
 *  是同一纪律)。
 * ================================================================ */

/** @brief 字节出口回调 (与 file_transfer 的 ft_tx_fn 同签名, 可互传) */
typedef void (*log_sink_fn)(const uint8_t *data, uint32_t len);

/* ================================================================
 *  后端选择
 * ================================================================ */

#if !defined(LOG_BACKEND_EASYLOGGER) && !defined(LOG_BACKEND_RXI) && \
    !defined(LOG_BACKEND_NONE)
    #define LOG_BACKEND_EASYLOGGER 1
#endif

/* ================================================================
 *  后端生命周期 API
 *
 *  EASYLOGGER 后端在 log_elog_port.c 提供实现; 其余后端退化为空宏,
 *  应用层无需 #if 保护即可无条件调用 log_init() / log_sink_set()。
 * ================================================================ */

#if defined(LOG_BACKEND_EASYLOGGER)
    void log_sink_set(log_sink_fn fn);
    int  log_init(void);
#else
    #define log_sink_set(fn)   ((void)(fn))
    #define log_init()         (0)
#endif

/* ================================================================
 *  LOG_* 宏映射 —— 调用面唯一入口
 * ================================================================ */

#if defined(LOG_BACKEND_EASYLOGGER)
    /* ---- EasyLogger ---- */
    #include <elog.h>
    #define LOG_A(tag, ...)     elog_a(tag, __VA_ARGS__)
    #define LOG_E(tag, ...)     elog_e(tag, __VA_ARGS__)
    #define LOG_W(tag, ...)     elog_w(tag, __VA_ARGS__)
    #define LOG_I(tag, ...)     elog_i(tag, __VA_ARGS__)
    #define LOG_D(tag, ...)     elog_d(tag, __VA_ARGS__)
    #define LOG_V(tag, ...)     elog_v(tag, __VA_ARGS__)
    #define LOG_RAW(...)        elog_raw(__VA_ARGS__)
    #define LOG_HEXDUMP(name, width, buf, size) \
            elog_hexdump(name, width, buf, size)

#elif defined(LOG_BACKEND_RXI)
    /* ---- rxi/log.c ----
     * 尚未接线: rxi 无 tag/hexdump/异步, 接入时需把其 log.h 改名 (避免
     * 与本门面同路径冲突) 后在此 include 并映射, tag 参数被丢弃。 */
    #error "LOG_BACKEND_RXI not wired yet: map rxi log_* here (tag dropped)"

#else /* LOG_BACKEND_NONE */
    /* ---- 关日志: 全部编译为空 ---- */
    #define LOG_A(tag, ...)     ((void)0)
    #define LOG_E(tag, ...)     ((void)0)
    #define LOG_W(tag, ...)     ((void)0)
    #define LOG_I(tag, ...)     ((void)0)
    #define LOG_D(tag, ...)     ((void)0)
    #define LOG_V(tag, ...)     ((void)0)
    #define LOG_RAW(...)        ((void)0)
    #define LOG_HEXDUMP(name, width, buf, size) ((void)0)
#endif

#ifdef __cplusplus
}
#endif

#endif /* CORE_LOG_API_H */
