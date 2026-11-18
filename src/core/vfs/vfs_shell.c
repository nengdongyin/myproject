/**
 * @file vfs_shell.c
 * @brief VFS 的 shell 命令集 —— 文件系统在线调试入口
 *
 * @par 为什么放在 core/vfs 而不是 app/
 * 本文件只依赖 `vfs.h` (公共 API) 与 `shell_api.h` (门面), 不 include 任何
 * shell 后端头文件, 因此任何用这套 VFS 的工程把它加进源列表就白得一套
 * 文件命令 —— 与 log/shell 门面"调用点零修改"是同一纪律。
 *
 * @par 当前目录 (cwd) 为什么必须由本层自己管
 * VFS 故意不提供 chdir: 所有 API 的 path 参数都要求**绝对路径**
 * (见 vfs.h 的 @section usage)。所以 `cd` / 相对路径只能落在 shell 层:
 * `s_cwd` 保存当前目录, `path_resolve()` 负责拼接与 `.`/`..` 归一。
 *
 * @par 输出必须封顶
 * 命令是在协议接收线程里同步执行的 (通信串口切到 shell 模式后, 由
 * protocol_parser_thread 直接 shell_rx_feed)。一条命令按住该线程几秒
 * 等于协议停摆, 所以:
 *   - `cat` 单次最多 VFS_SH_CAT_MAX 字节 —— 三种形态 (默认 / [off] [len] /
 *     -r 尾部窗口) 都封顶, 窗口只是换起点, 不放开上限;
 *   - `hexdump` 单次最多 VFS_SH_HD_MAX 字节;
 *   - `ls` 单目录最多 VFS_SH_LS_MAX 项。
 *
 * @par 与既有日志的已知交错
 * shell 与 EasyLogger 各持一把输出锁 (见 shell_config.h 的 SHELL_USING_LOCK
 * 注释), 所以 `cat` 读大文件时可能与日志行交错 —— 可读性受影响, 不影响正确性。
 */

#include "vfs_config.h"
#include "shell_api.h"
#include "shell_parse.h"
#include "vfs.h"

/*
 * 双重开关:
 *   - VFS_ENABLE_SHELL   : 工程是否要这套命令 (vfs_config.h, 可 -D 覆盖);
 *   - SHELL_BACKEND_NONE : shell 门面整体关闭时, 命令注册宏与 shell_printf
 *                          都会退化成空宏, 此时整个文件编译为空, 不占 ROM。
 */
#if VFS_ENABLE_SHELL && !defined(SHELL_BACKEND_NONE)

#include <string.h>
#include <errno.h>
#include <stdbool.h>

/* ================================================================
 *  容量与状态
 * ================================================================ */

/** @brief 单次读的块大小 */
#ifndef VFS_SH_IO_CHUNK
#define VFS_SH_IO_CHUNK   128u
#endif
/** @brief cat 单次最多输出的字节数 (覆盖前请评估同步输出的阻塞时间) */
#ifndef VFS_SH_CAT_MAX
#define VFS_SH_CAT_MAX    4096u
#endif
/** @brief hexdump 单次最多输出的字节数 */
#ifndef VFS_SH_HD_MAX
#define VFS_SH_HD_MAX     512u
#endif
/** @brief ls 单目录最多列出的条目数 */
#ifndef VFS_SH_LS_MAX
#define VFS_SH_LS_MAX     64
#endif
/** @brief cat 行缓冲: 攒够一行再一次性打印, 避免每字符一次 vsnprintf */
#ifndef VFS_SH_LINE_BUF
#define VFS_SH_LINE_BUF   96u
#endif
/** @brief cat 用法 (三种形态共用同一串, 失败与 help 一致) */
#define VFS_SH_CAT_USAGE \
    "usage: cat <file> [off] [len] | cat -r <file> [len]\r\n"

/** @brief shell 侧当前目录 (VFS 无 chdir, 这里只做路径拼接用) */
static char s_cwd[VFS_PATH_MAX] = "/";

/* ================================================================
 *  小工具
 * ================================================================ */

/**
 * @brief 负 errno 转可读名字
 * @param err VFS 返回值 (< 0)
 * @return 静态字符串
 */
