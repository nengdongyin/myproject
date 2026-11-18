#include "protocol_chain_init.h"
#include <zephyr/logging/log.h>
#include "imperx_cmd_map.h"
#include "param_manager.h"
#include <string.h>
#include "param_dump.h"
#include "file_transfer.h"
#include "bsp_uart.h" /* bsp_comm_write: 协议帧出口 */

LOG_MODULE_REGISTER(chain_test, LOG_LEVEL_DBG);

/* shell 的字节入口不走协议透传帧了: 通信串口由 core/console 的 console_mode
   在“协议解析 / shell”之间时分复用 (WRITE 0xF003 切 shell, shell 内
   `uart_mode proto` 切回)。

   本文件的职责只剩"协议链": 建链 → 挂 ymodem → 建/挂 imperx、camyu (+各自
   回调) → 拆卸。以下三件事**不在这里** (曾在, 已拆走):
     - 串口设备/波特率/中断/三路出口  → platform/bsp/bsp_uart.c
     - param_dump 输出 / 日志 / shell / md-mw 白名单 → diag_init.c
     - 装配顺序                        → init_thread.c (唯一的顺序表) */

/** @brief 各协议共用的发送就绪回调 (协议帧出口, 不做任何模式判断) */
static void on_tx_ready(protocol_parser_t *parser, void *user_ctx)
{
    (void)user_ctx;
    bsp_comm_write(parser->tx.buffer, parser->tx.data_len);
}

/* ================================================================
 *  字节序转换 — wire format 为 big-endian
 * ================================================================ */

static inline uint16_t endian_swap16(uint16_t val)
{
    return (uint16_t)(((val & 0xFF00u) >> 8) | ((val & 0x00FFu) << 8));
}

static inline uint32_t endian_swap32(uint32_t val)
{
    return ((val & 0xFF000000ul) >> 24) | ((val & 0x00FF0000ul) >> 8)
         | ((val & 0x0000FF00ul) << 8)  | ((val & 0x000000FFul) << 24);
}

/* ================================================================
 *  协议回调
 *
 *  地址 → param_id 的映射全部在 imperx_cmd_map.c 的 g_cmd_map 里,
 *  本文件不做任何命令分发:
 *   - WRITE: param_write_raw —— 对普通参数落位, 对 EXEC 参数自动路由到
 *            模块的 exec 回调 (文件传输的启动命令就是这样触发的);
 *   - READ : param_read —— 回填 4 字节应答。
 * ================================================================ */

static void imperx_on_frame_ready(protocol_parser_t *parser,
                                  void *parsed_data, void *user_ctx)
{
    (void)user_ctx;
    imperx_private_t *pri = (imperx_private_t *)parsed_data;
    imperx_cmd_type_t cmd = (pri->parsed_len == 0) ? IMPERX_CMD_READ
                                                   : IMPERX_CMD_WRITE;

    LOG_INF("Rev Imperx %s, Address: 0x%04llX",
            (cmd == IMPERX_CMD_READ) ? "READ" : "WRITE",
            (unsigned long long)pri->parsed_id);

    uint16_t raw_addr = (uint16_t)(pri->parsed_id & 0xFFFF);
    uint16_t addr = endian_swap16(raw_addr);

    const cmd_map_entry_t *e = cmd_map_lookup(addr);
    if (!e) {
        LOG_WRN("Unknown address: 0x%04X", addr);
        return;
    }

    if (cmd == IMPERX_CMD_WRITE) {
        uint32_t raw;
        memcpy(&raw, pri->parsed_data, 4);
        uint32_t val = endian_swap32(raw);
        /* 字节级协议接口用 param_write_raw: 按条目类型落位
           (UINT/INT/FLOAT 的 32-bit 位模式都正确), 而 typed 包装
           校验类型 —— 对 FLOAT 参数 (如 gain) 调
           param_write_u32 会被拒绝。
           EXEC 参数 (如 PID_FT_RECV_START) 由它在内部路由到 exec 回调。 */
        int ret = param_write_raw(e->param_id, (const uint8_t *)&val, sizeof(val));
        if (ret != PARAM_OK) {
            LOG_WRN("Write 0x%04X -> %s", addr, param_strerror(ret));
        }
        param_flush();
        param_dump(PARAM_MODULE_ID(e->param_id), NULL);
    } else {
        param_value_t v;
        int ret = param_read(e->param_id, &v);
        if (ret == PARAM_OK) {
            static uint8_t s_resp[4];
            uint32_t raw = endian_swap32(v.u32);
            memcpy(s_resp, &raw, 4);
            pri->parsed_data = s_resp;
            pri->parsed_len = sizeof(s_resp);
        }
    }

    LOG_INF("Resp Imperx %s, Address: 0x%04llX",
            (cmd == IMPERX_CMD_READ) ? "READ" : "WRITE",
            (unsigned long long)pri->parsed_id);
}

