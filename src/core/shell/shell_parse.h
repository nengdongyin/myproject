#ifndef CORE_SHELL_SHELL_PARSE_H
#define CORE_SHELL_SHELL_PARSE_H

/**
 * @file shell_parse.h
 * @brief shell 命令参数解析的唯一入口 (整数 argv → 数值) —— 接口声明
 *
 * @par 为什么需要这一层
 * `mem_access.c` (md/mw) 与 `vfs_shell.c` (hexdump) 曾各自手写一份"字符串转
 * 无符号整数", 两份逐字相同; `shell_cmds.c` (dump) 又内联了一份残缺版
 * (不支持 0x, 且把 "12abc" 当 12)。同一套解析散落三处, 修一处漏两处。本组件把
 * 它收敛为唯一实现, 所有 shell 命令共用, 与 `shell_api.h` / `log_api.h` 的
 * "门面唯一入口" 是同一纪律。
 *
 * @par 实现位置
 * 解析逻辑 (含 strtoul / 手写兜底分支) 在 `shell_parse.c`, 本头只暴露两个
 * 函数声明 —— 解析体量不小 (含 base 检测 / 溢出判定), 不适合内联进每个调用
 * 点, 否则每个引用它的 TU 都复制一份代码。
 *
 * @par 解析语义 (与 `strtoul(s, &end, 0)` 对齐)
 *  - 支持 **十进制** / **0x 十六进制** / **0 八进制** (base=0);
 *  - **整串必须被完全消费**: "12abc" 非法 (而不是被当成 12);
 *  - **溢出拒收**: 超过 uint32_t 或 `errno == ERANGE` 判非法;
 *  - 空串非法; `shell_arg_u32` 额外拒绝带符号 ("-1" 不作无符号回绕)。
 *
 * @par 后端选择 (编译期, 与 log/shell 门面同构, 见 shell_parse.c)
 *  - 默认 `SHELL_PARSE_USE_LIBC=1`: 走 libc 的 strtoul/strtol;
 *  - 精简 libc 无 strtoul 时置 `SHELL_PARSE_USE_LIBC=0`: 走手写兜底,
 *    两个分支的返回值语义**逐条一致** (含 0x/0 前缀、整串消费、溢出)。
 *  - 浮点 (`shell_arg_float`) 用 strtod 取 double 再收窄到 float, 溢出/NaN 拒收。
 *
 * @par 用法
 * @code
 *   #include "shell_parse.h"
 *   uint32_t addr;
 *   if (!shell_arg_u32(argv[1], &addr)) { shell_printf("bad addr\r\n"); return -1; }
 * @endcode
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef SHELL_PARSE_USE_LIBC
#define SHELL_PARSE_USE_LIBC 1
#endif

/**
 * @brief 解析无符号 32 位整数 (十进制 / 0x 十六进制 / 0 八进制)
 *
 * @param s   输入字符串 (须整串消费)
 * @param out 输出值 (不能为 NULL)
 * @retval true  解析成功, *out 已赋值
 * @retval false 非法 (空串 / 含非法字符 / 带符号 / 溢出)
 */
bool shell_arg_u32(const char *s, uint32_t *out);

/**
 * @brief 解析有符号整数 (十进制 / 0x 十六进制 / 0 八进制, 可带 +/-)
 *
 * @param s   输入字符串 (须整串消费)
 * @param out 输出值 (不能为 NULL)
 * @retval true  解析成功
 * @retval false 非法 (空串 / 含非法字符 / 溢出 int 范围)
 */
bool shell_arg_int(const char *s, int *out);

/**
 * @brief 解析浮点数 (十进制, 可带符号 / 小数点 / e 指数)
 *
 * @param s   输入字符串 (须整串消费)
 * @param out 输出值 (不能为 NULL)
 * @retval true  解析成功
 * @retval false 非法 (空串 / 含非法字符 / 溢出 float 范围 / NaN)
 */
bool shell_arg_float(const char *s, float *out);

#ifdef __cplusplus
}
#endif

#endif /* CORE_SHELL_SHELL_PARSE_H */