static const char *vfs_sh_err_name(int err)
{
    switch (err)
    {
    case -ENOENT:       return "ENOENT";
    case -ENOTSUP:      return "ENOTSUP";
    case -ENOSPC:       return "ENOSPC";
    case -ENFILE:       return "ENFILE";
    case -EINVAL:       return "EINVAL";
    case -EIO:          return "EIO";
    case -EROFS:        return "EROFS";
    case -EBUSY:        return "EBUSY";
    case -EEXIST:       return "EEXIST";
    case -EBADF:        return "EBADF";
    case -ENOTDIR:      return "ENOTDIR";
    case -EISDIR:       return "EISDIR";
    case -ENAMETOOLONG: return "ENAMETOOLONG";
    case -EACCES:       return "EACCES";
    default:            return "ERR";
    }
}

/*
 * 参数解析统一走 shell_parse.h 的 shell_arg_u32 (整串消费 + 溢出检测)。
 * 原 vfs_sh_parse_u32 已删除 —— 与 mem_access.c / shell_cmds.c 的手写版重复。
 */

/**
 * @brief 把用户输入解析成 VFS 绝对路径 (相对路径基于 s_cwd)
 *
 * @param in  用户输入 (绝对或相对)
 * @param out 输出缓冲区, 容量须 >= VFS_PATH_MAX
 * @return 0 成功; -EINVAL 空输入; -ENAMETOOLONG 超长
 *
 * @note 只做词法归一 (`/` 折叠、`.` 跳过、`..` 回退一级), 不碰介质, 也不
 *       解析符号链接 —— VFS 本来就没有链接概念。
 */
static int vfs_sh_resolve(const char *in, char *out)
{
    size_t len = 0;

    if (!in || !in[0])
        return -EINVAL;

    out[0] = '\0';

    /* 相对路径: 以 cwd 起头 (去掉尾部 '/' 便于后面统一补分隔符) */
    if (in[0] != '/')
    {
        size_t cl = strlen(s_cwd);
        while (cl > 1 && s_cwd[cl - 1] == '/')
            cl--;
        if (cl >= VFS_PATH_MAX)
            return -ENAMETOOLONG;
        memcpy(out, s_cwd, cl);
        out[cl] = '\0';
        len = cl;
    }

    /* 根目录特殊处理: 不能事后拼成 "//x" */
    if (len == 1 && out[0] == '/')
    {
        out[0] = '\0';
        len = 0;
    }

    for (const char *p = in; *p; )
    {
        while (*p == '/')
            p++;
        if (!*p)
            break;

        const char *q = p;
        while (*q && *q != '/')
            q++;
        size_t clen = (size_t)(q - p);

        if (clen == 1 && p[0] == '.')
        {
            /* 当前目录: 丢弃 */
        }
        else if (clen == 2 && p[0] == '.' && p[1] == '.')
        {
            /* 上一级: 去掉末段与分隔符, 已在根则保持根 */
            while (len > 0 && out[len - 1] != '/')
                len--;
            if (len > 0)
                len--;
            out[len] = '\0';
        }
        else if (clen > VFS_NAME_MAX)
        {
            return -ENAMETOOLONG;
        }
        else
        {
            if (len + 1 + clen + 1 > VFS_PATH_MAX)
                return -ENAMETOOLONG;
            out[len++] = '/';
            memcpy(out + len, p, clen);
            len += clen;
            out[len] = '\0';
        }

        p = q;
    }

    if (len == 0)
    {
        out[0] = '/';
        out[1] = '\0';
    }
    return 0;
}

/**
 * @brief 解析参数路径并统一打印失败原因
 * @param cmd 命令名 (用于错误前缀)
 * @param in  用户输入
 * @param out 输出绝对路径缓冲 (>= VFS_PATH_MAX)
 * @return 0 成功; -1 已打印错误
 */
static int vfs_sh_arg_path(const char *cmd, const char *in, char *out)
{
    int rc = vfs_sh_resolve(in, out);
    if (rc < 0)
    {
        shell_printf("%s: bad path (%s)\r\n", cmd, vfs_sh_err_name(rc));
        return -1;
    }
    return 0;
}

/* ================================================================
 *  命令: pwd / cd
 * ================================================================ */

/**
 * @brief pwd —— 打印 shell 侧当前目录
 * @return 0
 */
static int vfs_sh_pwd(void)
{
    shell_printf("%s\r\n", s_cwd);
    return 0;
}
SHELL_CMD(pwd, vfs_sh_pwd, print current directory);

