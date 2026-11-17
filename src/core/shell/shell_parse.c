/**
 * @file shell_parse.c
 * @brief shell 命令参数解析的实现 (接口/语义说明见 shell_parse.h)
 *
 * 两个编译期后端:
 *  - SHELL_PARSE_USE_LIBC=1 (默认): 走 libc strtoul/strtol/strtod;
 *  - SHELL_PARSE_USE_LIBC=0: 手写兜底, 供无 strto* 的精简 libc 使用。
 * 两分支返回值语义逐条一致:
 *  - shell_arg_u32/int: base=0 语义 (十进制 / 0x / 0 八进制), 整串消费, 溢出拒收;
 *  - shell_arg_float:   十进制浮点 (含 . 与 e 指数), 整串消费, 溢出/NaN 拒收。
 */

#include "shell_parse.h"
#include <limits.h>
#include <float.h>
#include <errno.h>

#if SHELL_PARSE_USE_LIBC

#include <stdlib.h> /* strtoul / strtol / strtod */

bool shell_arg_u32(const char *s, uint32_t *out)
{
    if (out == NULL || s == NULL || *s == '\0')
        return false;
    /* strtoul 会把 "-1" 合法地回绕成无符号最大值, 对 u32 语义应视为非法 */
    if (*s == '-' || *s == '+')
        return false;

    errno = 0;
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 0);
    if (end == s || *end != '\0' || errno == ERANGE)
        return false;
#if UINT32_MAX < ULONG_MAX
    if (v > (unsigned long)UINT32_MAX)
        return false;
#endif
    *out = (uint32_t)v;
    return true;
}

bool shell_arg_int(const char *s, int *out)
{
    if (out == NULL || s == NULL || *s == '\0')
        return false;

    errno = 0;
    char *end = NULL;
    long v = strtol(s, &end, 0);
    if (end == s || *end != '\0' || errno == ERANGE)
        return false;
#if (INT_MAX < LONG_MAX) || (INT_MIN > LONG_MIN)
    if (v > (long)INT_MAX || v < (long)INT_MIN)
        return false;
#endif
    *out = (int)v;
    return true;
}

bool shell_arg_float(const char *s, float *out)
{
    if (out == NULL || s == NULL || *s == '\0')
        return false;

    /* 用 strtod 取 double 再收窄到 float: 比 strtof 更能掌控
       溢出/下溢判定 (strtof 对次正规会报 ERANGE, 我们不想把极小值当错)。 */
    errno = 0;
    char *end = NULL;
    double v = strtod(s, &end);
    if (end == s || *end != '\0' || errno == ERANGE)
        return false;
    if (v != v) /* NaN 拒收 (strtod 接受 "nan") */
        return false;
    if (v > (double)FLT_MAX || v < -(double)FLT_MAX) /* ±inf / 溢出 float 范围 */
        return false;
    *out = (float)v;
    return true;
}

#else /* SHELL_PARSE_USE_LIBC == 0 : 精简 libc 手写兜底 */

/**
 * @brief 手写幅度解析 (无符号, 识别 0x/0/十进制, 整串消费, 溢出返回 false)
 *
 * 仅供本文件使用; 与 libc 分支逐条对齐。
 */
static bool shell_parse_scan_mag(const char *p, uint32_t *mag)
{
    if (p == NULL || *p == '\0')
        return false;

    uint32_t base = 10u;
    if (p[0] == '0')
    {
        if (p[1] == 'x' || p[1] == 'X')
        {
            base = 16u;
            p += 2;
            if (*p == '\0') /* 只有 "0x" 前缀, 无数字 */
                return false;
        }
        else if (p[1] != '\0')
        {
            base = 8u; /* 前导 0 → 八进制 */
            p += 1;
        }
        else
        {
            *mag = 0u; /* 单独的 "0" */
            return true;
        }
    }

    uint32_t v = 0u;
    for (; *p != '\0'; ++p)
    {
        uint32_t d;
        if (*p >= '0' && *p <= '9')
            d = (uint32_t)(*p - '0');
        else if (base == 16u && *p >= 'a' && *p <= 'f')
            d = (uint32_t)(*p - 'a') + 10u;
        else if (base == 16u && *p >= 'A' && *p <= 'F')
            d = (uint32_t)(*p - 'A') + 10u;
        else
            return false;

        if (d >= base) /* 例如八进制里出现 '8'/'9' */
            return false;
        if (v > (UINT32_MAX - d) / base) /* 溢出 */
            return false;
        v = v * base + d;
    }

    *mag = v;
    return true;
}

