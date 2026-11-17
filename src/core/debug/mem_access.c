/**
 * @file mem_access.c
 * @brief 物理地址白名单闸门 + md / mw 命令实现
 *
 * 见头文件的设计说明。这里只补充两个实现要点:
 *   - 白名单判定是"整段必须落在一段内", 不做跨段拼接 —— 跨段说明调用方算错了,
 *     宁可拒绝也不要悄悄分成两段读;
 *   - 读写本身用 `volatile` 指针, 保证编译器不会把多次访问合并/消除 (对外设
 *     寄存器语义重要); `mw` 写完立刻回读并打印, 便于确认真的写进去了。
 */

#include "mem_access.h"
#include "shell_api.h"
#include "shell_parse.h"
#include <errno.h>

/* ================================================================
 *  白名单状态 (板级注入, 静态生命周期由调用方保证)
 * ================================================================ */

static const mem_region_t *s_regions;
static size_t              s_region_n;

int mem_access_configure(const mem_region_t *tbl, size_t n)
{
    s_regions  = tbl;
    s_region_n = (tbl != NULL) ? n : 0;
    return (tbl != NULL && n > 0) ? 0 : -EINVAL;
}

size_t mem_access_region_count(void)
{
    return s_region_n;
}

int mem_access_check(uintptr_t addr, size_t len, int want_write)
{
    /* fail-closed: 没配白名单 → 一律拒 */
    if (s_regions == NULL || s_region_n == 0)
        return -EPERM;
    if (len == 0)
        return -EINVAL;
    if (addr + len < addr) /* size_t 回绕 */
        return -EINVAL;

    for (size_t i = 0; i < s_region_n; i++)
    {
        const mem_region_t *r = &s_regions[i];

        if (r->size == 0 || addr < r->base)
            continue;

        size_t off = addr - r->base;
        if (off >= r->size)
            continue;

        /* 起点命中: 整段必须落在这一个区间内 (跨段 → 不认) */
        if (len > r->size - off)
            continue;

        if (want_write && !r->writable)
            return -EPERM;

        return 0;
    }
    return -EPERM;
}

#if !defined(SHELL_BACKEND_NONE)

/* ================================================================
 *  命令: md / mw
 * ================================================================ */

/** @brief md 默认长度与上限 (命令跑在协议/调试线程里, 输出必须封顶) */
#define MEM_DUMP_DEFAULT_LEN 64u
#define MEM_DUMP_MAX_LEN     512u
/** @brief 每行字节数 */
#define MEM_DUMP_PER_LINE    16u

/*
 * 参数解析统一走 shell_parse.h 的 shell_arg_u32 (整串消费 + 溢出检测)。
 * 原 mem_parse_u32 已删除 —— 与 vfs_shell.c / shell_cmds.c 的手写版重复。
 */

/**
 * @brief 打印白名单拒绝的原因 (带上下文, 便于现场判断是"没配"还是"越界")
 */
static void mem_report_denied(const char *cmd, uint32_t addr, uint32_t len, int rc)
{
    if (s_region_n == 0)
    {
        shell_printf("%s: no whitelist configured\r\n", cmd);
        return;
    }
    shell_printf("%s: 0x%08x + %u not allowed (%s)\r\n", cmd, (unsigned)addr,
                 (unsigned)len, (rc == -EINVAL) ? "bad range" : "out of whitelist");
}

/**
 * @brief md —— 按白名单读内存, 16 字节一行 + ASCII 侧栏
 *
 * @param argc 参数个数
 * @param argv argv[1] = 地址 (支持 0x); argv[2] = 长度 (可省, 默认 64, 上限 512)
 * @return 0 成功; -1 失败 (已打印原因)
 */