/**
 * @brief cd —— 切换当前目录 (无参回根目录)
 *
 * @param argc 参数个数
 * @param argv argv[1] = 目标目录
 * @return 0 成功; -1 失败 (已打印原因)
 */
static int vfs_sh_cd(int argc, char *argv[])
{
    char path[VFS_PATH_MAX];
    vfs_stat_t st;
    int rc;

    if (argc < 2)
    {
        s_cwd[0] = '/';
        s_cwd[1] = '\0';
        return 0;
    }

    if (vfs_sh_arg_path("cd", argv[1], path) != 0)
        return -1;

    /* 根 "/" 不是挂载点, vfs_stat 会返回 -ENOENT —— 它是 shell 层的虚拟节点,
       只用于列出所有卷 (见 vfs_sh_ls_root), 所以这里直接接受。 */
    if (strcmp(path, "/") == 0)
    {
        s_cwd[0] = '/';
        s_cwd[1] = '\0';
        return 0;
    }

    rc = vfs_stat(path, &st);
    if (rc < 0)
    {
        shell_printf("cd: %s\r\n", vfs_sh_err_name(rc));
        return -1;
    }
    if (st.type != VFS_TYPE_DIR)
    {
        shell_printf("cd: not a directory\r\n");
        return -1;
    }

    memcpy(s_cwd, path, strlen(path) + 1);
    return 0;
}
SHELL_CMD_MAIN(cd, vfs_sh_cd, change directory - cd [path]);

#if VFS_ENABLE_DIR
/**
 * @brief 列出根目录 —— 根不是挂载点, 列出所有已挂载卷
 * @return 0
 */
static int vfs_sh_ls_root(void)
{
    const char *mnt;
    uint8_t fstype;
    uint8_t flags;

    for (size_t i = 0; vfs_mount_get(i, &mnt, &fstype, &flags); i++)
    {
        shell_printf("d %-8s %s fstype=%u%s\r\n", mnt,
                     (flags & VFS_MNT_RO) ? "ro" : "rw", (unsigned)fstype,
                     (flags & VFS_MNT_AUTOFORMAT) ? " autoformat" : "");
    }
    return 0;
}
/**
 * @brief ls —— 列目录 (无参列当前目录; 根目录列挂载卷)
 *
 * @param argc 参数个数
 * @param argv argv[1] = 目录路径
 * @return 0 成功; -1 失败 (已打印原因)
 */
static int vfs_sh_ls(int argc, char *argv[])
{
    char path[VFS_PATH_MAX];
    vfs_dirent_t ent;
    vfs_fd_t fd;
    int n = 0;
    int rc;

    if (vfs_sh_arg_path("ls", (argc > 1) ? argv[1] : ".", path) != 0)
        return -1;

    if (strcmp(path, "/") == 0)
        return vfs_sh_ls_root();

    fd = vfs_opendir(path);
    if (fd < 0)
    {
        shell_printf("ls: %s\r\n", vfs_sh_err_name(fd));
        return -1;
    }

    while (vfs_readdir(fd, &ent) == 1)
    {
        shell_printf("%c %8u  %s\r\n",
                     (ent.type == VFS_TYPE_DIR) ? 'd' : 'f',
                     (unsigned)ent.size, ent.name);
        if (++n >= VFS_SH_LS_MAX)
        {
            shell_printf("... truncated\r\n");
            break;
        }
    }

    rc = vfs_closedir(fd);
    if (rc < 0)
        shell_printf("ls: close %s\r\n", vfs_sh_err_name(rc));

    if (n == 0)
        shell_printf("(empty)\r\n");
    return 0;
}
SHELL_CMD_MAIN(ls, vfs_sh_ls, list directory - ls [path]);
#else
/**
 * @brief ls —— 目录功能被编译关闭时的占位
 * @return 0
 */
static int vfs_sh_ls(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    shell_printf("ls: VFS_ENABLE_DIR=0\r\n");
    return 0;
}
SHELL_CMD_MAIN(ls, vfs_sh_ls, list directory - ls [path]);
#endif /* VFS_ENABLE_DIR */

/* ================================================================
 *  命令: cat / hexdump
 * ================================================================ */

/**
 * @brief 把 cat 的行缓冲刷出去 (补 CRLF)
 * @param line 行缓冲
 * @param len  当前有效长度 (刷完清零)
 */