bool shell_arg_u32(const char *s, uint32_t *out)
{
    if (out == NULL || s == NULL || *s == '\0')
        return false;
    if (*s == '-' || *s == '+')
        return false;
    return shell_parse_scan_mag(s, out);
}

bool shell_arg_int(const char *s, int *out)
{
    if (out == NULL || s == NULL || *s == '\0')
        return false;

    bool neg = false;
    const char *p = s;
    if (*p == '+' || *p == '-')
    {
        neg = (*p == '-');
        ++p;
    }

    uint32_t mag;
    if (!shell_parse_scan_mag(p, &mag))
        return false;

    if (neg)
    {
        uint32_t limit = (uint32_t)INT_MAX + 1u; /* INT_MIN 的幅度 */
        if (mag > limit)
            return false;
        *out = (mag == limit) ? INT_MIN : -(int)mag;
    }
    else
    {
        if (mag > (uint32_t)INT_MAX)
            return false;
        *out = (int)mag;
    }
    return true;
}

/**
 * @brief 手写十进制浮点解析 (可选符号 + 整数/小数 + 可选 e 指数)
 *
 * 仅用于无 strtod 的精简 libc; 只认十进制, 不认 "inf"/"nan"/十六进制浮点,
 * 因此天然不会产出非有限值。溢出 float 范围返回 false。
 */
static bool shell_parse_scan_float(const char *p, float *out)
{
    bool neg = false;
    bool any = false;
    double value;

    if (*p == '+' || *p == '-')
    {
        neg = (*p == '-');
        ++p;
    }

    /* 整数部分 */
    value = 0.0;
    while (*p >= '0' && *p <= '9')
    {
        value = value * 10.0 + (double)(*p - '0');
        ++p;
        any = true;
    }

    /* 小数部分 */
    if (*p == '.')
    {
        double scale = 1.0;
        ++p;
        while (*p >= '0' && *p <= '9')
        {
            scale *= 10.0;
            value += (double)(*p - '0') / scale;
            ++p;
            any = true;
        }
    }

    if (!any) /* 一个数字都没有 (如 "", "-", ".", ".e" ) */
        return false;

    /* 指数 */
    if (*p == 'e' || *p == 'E')
    {
        bool eneg = false;
        int exp = 0;
        ++p;
        if (*p == '+' || *p == '-')
        {
            eneg = (*p == '-');
            ++p;
        }
        if (!(*p >= '0' && *p <= '9')) /* "1e"/"1e+" 无指数数字 */
            return false;
        while (*p >= '0' && *p <= '9')
        {
            if (exp < 400) /* 上限足够触发 float 溢出判定, 且避免长循环 */
                exp = exp * 10 + (*p - '0');
            ++p;
        }

        double e = 1.0;
        for (int n = 0; n < exp; ++n)
            e *= 10.0;
        value = eneg ? value / e : value * e;
    }

    if (*p != '\0') /* 尾随垃圾 */
        return false;
    if (value > (double)FLT_MAX || value < -(double)FLT_MAX) /* inf / 溢出 */
        return false;

    *out = (float)(neg ? -value : value);
    return true;
}

bool shell_arg_float(const char *s, float *out)
{
    if (out == NULL || s == NULL || *s == '\0')
        return false;
    return shell_parse_scan_float(s, out);
}

#endif /* SHELL_PARSE_USE_LIBC */
