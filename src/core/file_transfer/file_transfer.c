/**
 * @file file_transfer.c
 * @brief 文件传输组件 —— 协议侧实现 (parser 创建 / 挂链 / 收发事件 / 文件 I/O)
 *
 * @par 职责边界
 * 本单元只做"把 ymodem 协议引擎接进协议链, 并把收发事件落成文件操作"。
 * 它**不认识**:
 *  - UART     —— 字节出口经 @ref ft_tx_fn 注入
 *  - 链的生命周期 —— chain 句柄由 file_transfer_attach() 注入, 只 add/remove parser
 *  - 参数系统 —— 只在"启动"和"读路径"时调 param_read_string,
 *                参数表本身在 file_transfer_param.c
 *
 * @par 为什么不需要轮询函数
 * 会话状态**不单独存储**, 全部由协议链当前的锁定 parser 与 ymodem 协议阶段
 * 派生 (见 file_transfer_get_state / file_transfer_wants_bulk_rx):
 *  - 启动时本组件把链锁到收/发 parser 上 → 会话"在进行中";
 *  - 正常收尾在帧事件里解锁;
 *  - 协议层出错 (CAN / 重传超限 / 校验失败) 时 protocol_chain_feed 检测到
 *    致命错误会**自行解锁** —— 与改造前的行为一致。
 * 因此主循环不需要为本组件额外做任何事, 只需照旧调
 * protocol_chain_check_timeout_poll(), 协议超时重传由各 parser 的 poll 钩子驱动。
 */

#include "file_transfer.h"

#include <string.h>
#include <stdio.h>

#include "port.h"
#include "protocol_parser_ymodem.h"
#include "ymodem_common.h"

/* ================================================================
 *  日志 —— core/ 目录整体零 printf 依赖, 沿用 param_service.c 的策略:
 *  由 FT_LOG_ENABLE 隔离, 裸机产品置 0 即可完全去掉 stdio。
 * ================================================================ */
#ifndef FT_LOG_ENABLE
#define FT_LOG_ENABLE 1
#endif

#if FT_LOG_ENABLE
#define FT_LOG(...) printf(__VA_ARGS__)
#else
#define FT_LOG(...) ((void)0)
#endif

/* ================================================================
 *  内部状态
 * ================================================================ */

/**
 * @brief 组件运行时上下文
 *
 * @note 无"会话状态"字段: 会话是否在进行中由 @ref ft_is_recv_locked /
 *       @ref ft_is_send_locked (协议链当前锁定的 parser) 与 ymodem 协议阶段
 *       派生, 不另存副本 —— 两份状态必然分叉。
 * @note 路径同样不在此另存副本: PID_FT_RECV_PATH / PID_FT_SEND_PATH 的
 *       参数缓存就是唯一真相源。
 */
typedef struct
{
    sys_mutex_t *lock; /**< 保护下面的文件句柄等瞬态字段 */

    vfs_fd_t fd;                            /**< 本次会话的文件句柄 */
    uint32_t file_size;                     /**< 本次会话的文件总字节数 */
    uint32_t transferred;                   /**< 已收/已发字节数 (诊断用) */
    char     peer_name[VFS_NAME_MAX + 1];   /**< 接收: 对端 FILE_INFO 给的文件名 */

    protocol_chain           *chain;       /**< 注入, 不拥有 */
    ymodem_protocol_parser_t *recv_parser; /**< 本组件持有 */
    ymodem_protocol_parser_t *send_parser; /**< 本组件持有 */
    ft_tx_fn                  tx_write;     /**< 注入的字节出口 */
} file_transfer_t;

/* 静态初始化即置为非法句柄: detach() 可能在任何一次 attach 之前被调
   (如启动失败路径 / 单元测试), 此时 fd 的零值 0 会被当成合法句柄
   交给 vfs_close(), 误关别人的文件 */
static file_transfer_t g_ft = { .fd = VFS_FD_INVALID };