static void vfs_sh_cat_flush(char *line, size_t *len)
{
    if (*len == 0)
        return;
    line[*len] = '\0';
    shell_printf("%s\r\n", line);
    *len = 0;
}

/**
 * @brief cat —— 以文本形式打印文件 (不可打印字符记 '.')
 *
 * @par 三种形态
 * @code
 *   cat <file>               从文件头起, 最多 VFS_SH_CAT_MAX 字节
 *   cat <file> [off] [len]   从 off 起取 len 字节 (参数风格与 hexdump 一致)
 *   cat -r <file> [len]      只看尾部: 末尾 len 字节 (tail -c 语义)
 * @endcode
 * `-r` 只接受"尾部窗口"这一种形式, 不与显式偏移组合 (语义冲突, 直接报用法);
 * 窗口内按文件原序输出 —— 看日志尾部要的就是这个, 不是逐行倒着打。
 *
 * @note 三种形态都封顶 VFS_SH_CAT_MAX: 命令在协议接收线程里同步跑, 出口是
 *       逐字节 uart_poll_out, 放开上限等于让交互与协议停摆几分钟 (见文件头
 *       "输出必须封顶")。要看大文件的别处, 用窗口。
 * @note 需要后端的 lseek (VFS_CAP_SEEK): 长度靠 SEEK_END 取, 窗口靠
 *       SEEK_SET 定位。现有三个后端 (littlefs / fatfs / rawflash) 都提供;
 *       将来的纯流式后端没有它, 本命令会直接报 ENOTSUP 而不是给出错的内容。
 *
 * @param argc 参数个数
 * @param argv argv[1] = 文件路径 或 "-r"; 其余参数见上
 * @return 0 成功; -1 失败 (已打印原因)
 */
