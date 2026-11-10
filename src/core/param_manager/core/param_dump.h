#ifndef PARAM_DUMP_H
#define PARAM_DUMP_H

#include <stdint.h>
#include "param_manager_config.h"
struct param_entry;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file param_dump.h
 * @brief 参数格式化输出 — 将参数状态转为可读文本
 *
 * 受 param_manager_config.h 中的 PARAM_DUMP_ENABLE 宏控制:
 *   - 启用时: 编译完整实现，提供 param_dump() / param_dump_entry()
 *   - 禁用时: 函数变为空操作宏，param_dump.c 不参与编译
 */

#if PARAM_DUMP_ENABLE

/**
 * @brief dump 输出回调 — 每行文本调用一次
 * @param line      格式化后的文本行
 * @param user_data 用户上下文
 */
typedef void (*param_dump_fn)(const char *line, void *user_data);

/**
 * @brief 注册 dump 输出回调 (全局单例, 建议启动时调用一次)
 *
 * param_dump() 每格式化一行文本即回调一次。注册为 NULL 可关闭输出。
 * 后注册的回调覆盖先前回调; 并发注册与遍历需调用方自行串行化。
 *
 * @param cb 输出回调; NULL 表示注销/关闭 dump 输出
 */
void param_dump_set_output(param_dump_fn cb);

/**
 * @brief 打印指定模块(或全部模块)的参数状态
 *
 * 按 MODULE_INIT_ORDER 顺序遍历模块，逐个输出参数名、当前值、范围、dirty、flag 等信息。
 * 每行文本经 param_dump_set_output() 注册的回调输出; 未注册回调时为空操作。
 *
 * @param module_id 模块 ID，0 表示所有模块
 * @param user_data 回调上下文 (透传给已注册回调)
 */
void param_dump(uint16_t module_id, void *user_data);

/**
 * @brief 格式化单个参数条目为一行文本
 *
 * 按 param_type_t 自动选择格式化器 (与条目所属子系统无关)。
 * 若 e 为 NULL / 未装配 vtable，line[0] 为 '\\0'；类型越界时输出 fallback 行。
 *
 * @param e    参数条目指针
 * @param line 输出缓冲区
 * @param size 缓冲区大小
 */
void param_dump_entry(struct param_entry *e, char *line, uint16_t size);

#else  /* !PARAM_DUMP_ENABLE */

typedef void (*param_dump_fn)(const char *line, void *user_data);

#define param_dump_set_output(cb)    ((void)(cb))
#define param_dump(id, ud)           ((void)0)
#define param_dump_entry(e, l, s)    ((void)(l), (void)(s), (void)0)

#endif /* PARAM_DUMP_ENABLE */

#ifdef __cplusplus
}
#endif

#endif
