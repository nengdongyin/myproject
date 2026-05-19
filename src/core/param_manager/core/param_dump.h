#ifndef PARAM_DUMP_H
#define PARAM_DUMP_H

#include <stdint.h>

struct param_entry;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file param_dump.h
 * @brief 参数格式化输出 — 将参数状态转为可读文本
 */

/**
 * @brief dump 输出回调 — 每行文本调用一次
 * @param line      格式化后的文本行
 * @param user_data 用户上下文
 */
typedef void (*param_dump_fn)(const char *line, void *user_data);

/**
 * @brief 打印指定模块(或全部模块)的参数状态
 *
 * 按 MODULE_INIT_ORDER 顺序遍历模块，逐个输出参数名、当前值、范围、dirty、flag 等信息。
 *
 * @param module_id 模块 ID，0 表示所有模块
 * @param cb        输出回调
 * @param user_data 回调上下文
 */
void param_dump(uint16_t module_id, param_dump_fn cb, void *user_data);

/**
 * @brief 格式化单个参数条目为一行文本
 *
 * 根据参数类型 (vtable 判断 App/IP) 自动选择格式化器。
 * 若 e 为 NULL 或类型不支持，line[0] 为 '\\0'。
 *
 * @param e    参数条目指针
 * @param line 输出缓冲区
 * @param size 缓冲区大小
 */
void param_dump_entry(struct param_entry *e, char *line, uint16_t size);

#ifdef __cplusplus
}
#endif

#endif