static int vfs_sh_cat(int argc, char *argv[])
{
    char path[VFS_PATH_MAX];
    char line[VFS_SH_LINE_BUF];
    uint8_t buf[VFS_SH_IO_CHUNK];
    size_t ln = 0;
    uint32_t off = 0;
    uint32_t want = VFS_SH_CAT_MAX;
    uint32_t printed = 0;
    bool tail = false;
    int argi = 1;
    vfs_fd_t fd;

    if (argc < 2)
    {
        shell_printf(VFS_SH_CAT_USAGE);
        return -1;
    }

    /* ---- 参数: "-r" 只换窗口算法 (从尾部量), 不改变其它语义 ---- */
    if (strcmp(argv[1], "-r") == 0)
    {
        tail = true;
        argi = 2;
        if (argc <= argi)
        {
            shell_printf(VFS_SH_CAT_USAGE);
            return -1;
        }
    }
    if (vfs_sh_arg_path("cat", argv[argi], path) != 0)
        return -1;

    if (tail)
    {
        if (argc > argi + 1 && !shell_arg_u32(argv[argi + 1], &want))
        {
            shell_printf("cat: bad length\r\n");
            return -1;
        }
        if (argc > argi + 2) /* 尾部窗口不接受偏移 */
        {
            shell_printf(VFS_SH_CAT_USAGE);
            return -1;
        }
    }
    else
    {
        if (argc > argi + 1 && !shell_arg_u32(argv[argi + 1], &off))
        {
            shell_printf("cat: bad offset\r\n");
            return -1;
        }
        if (argc > argi + 2 && !shell_arg_u32(argv[argi + 2], &want))
        {
            shell_printf("cat: bad length\r\n");
            return -1;
        }
    }
    if (want == 0)
    {
        shell_printf("cat: length 0\r\n");
        return -1;
    }

    fd = vfs_open(path, VFS_O_RDONLY);
    if (fd < 0)
    {
        shell_printf("cat: %s\r\n", vfs_sh_err_name(fd));
        return -1;
    }

    /* ---- 定窗口 [off, off+want): 文件长度用 SEEK_END 取, 尾部模式反推起点 ---- */
    const off_t end = vfs_lseek(fd, 0, VFS_SEEK_END);
    if (end < 0)
    {
        shell_printf("cat: %s\r\n", vfs_sh_err_name((int)end));
        (void)vfs_close(fd);
        return -1;
    }
    const uint32_t size = (uint32_t)end;

    if (size == 0)
    {
        shell_printf("(empty)\r\n");
        (void)vfs_close(fd);
        return 0;
    }

    /* 单次上限先落地 (对尾部模式尤其重要): `-r 999999` 要的是"最后 N 个、
       但最多一个上限", 而不是"尾部窗口的前 4096 字节" */
    if (want > VFS_SH_CAT_MAX)
        want = VFS_SH_CAT_MAX;

    if (tail)
        off = (want < size) ? (size - want) : 0;
    if (off >= size)
    {
        shell_printf("cat: offset beyond EOF (offset=%u size=%u)\r\n",
                     (unsigned)off, (unsigned)size);
        (void)vfs_close(fd);
        return -1;
    }

    /* 文件剩余不足 → 缩短窗口 */
    const uint32_t avail = size - off;
    if (want > avail)
        want = avail;

    /* 定位到窗口起点 —— 必须无条件做: 上面 SEEK_END 取长度已把偏移停在文件尾,
       off==0 时若跳过这一步就会从 EOF 开始读 (什么都不打印) */
    if (vfs_lseek(fd, (off_t)off, VFS_SEEK_SET) < 0)
    {
        shell_printf("cat: seek failed\r\n");
        (void)vfs_close(fd);
        return -1;
    }

    while (printed < want)
    {
        size_t chunk = (size_t)(want - printed);
        if (chunk > sizeof(buf))
            chunk = sizeof(buf);

        ssize_t n = vfs_read(fd, buf, chunk);
        if (n < 0)
        {
            shell_printf("cat: %s\r\n", vfs_sh_err_name((int)n));
            break;
        }
        if (n == 0)
            break;

        for (ssize_t i = 0; i < n; i++)
        {
            char c = (char)buf[i];
            if (c == '\r')
                continue;                        /* 终端侧统一补 CRLF */
            if (c == '\n')
            {
                vfs_sh_cat_flush(line, &ln);
                continue;
            }
            if (c < 0x20 || c > 0x7e)
                c = '.';
            if (ln + 1 >= sizeof(line))
                vfs_sh_cat_flush(line, &ln);
            line[ln++] = c;
        }
        printed += (uint32_t)n;
    }

    vfs_sh_cat_flush(line, &ln);
    (void)vfs_close(fd);

    /* 封顶提示: 只在"确实被上限切掉"时出现 —— 判定的是"窗口外侧还有没有
       数据", 而不是"请求是否大于上限": 默认形态 (want==上限) 与 `-r` 默认
       都要能提示; 反过来"文件比窗口短"不是截断, 不提示。
       正序看窗口之后 (size > off+printed), 尾部看窗口之前 (off > 0)。 */
    const bool more_beyond = tail ? (off > 0) : (size > off + printed);
    if (more_beyond && printed >= VFS_SH_CAT_MAX)
        shell_printf("... truncated at %u bytes\r\n", (unsigned)VFS_SH_CAT_MAX);
    return 0;
}
SHELL_CMD_MAIN(cat, vfs_sh_cat, print file as text - cat <f> [off] [len] | -r <f> [len]);

/**
 * @brief hexdump —— 十六进制 + ASCII 打印文件片段
 *
 * @param argc 参数个数
 * @param argv argv[1] = 文件路径; argv[2] = 起始偏移(可省, 支持 0x);
 *             argv[3] = 长度(可省, 上限 VFS_SH_HD_MAX)
 * @return 0 成功; -1 失败 (已打印原因)
 */