static int mem_cmd_md(int argc, char *argv[])
{
    uint32_t addr;
    uint32_t len = MEM_DUMP_DEFAULT_LEN;
    int rc;

    if (argc < 2)
    {
        shell_printf("usage: md <addr> [len]    (addr supports 0x)\r\n");
        return -1;
    }
    if (!shell_arg_u32(argv[1], &addr))
    {
        shell_printf("md: bad address '%s'\r\n", argv[1]);
        return -1;
    }
    if (argc > 2 && !shell_arg_u32(argv[2], &len))
    {
        shell_printf("md: bad length '%s'\r\n", argv[2]);
        return -1;
    }
    if (len == 0)
        len = 1;
    if (len > MEM_DUMP_MAX_LEN)
        len = MEM_DUMP_MAX_LEN;

    rc = mem_access_check((uintptr_t)addr, (size_t)len, 0);
    if (rc != 0)
    {
        mem_report_denied("md", addr, len, rc);
        return -1;
    }

    for (uint32_t off = 0; off < len; off += MEM_DUMP_PER_LINE)
    {
        uint32_t n = len - off;
        const volatile uint8_t *p = (const volatile uint8_t *)(uintptr_t)(addr + off);

        if (n > MEM_DUMP_PER_LINE)
            n = MEM_DUMP_PER_LINE;

        shell_printf("%08x: ", (unsigned)(addr + off));
        for (uint32_t i = 0; i < MEM_DUMP_PER_LINE; i++)
        {
            if (i < n)
                shell_printf("%02x ", p[i]);
            else
                shell_printf("   ");
        }

        shell_printf(" |");
        for (uint32_t i = 0; i < n; i++)
        {
            uint8_t b = p[i];
            shell_printf("%c", (b >= 0x20u && b <= 0x7eu) ? (char)b : '.');
        }
        shell_printf("|\r\n");
    }
    return 0;
}
SHELL_CMD_MAIN(md, mem_cmd_md, dump memory: md <addr> [len]);

/**
 * @brief mw —— 按白名单写内存, 写完回读校验
 *
 * @param argc 参数个数
 * @param argv argv[1] = 地址; argv[2] = 值; argv[3] = 宽度 b|h|w (可省, 默认 w)
 * @return 0 成功; -1 失败 (已打印原因)
 *
 * @note 只写一个单元 (不做批量): 物理地址写是危险操作, 一次一个便于控制。
 * @note 宽度 >1 时要求按宽度对齐, 避免 unaligned access 在部分配置下触发异常。
 */
static int mem_cmd_mw(int argc, char *argv[])
{
    uint32_t addr;
    uint32_t val;
    uint32_t width = 4;
    int rc;

    if (argc < 3)
    {
        shell_printf("usage: mw <addr> <value> [b|h|w]\r\n");
        return -1;
    }
    if (!shell_arg_u32(argv[1], &addr))
    {
        shell_printf("mw: bad address '%s'\r\n", argv[1]);
        return -1;
    }
    if (!shell_arg_u32(argv[2], &val))
    {
        shell_printf("mw: bad value '%s'\r\n", argv[2]);
        return -1;
    }
    if (argc > 3)
    {
        if (argv[3][0] == 'b' && argv[3][1] == '\0')
            width = 1;
        else if (argv[3][0] == 'h' && argv[3][1] == '\0')
            width = 2;
        else if (argv[3][0] == 'w' && argv[3][1] == '\0')
            width = 4;
        else
        {
            shell_printf("mw: bad width '%s' (b|h|w)\r\n", argv[3]);
            return -1;
        }
    }
    if ((addr % width) != 0)
    {
        shell_printf("mw: 0x%08x not aligned to %u\r\n", (unsigned)addr, (unsigned)width);
        return -1;
    }

    rc = mem_access_check((uintptr_t)addr, (size_t)width, 1);
    if (rc != 0)
    {
        mem_report_denied("mw", addr, width, rc);
        return -1;
    }

    switch (width)
    {
    case 1:
        *(volatile uint8_t *)(uintptr_t)addr = (uint8_t)val;
        shell_printf("%08x <- %02x (readback %02x)\r\n", (unsigned)addr,
                     (unsigned)(val & 0xFFu),
                     (unsigned)*(volatile uint8_t *)(uintptr_t)addr);
        break;
    case 2:
        *(volatile uint16_t *)(uintptr_t)addr = (uint16_t)val;
        shell_printf("%08x <- %04x (readback %04x)\r\n", (unsigned)addr,
                     (unsigned)(val & 0xFFFFu),
                     (unsigned)*(volatile uint16_t *)(uintptr_t)addr);
        break;
    default:
        *(volatile uint32_t *)(uintptr_t)addr = val;
        shell_printf("%08x <- %08x (readback %08x)\r\n", (unsigned)addr, (unsigned)val,
                     (unsigned)*(volatile uint32_t *)(uintptr_t)addr);
        break;
    }
    return 0;
}
SHELL_CMD_MAIN(mw, mem_cmd_mw, write memory: mw <addr> <value> [b|h|w]);

#endif /* !SHELL_BACKEND_NONE */