static void camyu_on_frame_ready(protocol_parser_t *parser,
                                 void *parsed_data, void *user_ctx)
{
    camyu_private_t *pri = (camyu_private_t *)parsed_data;

    (void)parser;
    (void)user_ctx;

    LOG_INF("Camyu Opcode:%d, Address: 0x%08llX",
            pri->current_frame_info.opcode,
            (unsigned long long)pri->parsed_id);

    if (pri->current_frame_info.opcode == PCTOCAMERA_READ)
    {
        static uint8_t s_resp[4] = {0xAA, 0xBB, 0xCC, 0xDD};
        pri->parsed_data = s_resp;
        pri->parsed_data_len = sizeof(s_resp);
    }
}

/* ================================================================
 *  装配
 * ================================================================ */

bool protocol_chain_init(app_ctx_t *ctx)
{
    ctx->chain = protocol_chain_create(4); /* ymodem×2 + imperx + camyu */
    if (!ctx->chain)
    {
        LOG_ERR("Failed to create protocol chain");
        return false;
    }

    /* ---- ymodem 文件传输组件 ----
       顺序契约: 必须第一个挂。protocol_chain_feed 按添加顺序依次尝试,
       ymodem 在链首才能先于 imperx/camyu 匹配到自己的帧。
       组件内部负责创建收/发两个 parser、设置回调并 add_parser。 */
    file_transfer_cfg_t ft_cfg = {
        .chain    = ctx->chain,
        .tx_write = bsp_comm_write, /* 协议帧出口: 只进通讯口, 不做模式判断 */
    };
    int ft_ret = file_transfer_attach(&ft_cfg);
    if (ft_ret != PARAM_OK)
    {
        LOG_ERR("file_transfer attach failed: %s", param_strerror(ft_ret));
        goto fail_ft;
    }

    /* ---- Imperx ---- */
    ctx->imperx_parser = imperx_protocol_create(
        ctx->imperx_rx_buf, sizeof(ctx->imperx_rx_buf),
        ctx->imperx_tx_buf, sizeof(ctx->imperx_tx_buf));
    if (!ctx->imperx_parser)
    {
        LOG_ERR("Failed to create Imperx parser");
        goto fail_imperx;
    }
    protocol_parser_set_callbacks((protocol_parser_t *)ctx->imperx_parser,
                                  imperx_on_frame_ready, ctx,
                                  on_tx_ready, ctx);
    protocol_chain_add_parser(ctx->chain, (protocol_parser_t *)ctx->imperx_parser);

    /* ---- Camyu ---- */
    ctx->camyu_parser = camyu_protocol_create(
        ctx->camyu_rx_buf, sizeof(ctx->camyu_rx_buf),
        ctx->camyu_tx_buf, sizeof(ctx->camyu_tx_buf));
    if (!ctx->camyu_parser)
    {
        LOG_ERR("Failed to create Camyu parser");
        goto fail_camyu;
    }
    protocol_parser_set_callbacks((protocol_parser_t *)ctx->camyu_parser,
                                  camyu_on_frame_ready, ctx,
                                  on_tx_ready, ctx);
    protocol_chain_add_parser(ctx->chain, (protocol_parser_t *)ctx->camyu_parser);

    LOG_INF("Protocol chain initialized: Ymodem(Rx/Tx), Imperx, Camyu");
    return true;

fail_camyu:
    protocol_parser_destroy((protocol_parser_t *)ctx->imperx_parser);
    ctx->imperx_parser = NULL;
fail_imperx:
    file_transfer_detach();
fail_ft:
    protocol_chain_destroy(ctx->chain);
    ctx->chain = NULL;
    return false;
}

void protocol_chain_cleanup(app_ctx_t *ctx)
{
    /* 注: shell/log 的关闭属于诊断设施, 由编排层调 diag_deinit() —— 本函数只管协议链。
       组件必须先拆: remove_parser 需要 chain 存活, 且会顺手清掉链上的锁定。
       现有顺序是 destroy chain → destroy parser, 这里必须反过来。 */
    file_transfer_detach();

    if (ctx->imperx_parser)
    {
        protocol_parser_destroy((protocol_parser_t *)ctx->imperx_parser);
        ctx->imperx_parser = NULL;
    }
    if (ctx->camyu_parser)
    {
        protocol_parser_destroy((protocol_parser_t *)ctx->camyu_parser);
        ctx->camyu_parser = NULL;
    }
    if (ctx->chain)
    {
        protocol_chain_destroy(ctx->chain);
        ctx->chain = NULL;
    }
}