/* 收发缓冲区: 从 app_config.h 迁入, 静态零堆。
   接收器: 大 rx + 4B 应答 tx; 发送器: 4B 虚拟 rx + 大 tx。 */
static uint8_t s_recv_rx[YMODEM_STX_FRAME_LEN_BYTE]; /**< 1029 */
static uint8_t s_recv_tx[4];
static uint8_t s_send_rx[4];
static uint8_t s_send_tx[YMODEM_STX_FRAME_LEN_BYTE]; /**< 1029 */

/* ================================================================
 *  小工具
 * ================================================================ */

/** @brief 会话锁 (sys_mutex_lock/unlock 对 NULL 安全) */
static void ft_lock(void) { sys_mutex_lock(g_ft.lock); }
static void ft_unlock(void) { sys_mutex_unlock(g_ft.lock); }

/** @brief 拼 "dir/name" 到 out, 超长返回 -1 */
static int ft_join(char *out, size_t n, const char *dir, const char *name)
{
    int len = snprintf(out, n, "%s/%s", dir, name);
    if (len < 0 || (size_t)len >= n)
        return -1;
    return 0;
}

/** @brief 取路径最后一段 (文件名), 无 '/' 时原样返回 */
static const char *ft_name_of(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/**
 * @brief 读 STRING 参数到调用者缓冲区
 * @return 0 成功; -1 读取失败 (未注册/未加载)
 * @note 用 param_read_string 而非 param_read_cache: 不依赖 STRING
 *       缓存 .ptr 的内部语义, 且自带截断与 '\0' 保证。
 */
static int ft_read_path(uint32_t pid, char *buf, size_t buf_size)
{
    if (param_read_string(pid, buf, (uint16_t)buf_size) < 0)
        return -1;
    return 0;
}

/* ================================================================
 *  会话状态 (派生, 不存储)
 * ================================================================ */

/** @brief 协议链当前是否锁在接收器上 */
static bool ft_is_recv_locked(void)
{
    if (!g_ft.chain || !g_ft.recv_parser)
        return false;
    return protocol_chain_get_locked_parser(g_ft.chain) ==
           (protocol_parser_t *)g_ft.recv_parser;
}

/** @brief 协议链当前是否锁在发送器上 */
static bool ft_is_send_locked(void)
{
    if (!g_ft.chain || !g_ft.send_parser)
        return false;
    return protocol_chain_get_locked_parser(g_ft.chain) ==
           (protocol_parser_t *)g_ft.send_parser;
}

/* ================================================================
 *  路径校验
 * ================================================================ */

int file_transfer_check_path(const char *path, bool want_dir)
{
    char dir[FT_PATH_MAX + 1];
    vfs_stat_t st;
    size_t len;
    size_t cut;

    if (!path || path[0] != '/')
        return -1;

    len = strlen(path);
    if (len == 0 || len > (size_t)FT_PATH_MAX)
        return -1;
    /* 结尾带 '/' 会拼出双斜杠, 直接拒绝 */
    if (path[len - 1] == '/')
        return -1;

    if (want_dir)
    {
        memcpy(dir, path, len);
        dir[len] = '\0';
    }
    else
    {
        const char *slash = strrchr(path, '/');
        if (!slash || slash == path)
            return -1; /* 必须含文件名 */
        cut = (size_t)(slash - path);
        if (cut >= sizeof(dir))
            return -1;
        memcpy(dir, path, cut);
        dir[cut] = '\0';
    }

    if (vfs_stat(dir, &st) < 0)
        return -1;
    if (st.type != VFS_TYPE_DIR)
        return -1;

    return 0;
}

/* ================================================================
 *  底层动作
 * ================================================================ */

/** @brief 关掉当前文件句柄并清会话瞬态 (幂等) */
static void ft_reset_session(void)
{
    ft_lock();
    if (g_ft.fd >= 0)
    {
        vfs_close(g_ft.fd);
        g_ft.fd = VFS_FD_INVALID;
    }
    g_ft.file_size = 0;
    g_ft.transferred = 0;
    g_ft.peer_name[0] = '\0';
    ft_unlock();
}

/**
 * @brief 会话收尾 — 关文件句柄
 *
 * @param sync true = 落盘 (接收侧), false = 只读文件不刷
 * @note 协议链的解锁由调用方在本函数的同一事件里完成 (见
 *       ft_recv_frame_ready / ft_send_frame_ready), 不需要轮询。
 */
static void ft_session_close(bool sync)
{
    ft_lock();
    if (g_ft.fd >= 0)
    {
        if (sync)
            (void)vfs_sync(g_ft.fd);
        vfs_close(g_ft.fd);
        g_ft.fd = VFS_FD_INVALID;
    }
    ft_unlock();
}

/**
 * @brief 若链正锁在指定 parser 上则解链
 *
 * 会话终结时调用(正常收尾 / 协议层中止 / 本机 I/O 失败)。
 *
 * @note 从帧事件回调里调用是安全的: protocol_chain_feed 会检测到
 *       chain->locked_parser 被回调改动过, 尊重该选择并跳过随后的
 *       "致命错误解锁 + 重匹配重试"。
 * @note 必须在这里解链: 否则链一直锁在 ymodem 解析器上, 直到下一个
 *       非 ymodem 字节撞出 GARBAGE 才被动解锁 —— 期间
 *       file_transfer_wants_bulk_rx() 持续为真, 对外状态也是陈旧的。
 */
static void ft_release_chain(protocol_parser_t *parser)
{
    if (g_ft.chain && protocol_chain_get_locked_parser(g_ft.chain) == parser)
        protocol_chain_set_locked_parser(g_ft.chain, NULL);
}

/**
 * @brief 接收侧异常收尾: 通知对端 + 关文件 + 解链
 *
 * 对端收到 CAN CAN 会中止本次传输, 避免"本机已放弃但对方还在发包"。
 */
static void ft_recv_fail(const char *why)
{
    FT_LOG("[FT] recv aborted: %s\n", why);
    /* 中止对端由协议层表达(CAN CAN), 本组件不构字节、不碰物理通道 */
    ymodem_adapter_cancel(g_ft.recv_parser);
    ft_session_close(false);
    ft_release_chain((protocol_parser_t *)g_ft.recv_parser);
}

/* ================================================================
 *  TX 出口桥接
 * ================================================================ */

/** @brief tx buffer 有待发数据时交给注入的字节出口 */
static void ft_tx_ready(protocol_parser_t *parser, void *user_ctx)
{
    (void)user_ctx;

    if (!parser || parser->tx.data_len == 0 || !g_ft.tx_write)
        return;

    g_ft.tx_write(parser->tx.buffer, parser->tx.data_len);
}

/* ================================================================
 *  接收侧事件处理
 * ================================================================ */

/**
 * @brief 收到 FILE_INFO: 打开落地文件
 *
 * 落地路径 = PID_FT_RECV_PATH (目录) + "/" + 对端给的文件名。
 * 对端给的文件名不参与"本地想叫什么"的决策 —— ymodem 的语义就是
 * 发送方决定文件名, 本机只决定放哪。
 */
static void ft_recv_open(const char *peer_name, uint32_t size)
{
    char dir[FT_PATH_MAX + 1];
    char path[FT_PATH_MAX + 1];
    vfs_fd_t fd;

    if (!peer_name || peer_name[0] == '\0')
    {
        ft_recv_fail("empty file name from peer");
        return;
    }
    if (ft_read_path(PID_FT_RECV_PATH, dir, sizeof(dir)) != 0)
    {
        ft_recv_fail("cannot read PID_FT_RECV_PATH");
        return;
    }
    if (ft_join(path, sizeof(path), dir, peer_name) != 0)
    {
        FT_LOG("[FT] recv: path too long: %s/%s\n", dir, peer_name);
        ft_recv_fail("path too long");
        return;
    }

    ft_lock();
    /* 上一会话异常中止时可能残留句柄, 覆盖前先关 */
    if (g_ft.fd >= 0)
        vfs_close(g_ft.fd);
    fd = vfs_open(path, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
    g_ft.fd = fd;
    strncpy(g_ft.peer_name, peer_name, sizeof(g_ft.peer_name) - 1);
    g_ft.peer_name[sizeof(g_ft.peer_name) - 1] = '\0';
    g_ft.file_size = size;
    g_ft.transferred = 0;
    ft_unlock();

    if (fd < 0)
    {
        FT_LOG("[FT] recv: open %s failed (%d)\n", path, (int)fd);
        ft_recv_fail("open failed");
        return;
    }

    FT_LOG("[FT] recv: %s (%u bytes)\n", path, (unsigned)size);
}

/**
 * @brief 收到 DATA_PACKET: 写入落地文件
 *
 * 写失败 (含 -ENOSPC) 必须主动中止 —— 否则会对数据包回 ACK 却丢数据,
 * 对端以为成功, 本机拿到一个静默截断的文件。
 */
static void ft_recv_data(const uint8_t *data, uint32_t len)
{
    ssize_t written;

    ft_lock();
    written = (g_ft.fd >= 0) ? vfs_write(g_ft.fd, data, len) : (ssize_t)-1;
    if (written == (ssize_t)len)
        g_ft.transferred += len;
    ft_unlock();

    if (written != (ssize_t)len)
    {
        FT_LOG("[FT] recv: write failed (want %u, got %d)\n",
               (unsigned)len, (int)written);
        ft_recv_fail("write failed");
    }
}

static void ft_recv_frame_ready(protocol_parser_t *parser, void *parsed_data, void *user_ctx)
{
    const ymodem_receiver_event_t *event = (const ymodem_receiver_event_t *)parsed_data;

    (void)user_ctx;

    if (!event)
        return;

    switch (event->type)
    {
    case YMODEM_RECV_EVENT_FILE_INFO:
        ft_recv_open(event->file_name, event->file_size);
        break;

    case YMODEM_RECV_EVENT_DATA_PACKET:
        ft_recv_data(event->data, event->data_len);
        break;

    case YMODEM_RECV_EVENT_TRANSFER_COMPLETE:
        /* 当前文件收完: 落盘关句柄, 但会话可能还要走第二个 EOT */
        FT_LOG("[FT] recv: complete (%u bytes)\n", (unsigned)g_ft.transferred);
        ft_session_close(true);
        break;

    case YMODEM_RECV_EVENT_TRANSFER_FINISHED:
        /* 会话真正结束: 解链, 让链回到可自由匹配状态 */
        FT_LOG("[FT] recv: session finished\n");
        ft_session_close(true);
        ft_release_chain(parser);
        break;

    case YMODEM_RECV_EVENT_ERROR:
        /* 对端 CAN / 重传超限: 传输中止, 及时关句柄并解链 */
        FT_LOG("[FT] recv: aborted by protocol\n");
        ft_session_close(false);
        ft_release_chain(parser);
        break;

    default:
        break;
    }
}

/* ================================================================
 *  发送侧事件处理
 * ================================================================ */

/**
 * @brief 设置本次会话要发送的文件信息
 *
 * ymodem_sender_t 随 ymodem_protocol_parser_t 一同公开 (base 与 pri 都是公开
 * 成员), 因此直接取 &yp->pri.sender 即可 —— 适配层不需要再开一个访问器。
 *
 * @param name 空串表示"没有更多文件", 发送器将发空文件名包结束会话
 * @note 只能在 YMODEM_SENDER_EVENT_FILE_INFO 事件回调内调用:
 *       ymodem_sender_start() 会 memset file_info。
 */
static void ft_set_send_file_info(ymodem_protocol_parser_t *yp,
                                  const char *name, uint32_t size)
{
    ymodem_sender_file_info_t *fi;

    if (!yp || !name)
        return;

    fi = &yp->pri.sender.file_info;
    memset(fi->file_name, 0, sizeof(fi->file_name));
    strncpy(fi->file_name, name, sizeof(fi->file_name) - 1);
    fi->file_total_size = size;
}

/**
 * @brief 收到 FILE_INFO: 打开待发文件并填 file_info
 *
 * file_index == 0 是本组件唯一支持的文件; 之后 file_index > 0 意味着
 * 本文件已传完, 用空文件名让发送器自动发空文件名包结束会话。
 */
static void ft_send_file_info(ymodem_protocol_parser_t *yp, ymodem_sender_event_t *event)
{
    char path[FT_PATH_MAX + 1];
    vfs_stat_t st;
    vfs_fd_t fd;

    if (event->file_index != 0)
    {
        ft_set_send_file_info(yp, "", 0);
        FT_LOG("[FT] send: no more files, ending session\n");
        return;
    }

    if (ft_read_path(PID_FT_SEND_PATH, path, sizeof(path)) != 0)
    {
        FT_LOG("[FT] send: cannot read PID_FT_SEND_PATH\n");
        ft_session_close(false);
        return;
    }

    if (vfs_stat(path, &st) < 0 || st.type != VFS_TYPE_FILE)
    {
        FT_LOG("[FT] send: %s not found\n", path);
        ft_set_send_file_info(yp, "", 0); /* 让会话干净结束 */
        ft_session_close(false);
        return;
    }

    ft_lock();
    if (g_ft.fd >= 0) /* 上一会话残留句柄 */
        vfs_close(g_ft.fd);
    fd = vfs_open(path, VFS_O_RDONLY);
    g_ft.fd = fd;
    g_ft.file_size = st.size;
    g_ft.transferred = 0;
    ft_unlock();

    if (fd < 0)
    {
        FT_LOG("[FT] send: open %s failed (%d)\n", path, (int)fd);
        ft_session_close(false);
        return;
    }

    ft_set_send_file_info(yp, ft_name_of(path), st.size);
    ymodem_sender_enable_1k(&yp->pri.sender);

    FT_LOG("[FT] send: %s (%u bytes)\n", path, (unsigned)st.size);
}

/** @brief 收到 DATA_PACKET: 按帧序号偏移读出 1K 数据交给发送器 */
static void ft_send_data(ymodem_sender_event_t *event)
{
    ssize_t actual = 0;

    ft_lock();
    if (g_ft.fd >= 0)
        actual = vfs_pread(g_ft.fd, event->data, YMODEM_STX_DATA_LEN_BYTE,
                           (off_t)(event->data_seq * YMODEM_STX_DATA_LEN_BYTE));
    if (actual > 0)
        g_ft.transferred += (uint32_t)actual;
    ft_unlock();

    event->data_len = (actual > 0) ? (uint32_t)actual : 0;
}

static void ft_send_frame_ready(protocol_parser_t *parser, void *parsed_data, void *user_ctx)
{
    ymodem_sender_event_t *event = (ymodem_sender_event_t *)parsed_data;

    (void)user_ctx;

    if (!event)
        return;

    switch (event->type)
    {
    case YMODEM_SENDER_EVENT_FILE_INFO:
        ft_send_file_info((ymodem_protocol_parser_t *)parser, event);
        break;

    case YMODEM_SENDER_EVENT_DATA_PACKET:
        ft_send_data(event);
        break;

    case YMODEM_SENDER_EVENT_TRANSFER_COMPLETE:
        /* 本文件发完; 会话还没结束 —— 还要发空文件名包, 等 SESSION_FINISHED */
        FT_LOG("[FT] send: complete (%u bytes)\n", (unsigned)g_ft.transferred);
        ft_session_close(false);
        break;

    case YMODEM_SENDER_EVENT_SESSION_FINISHED:
        /* 空文件名包已发出, 会话真正结束: 解链 */
        FT_LOG("[FT] send: session finished\n");
        ft_session_close(false);
        ft_release_chain(parser);
        break;

    case YMODEM_SENDER_EVENT_ERROR:
        /* 对端 CAN / 重传超限: 传输中止 */
        FT_LOG("[FT] send: aborted by protocol\n");
        ft_session_close(false);
        ft_release_chain(parser);
        break;

    default:
        break;
    }
}

/* ================================================================
 *  装配 / 拆卸
 * ================================================================ */

int file_transfer_attach(const file_transfer_cfg_t *cfg)
{
    if (!cfg || !cfg->chain || !cfg->tx_write)
        return PARAM_ERR_NOT_SUPPORTED;
    if (g_ft.recv_parser || g_ft.send_parser)
        return PARAM_ERR_NOT_SUPPORTED; /* 已装配 */

    g_ft.chain = cfg->chain;
    g_ft.tx_write = cfg->tx_write;
    g_ft.fd = VFS_FD_INVALID;
    g_ft.file_size = 0;
    g_ft.transferred = 0;
    g_ft.peer_name[0] = '\0';

    if (!g_ft.lock)
    {
        g_ft.lock = sys_mutex_create();
        if (!g_ft.lock)
        {
            FT_LOG("[FT] attach: cannot create session lock\n");
            goto fail_link;
        }
    }

    /* ---- 接收器 (大 rx + 4B 应答 tx) ---- */
    g_ft.recv_parser = ymodem_protocol_create_receiver(
        s_recv_rx, sizeof(s_recv_rx), s_recv_tx, sizeof(s_recv_tx));
    if (!g_ft.recv_parser)
    {
        FT_LOG("[FT] attach: create receiver failed\n");
        goto fail_link;
    }
    protocol_parser_set_callbacks((protocol_parser_t *)g_ft.recv_parser,
                                  ft_recv_frame_ready, NULL, ft_tx_ready, NULL);

    /* ---- 发送器 (4B 虚拟 rx + 大 tx) ---- */
    g_ft.send_parser = ymodem_protocol_create_sender(
        s_send_rx, sizeof(s_send_rx), s_send_tx, sizeof(s_send_tx));
    if (!g_ft.send_parser)
    {
        FT_LOG("[FT] attach: create sender failed\n");
        goto fail_destroy;
    }
    protocol_parser_set_callbacks((protocol_parser_t *)g_ft.send_parser,
                                  ft_send_frame_ready, NULL, ft_tx_ready, NULL);

    /* ---- 挂链: 先收后发, 且整体在 imperx/camyu 之前 (顺序契约) ---- */
    if (!protocol_chain_add_parser(cfg->chain, (protocol_parser_t *)g_ft.recv_parser))
    {
        FT_LOG("[FT] attach: chain full on receiver\n");
        goto fail_destroy;
    }
    if (!protocol_chain_add_parser(cfg->chain, (protocol_parser_t *)g_ft.send_parser))
    {
        FT_LOG("[FT] attach: chain full on sender\n");
        protocol_chain_remove_parser(cfg->chain, (protocol_parser_t *)g_ft.recv_parser);
        goto fail_destroy;
    }

    /* 此处不打印路径: attach 早于 param_init, 参数尚未加载 (见 init_thread.c) */
    FT_LOG("[FT] attach: ymodem rx+tx attached to chain\n");
    return PARAM_OK;

fail_destroy:
    if (g_ft.recv_parser)
    {
        protocol_parser_destroy((protocol_parser_t *)g_ft.recv_parser);
        g_ft.recv_parser = NULL;
    }
    if (g_ft.send_parser)
    {
        protocol_parser_destroy((protocol_parser_t *)g_ft.send_parser);
        g_ft.send_parser = NULL;
    }
fail_link:
    /* 失败路径必须清掉注入的指针: 否则调用方销毁 chain 后 g_ft.chain 悬空,
       且下一次 attach 的"已装配"检查也判断不出这是残留状态 */
    g_ft.chain = NULL;
    g_ft.tx_write = NULL;
    return PARAM_ERR_NO_MEMORY;
}

void file_transfer_detach(void)
{
    ft_reset_session();

    /* remove 顺手清锁定, 且必须早于 chain destroy */
    if (g_ft.chain)
    {
        if (g_ft.recv_parser)
            protocol_chain_remove_parser(g_ft.chain, (protocol_parser_t *)g_ft.recv_parser);
        if (g_ft.send_parser)
            protocol_chain_remove_parser(g_ft.chain, (protocol_parser_t *)g_ft.send_parser);
    }

    if (g_ft.recv_parser)
    {
        protocol_parser_destroy((protocol_parser_t *)g_ft.recv_parser);
        g_ft.recv_parser = NULL;
    }
    if (g_ft.send_parser)
    {
        protocol_parser_destroy((protocol_parser_t *)g_ft.send_parser);
        g_ft.send_parser = NULL;
    }

    if (g_ft.lock)
    {
        sys_mutex_destroy(g_ft.lock);
        g_ft.lock = NULL;
    }

    g_ft.chain = NULL;
    g_ft.tx_write = NULL;
}

/* ================================================================
 *  查询
 * ================================================================ */

bool file_transfer_wants_bulk_rx(void)
{
    return ft_is_recv_locked();
}

ft_state_t file_transfer_get_state(void)
{
    if (ft_is_recv_locked())
        return FT_STATE_RECV;
    if (ft_is_send_locked())
        return FT_STATE_SEND;
    return FT_STATE_IDLE;
}

/* ================================================================
 *  会话启动 (由参数 exec 回调调用)
 * ================================================================ */

int file_transfer_start_recv(void)
{
    char dir[FT_PATH_MAX + 1];

    if (!g_ft.chain || !g_ft.recv_parser)
        return PARAM_ERR_NOT_SUPPORTED;

    /* 先验配置再启动: 配置错不该占用会话槽 */
    if (ft_read_path(PID_FT_RECV_PATH, dir, sizeof(dir)) != 0 ||
        file_transfer_check_path(dir, true) != 0)
    {
        FT_LOG("[FT] start_recv: invalid PID_FT_RECV_PATH\n");
        return PARAM_ERR_OUT_OF_RANGE;
    }

    ft_reset_session(); /* 清掉上一会话可能残留的句柄与统计 */

    if (!ymodem_adapter_start_receiver(g_ft.recv_parser))
        return PARAM_ERR_NOT_SUPPORTED;

    protocol_chain_set_locked_parser(g_ft.chain, (protocol_parser_t *)g_ft.recv_parser);
    FT_LOG("[FT] start_recv: armed, waiting for '%c' handshake\n", YMODEM_C);
    return PARAM_OK;
}

int file_transfer_start_send(void)
{
    char path[FT_PATH_MAX + 1];

    if (!g_ft.chain || !g_ft.send_parser)
        return PARAM_ERR_NOT_SUPPORTED;

    /* 启动前只校验父目录: 文件本身可能稍后才上传, 存在性在 FILE_INFO 时判 */
    if (ft_read_path(PID_FT_SEND_PATH, path, sizeof(path)) != 0 ||
        file_transfer_check_path(path, false) != 0)
    {
        FT_LOG("[FT] start_send: invalid PID_FT_SEND_PATH\n");
        return PARAM_ERR_OUT_OF_RANGE;
    }

    ft_reset_session();

    if (!ymodem_adapter_start_sender(g_ft.send_parser))
        return PARAM_ERR_NOT_SUPPORTED;

    protocol_chain_set_locked_parser(g_ft.chain, (protocol_parser_t *)g_ft.send_parser);
    FT_LOG("[FT] start_send: armed, waiting for peer 'C'\n");
    return PARAM_OK;
}