static int vfs_sh_hexdump(int argc, char *argv[])
{
    char path[VFS_PATH_MAX];
    uint8_t buf[16];
    uint32_t off = 0;
    uint32_t len = VFS_SH_HD_MAX;
    uint32_t done = 0;
    vfs_fd_t fd;

    if (argc < 2)
    {
        shell_printf("usage: hexdump <file> [off] [len]\r\n");
        return -1;
    }
    if (vfs_sh_arg_path("hexdump", argv[1], path) != 0)
        return -1;
    if (argc > 2 && !shell_arg_u32(argv[2], &off))
    {
        shell_printf("hexdump: bad offset\r\n");
        return -1;
    }
    if (argc > 3 && !shell_arg_u32(argv[3], &len))
    {
        shell_printf("hexdump: bad length\r\n");
        return -1;
    }
    if (len > VFS_SH_HD_MAX)
        len = VFS_SH_HD_MAX;

    fd = vfs_open(path, VFS_O_RDONLY);
    if (fd < 0)
    {
        shell_printf("hexdump: %s\r\n", vfs_sh_err_name(fd));
        return -1;
    }

    if (off > 0 && vfs_lseek(fd, (off_t)off, VFS_SEEK_SET) < 0)
    {
        shell_printf("hexdump: seek failed\r\n");
        (void)vfs_close(fd);
        return -1;
    }

    while (done < len)
    {
        size_t want = (size_t)(len - done);
        if (want > sizeof(buf))
            want = sizeof(buf);

        ssize_t n = vfs_read(fd, buf, want);
        if (n < 0)
        {
            shell_printf("hexdump: %s\r\n", vfs_sh_err_name((int)n));
            break;
        }
        if (n == 0)
            break;

        shell_printf("%08x  ", (unsigned)(off + done));
        for (ssize_t i = 0; i < (ssize_t)sizeof(buf); i++)
        {
            if (i < n)
                shell_printf("%02x ", buf[i]);
            else
                shell_printf("   ");
        }
        shell_printf(" |");
        for (ssize_t i = 0; i < n; i++)
        {
            char c = (char)buf[i];
            shell_printf("%c", (c >= 0x20 && c <= 0x7e) ? c : '.');
        }
        shell_printf("|\r\n");

        done += (uint32_t)n;
    }

    (void)vfs_close(fd);
    return 0;
}
SHELL_CMD_MAIN(hexdump, vfs_sh_hexdump, hex dump - hexdump <file> [off] [len]);

/* ================================================================
 *  命令: mkdir / rm / stat / write
 * ================================================================ */

/**
 * @brief mkdir —— 创建目录
 * @param argc 参数个数
 * @param argv argv[1] = 目录路径
 * @return 0 成功; -1 失败
 */
static int vfs_sh_mkdir(int argc, char *argv[])
{
    char path[VFS_PATH_MAX];
    int rc;

    if (argc < 2)
    {
        shell_printf("usage: mkdir <path>\r\n");
        return -1;
    }
    if (vfs_sh_arg_path("mkdir", argv[1], path) != 0)
        return -1;

    rc = vfs_mkdir(path);
    if (rc < 0)
    {
        shell_printf("mkdir: %s\r\n", vfs_sh_err_name(rc));
        return -1;
    }
    return 0;
}
SHELL_CMD_MAIN(mkdir, vfs_sh_mkdir, make directory - mkdir <path>);

/**
 * @brief rm —— 删除文件或空目录
 *
 * @note VFS 没有 rmdir, 统一走 vfs_unlink; 能否删目录由后端决定
 *       (littlefs 用 lfs_remove, 空目录可删)。
 */
static int vfs_sh_rm(int argc, char *argv[])
{
    char path[VFS_PATH_MAX];
    int rc;

    if (argc < 2)
    {
        shell_printf("usage: rm <path>\r\n");
        return -1;
    }
    if (vfs_sh_arg_path("rm", argv[1], path) != 0)
        return -1;

    rc = vfs_unlink(path);
    if (rc < 0)
    {
        shell_printf("rm: %s\r\n", vfs_sh_err_name(rc));
        return -1;
    }
    return 0;
}
SHELL_CMD_MAIN(rm, vfs_sh_rm, remove file or empty dir - rm <path>);

/**
 * @brief stat —— 打印类型与大小
 * @param argc 参数个数
 * @param argv argv[1] = 路径
 * @return 0 成功; -1 失败
 */
static int vfs_sh_stat(int argc, char *argv[])
{
    char path[VFS_PATH_MAX];
    vfs_stat_t st;
    int rc;

    if (argc < 2)
    {
        shell_printf("usage: stat <path>\r\n");
        return -1;
    }
    if (vfs_sh_arg_path("stat", argv[1], path) != 0)
        return -1;

    rc = vfs_stat(path, &st);
    if (rc < 0)
    {
        shell_printf("stat: %s\r\n", vfs_sh_err_name(rc));
        return -1;
    }

    /* 分三行打印: 路径可达 VFS_PATH_MAX(128), 单条格式串会撞上
       SHELL_PRINT_BUFFER(128) 被截断 */
    shell_printf("%s\r\n", path);
    shell_printf("  type: %s\r\n", (st.type == VFS_TYPE_DIR) ? "dir" : "file");
    shell_printf("  size: %u\r\n", (unsigned)st.size);
    return 0;
}
SHELL_CMD_MAIN(stat, vfs_sh_stat, show type and size - stat <path>);

