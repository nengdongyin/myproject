/**
 * @file log_file.h
 * @brief 日志落盘 —— 双文件轮换的通用实现 (与日志后端无关)
 *
 * @details
 * 本模块只做一件事: 把"一段已经格式化好的日志字节"安全地追加进两个轮换
 * 文件。它**不认识任何日志库**, 只依赖项目 VFS (vfs.h) 与端口层时间
 * (port.h); 因此任何后端都可以在自己的"字节出口"处调 log_file_write():
 *   - EasyLogger → log_elog_port.c 的 elog_port_output()
 *   - 将来换 rxi / 其它 → 在对应钩子里调同一个 API
 * 这条边界让 log_api.h 的后端可替换性 (LOG_BACKEND_*) 不被文件镜像破坏。
 *
 * @par 轮换语义
 * 两个文件轮流承载: 当前写入的是"最新", 另一个是"上一轮"。写满
 * LOG_FILE_MAX_LEN 就切到另一个 (先清空), 故任何时刻都能拿到两段连续日志:
 * @code
 *   /lfs/app.log (最新)  ──写满──▶  /lfs/app.log.1 (最新)  ──写满──▶ ...
 * @endcode
 * "上电时哪个是最新"不靠持久化记录, 而是按两个文件的长度反推 (VFS 无
 * mtime), 详见 log_file.c 的 log_pick_current()。
 *
 * @par 并发
 * 本模块**不加锁**, 调用方必须保证串行 (EasyLogger 后端天然在
 * output_lock 内调用)。任何失败都只放弃当前这一段, 不影响调用方的其它
 * 输出路径 —— 日志不能反过来把系统拖垮。
 *
 * @par 装配顺序
 * log_file_init() 只需在"后端初始化"时调一次; 真正的打开是懒的, 所以
 * 日志可以在文件系统挂载之前就开始工作 (挂载成功后自动接上)。
 */

#ifndef CORE_LOG_FILE_H
#define CORE_LOG_FILE_H

#include <stdint.h>
#include "storage_config.h" /* STORAGE_MNT_FILES: 文件挂载点的单一数据源 */

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 *  配置 (全部可用 -D 覆盖)
 * ================================================================ */

/** @brief 是否把日志同时写入文件 (0=整支编译消失, 调用点退化为空宏) */
#ifndef LOG_FILE_ENABLE
#define LOG_FILE_ENABLE 1
#endif

/** @brief 当前日志文件路径 (轮换目标 = 本路径 + LOG_FILE_BAK_SUFFIX) */
#ifndef LOG_FILE_PATH
#define LOG_FILE_PATH STORAGE_MNT_FILES "/app.log"
#endif

/** @brief 轮换备份文件后缀 — 与 LOG_FILE_PATH 做字面量拼接, 须同挂载点 */
#ifndef LOG_FILE_BAK_SUFFIX
#define LOG_FILE_BAK_SUFFIX ".1"
#endif

/** @brief 单个文件的软上限 (字节); 写满后切到另一个文件 */
#ifndef LOG_FILE_MAX_LEN
#define LOG_FILE_MAX_LEN (128u * 1024u)
#endif

/** @brief 打开失败后的重试间隔 (ms) — 文件系统可能晚于日志初始化才挂载 */
#ifndef LOG_FILE_RETRY_MS
#define LOG_FILE_RETRY_MS 1000u
#endif

#if LOG_FILE_ENABLE

/**
 * @brief 复位轮换状态 (后端初始化/重新初始化时调用)
 *
 * @note 只清 RAM 状态, 不碰文件; "哪个是最新"在首次写盘时按盘上长度判定。
 */
void log_file_init(void);

/**
 * @brief 追加一段日志落盘, 必要时轮换文件
 *
 * @param data 日志字节 (已格式化好的一整段, 可含多行)
 * @param len  字节数
 *
 * @note 签名与 log_sink_fn 一致, 可直接接到"字节出口"位置。
 * @note 无返回值: 未能落盘时静默放弃本段 (不阻塞、不递归报错)。
 */
void log_file_write(const uint8_t *data, uint32_t len);

#else /* !LOG_FILE_ENABLE */

/* 关掉落盘: 全部编译为空宏 —— 与 shell_api.h 的 SHELL_BACKEND_NONE 分支同风格。
 * 宏参数照样逐个 (void) 掉: 让"实参只在这里用到"的调用点不触发未使用告警。
 * 注意只能按 log_file_write(data, len) 形式调用 (宏名取不到函数地址) ——
 * 与 shell_write / shell_rx_feed 等宏完全相同的限制。 */
#define log_file_init()             ((void)0)
#define log_file_write(data, len)   ((void)(data), (void)(len))

#endif /* LOG_FILE_ENABLE */

#ifdef __cplusplus
}
#endif

#endif /* CORE_LOG_FILE_H */