/**
 * @brief write —— 用文本覆盖写一个文件 (调试用, 自动补换行)
 * @param argc 参数个数
 * @param argv argv[1] = 文件路径; argv[2..] = 文本 (以空格拼接)
 * @return 0 成功; -1 失败
 */
static int vfs_sh_write(int argc, char *argv[])
{
    char path[VFS_PATH_MAX];
    vfs_fd_t fd;
    int rc;

    if (argc < 3)
    {
        shell_printf("usage: write <file> <text...>\r\n");
        return -1;
    }
    if (vfs_sh_arg_path("write", argv[1], path) != 0)
        return -1;

    fd = vfs_open(path, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
    if (fd < 0)
    {
        shell_printf("write: %s\r\n", vfs_sh_err_name(fd));
        return -1;
    }

    for (int i = 2; i < argc; i++)
    {
        if (i > 2 && vfs_write(fd, " ", 1) < 0)
            break;
        rc = (int)vfs_write(fd, argv[i], strlen(argv[i]));
        if (rc < 0)
        {
            shell_printf("write: %s\r\n", vfs_sh_err_name(rc));
            (void)vfs_close(fd);
            return -1;
        }
    }
    (void)vfs_write(fd, "\n", 1);
    (void)vfs_close(fd);
    return 0;
}
SHELL_CMD_MAIN(write, vfs_sh_write, write text to file - write <file> <text...>);

/* ================================================================
 *  命令: mv / df (随 VFS 可选特性注册)
 * ================================================================ */

#if VFS_ENABLE_RENAME
/**
 * @brief mv —— 重命名 (同一挂载点内)
 * @param argc 参数个数
 * @param argv argv[1] = 源; argv[2] = 目标
 * @return 0 成功; -1 失败
 */
static int vfs_sh_mv(int argc, char *argv[])
{
    char from[VFS_PATH_MAX];
    char to[VFS_PATH_MAX];
    int rc;

    if (argc < 3)
    {
        shell_printf("usage: mv <from> <to>\r\n");
        return -1;
    }
    if (vfs_sh_arg_path("mv", argv[1], from) != 0)
        return -1;
    if (vfs_sh_arg_path("mv", argv[2], to) != 0)
        return -1;

    rc = vfs_rename(from, to);
    if (rc < 0)
    {
        shell_printf("mv: %s\r\n", vfs_sh_err_name(rc));
        return -1;
    }
    return 0;
}
SHELL_CMD_MAIN(mv, vfs_sh_mv, rename within one mount - mv <from> <to>);
#endif /* VFS_ENABLE_RENAME */

#if VFS_ENABLE_STATVFS
/**
 * @brief df —— 打印各卷总容量与空闲
 *
 * @param argc 参数个数
 * @param argv argv[1] 预留 (当前忽略, 一律列所有卷)
 * @return 0 成功; -1 失败
 */
static int vfs_sh_df(int argc, char *argv[])
{
    const char *mnt;
    uint8_t fstype;
    uint8_t flags;
    vfs_statvfs_t st;

    (void)argc;
    (void)argv;

    for (size_t i = 0; vfs_mount_get(i, &mnt, &fstype, &flags); i++)
    {
        int rc = vfs_statvfs(mnt, &st);
        if (rc < 0)
        {
            shell_printf("%-8s %s\r\n", mnt, vfs_sh_err_name(rc));
            continue;
        }
        shell_printf("%-8s %s total %u free %u\r\n", mnt,
                     (flags & VFS_MNT_RO) ? "ro" : "rw",
                     (unsigned)st.total_bytes, (unsigned)st.free_bytes);
    }
    return 0;
}
SHELL_CMD_MAIN(df, vfs_sh_df, show volume usage - df);
#endif /* VFS_ENABLE_STATVFS */

#else /* VFS_ENABLE_SHELL == 0 或 SHELL_BACKEND_NONE */

/* 编译为空: ISO C 不允许完全空的翻译单元, 放一个 typedef 兜底 */
typedef int vfs_shell_disabled_t;

#endif /* VFS_ENABLE_SHELL && !SHELL_BACKEND_NONE */
