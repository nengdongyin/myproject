/**
 * @file  protocol_chain.c
 * @brief 协议链(责任链模式)实现
 *
 * 管理多个协议解析器, 自动将接收数据匹配到首个能成功解析的解析器.
 * 一旦匹配成功, 该解析器被"锁定"用于后续数据.
 *
 * @author Protocol Parser Framework
 * @version 1.0
 * @date   2024
 */
#include "protocol_chain.h"


/**
 * @struct protocol_chain
 * @brief  协议链管理器(不透明结构体)
 */
struct protocol_chain {
    protocol_parser_t** parsers;       /**< 解析器指针数组 */
    uint32_t            max_parsers;  /**< 最大解析器数量 */
    uint32_t            count;        /**< 当前解析器数量 */
    protocol_parser_t*  locked_parser; /**< 当前锁定的解析器(NULL = 无) */
};

/* ======================== 创建/销毁 ================================= */

protocol_chain* protocol_chain_create(uint32_t max_parsers) {
    if (max_parsers == 0) {
        return NULL;
    }

    protocol_chain* chain = (protocol_chain*)system_malloc(sizeof(protocol_chain));
    if (!chain) {
        return NULL;
    }

    chain->parsers = (protocol_parser_t**)system_malloc(
        max_parsers * sizeof(protocol_parser_t*));
    if (!chain->parsers) {
        system_free(chain);
        return NULL;
    }
    memset(chain->parsers, 0, max_parsers * sizeof(protocol_parser_t*));

    chain->max_parsers = max_parsers;
    chain->count = 0;
    chain->locked_parser = NULL;

    return chain;
}

void protocol_chain_destroy(protocol_chain* chain) {
    if (chain) {
        system_free(chain->parsers);
        system_free(chain);
    }
}

/* ======================== 解析器管理 ================================ */

bool protocol_chain_add_parser(protocol_chain* chain, protocol_parser_t* parser) {
    if (!chain || !parser || chain->count >= chain->max_parsers) {
        return false;
    }
    chain->parsers[chain->count++] = parser;
    return true;
}

bool protocol_chain_remove_parser(protocol_chain* chain, protocol_parser_t* parser) {
    if (!chain || !parser) {
        return false;
    }

    for (uint32_t i = 0; i < chain->count; i++) {
        if (chain->parsers[i] == parser) {
            /* 将后续解析器前移 */
            for (uint32_t j = i; j < chain->count - 1; j++) {
                chain->parsers[j] = chain->parsers[j + 1];
            }
            chain->count--;

            if (chain->locked_parser == parser) {
                parser->locked = false;
                chain->locked_parser = NULL;
            }
            return true;
        }
    }
    return false;
}

/* ======================== 数据送入 ================================== */

parser_error_t protocol_chain_feed(protocol_chain* chain,
                                   const uint8_t* data, uint32_t len) {
    parser_error_t gen_err;
#if PROTOCOL_CHAIN_AUTO_MATCH
    protocol_parser_t* just_failed = NULL; /* 刚报致命错误的解析器, 遍历时跳过 */
#endif

    if (!chain || !data || !len || chain->count == 0) {
        return PARSER_ERR_INVALID_PARAM;
    }

    /* 如果已锁定解析器, 直接转发数据 */
    if (chain->locked_parser != NULL) {
        /* 先缓存本次喂入的解析器: 回调里可能改掉 chain->locked_parser,
           之后不能再重新读它(可能已是 NULL 或另一个解析器) */
        protocol_parser_t* locked = chain->locked_parser;

        gen_err = protocol_parser_parse_data(locked, data, len);

        /* 回调内改动过锁定 → 尊重用户的显式指定, 不再覆盖也不解引用旧指针:
         *   - 置 NULL   = 用户主动解链(如会话终结、强制重匹配)
         *   - 换成别的   = 用户手动切协议(如通过控制寄存器切换)
         * 与下方 NONE 分支的"用户回调已提前锁定"是同一个契约。 */
        if (chain->locked_parser != locked) {
            return gen_err;
        }

        if (parser_error_is_fatal(gen_err)) {
#if PROTOCOL_CHAIN_AUTO_MATCH
            locked->locked = false;
            chain->locked_parser = NULL;  /* 致命错误时解锁 */
            just_failed = locked;         /* 同一字节不在下方被它重解析一次 */
#else
            /* 手动模式: 致命错误**不解锁** —— 手动指定不该被一次错误冲垮,
               后续字节继续交给同一解析器。要换协议请显式 set_locked_parser */
            return gen_err;
#endif
        }
        else {
            return gen_err;
        }
    }

#if !PROTOCOL_CHAIN_AUTO_MATCH
    /* 手动模式: 未锁定 → 不解析任何数据, 数据被丢弃。
       需要解析行为请先 protocol_chain_set_locked_parser() */
    return PARSER_ERR_UNKNOWN;
#else
    /* 遍历每个解析器:
     * - NONE (成功)     → 锁定并返回
     * - INCOMPLETE      → 记录标记, 继续尝试其他解析器
     * - 致命错误        → 尝试下一个解析器 */
    bool saw_incomplete = false;
    for (uint32_t i = 0; i < chain->count; i++) {
        protocol_parser_t* parser = chain->parsers[i];

        /* 刚在锁定路径上失败的那个解析器跳过: 同一字节喂它第二次必然同样
           失败, 只会让 stats 与 on_frame_error 被多记一次 */
        if (parser == just_failed) {
            continue;
        }

        gen_err = protocol_parser_parse_data(parser, data, len);

        if (gen_err == PARSER_ERR_NONE) {
            /* 若用户回调已提前锁定其他解析器(如通过控制寄存器切换协议),
             * 则不再覆盖, 尊重用户的手动指定 */
            if (chain->locked_parser == NULL) {
                parser->locked = true;
                chain->locked_parser = parser;
            }
            return gen_err;
        }

        if (gen_err == PARSER_ERR_INCOMPLETE) {
            saw_incomplete = true;
            /* 继续尝试其他解析器 */
        }
    }

    /* 仅当解析器完全匹配时才锁定; 流式数据返回 INCOMPLETE */
    if (saw_incomplete) {
        return PARSER_ERR_INCOMPLETE;
    }

    return PARSER_ERR_UNKNOWN;
#endif /* PROTOCOL_CHAIN_AUTO_MATCH */
}

parser_error_t protocol_chain_feed_frame(protocol_chain* chain,
                                         const uint8_t* frame, uint32_t len) {
    parser_error_t gen_err;

    if (!chain || !frame || !len || chain->count == 0) {
        return PARSER_ERR_INVALID_PARAM;
    }

    /* 锁定状态下送入整帧 */
    if (chain->locked_parser != NULL) {
        protocol_parser_t* locked = chain->locked_parser;

        gen_err = protocol_parser_parse_data(locked, frame, len);

        /* 回调内改动过锁定 → 尊重用户的显式指定(同 protocol_chain_feed) */
        if (chain->locked_parser != locked) {
            return gen_err;
        }

#if PROTOCOL_CHAIN_AUTO_MATCH
        /* 自动模式: 任何错误都导致解锁 */
        if (gen_err != PARSER_ERR_NONE) {
            locked->locked = false;
            chain->locked_parser = NULL;
        }
#endif
        /* 手动模式保持锁定 */
        return gen_err;
    }

#if !PROTOCOL_CHAIN_AUTO_MATCH
    /* 手动模式: 未锁定 → 不解析 */
    return PARSER_ERR_UNKNOWN;
#else
    /* 遍历每个解析器 */
    for (uint32_t i = 0; i < chain->count; i++) {
        protocol_parser_t* parser = chain->parsers[i];

        gen_err = protocol_parser_parse_data(parser, frame, len);

        if (gen_err == PARSER_ERR_NONE) {
            if (chain->locked_parser == NULL) {
                parser->locked = true;
                chain->locked_parser = parser;
            }
            return gen_err;
        }
    }

    return PARSER_ERR_UNKNOWN;
#endif /* PROTOCOL_CHAIN_AUTO_MATCH */
}

/* ======================== 锁定管理 ================================== */

protocol_parser_t* protocol_chain_get_locked_parser(protocol_chain* chain) {
    return chain ? chain->locked_parser : NULL;
}

bool protocol_chain_set_locked_parser(protocol_chain* chain,
                                      protocol_parser_t* parser) {
    if (!chain) {
        return false;
    }
    if (parser == NULL) {
        if (chain->locked_parser) {
            chain->locked_parser->locked = false;
        }
        chain->locked_parser = NULL;
        return true;
    }

    for (uint32_t i = 0; i < chain->count; i++) {
        if (chain->parsers[i] == parser) {
            parser->locked = true;
            chain->locked_parser = parser;
            return true;
        }
    }
    return false;
}

/* ======================== 超时轮询 ================================== */

bool protocol_chain_check_timeout_poll(protocol_chain* chain) {
    if (!chain || chain->count == 0) {
        return false;
    }

    /* 若有锁定解析器, 仅轮询该解析器 */
    if (chain->locked_parser != NULL) {
        protocol_parser_t* parser = chain->locked_parser;

        if (protocol_parser_check_timeout_poll(parser)) {
            parser->locked = false;
            chain->locked_parser = NULL;
            return true;
        }
        return false;
    }

    /* 未锁定: 检查所有注册的解析器 */
    bool any_timeout = false;

    for (uint32_t i = 0; i < chain->count; i++) {
        protocol_parser_t* parser = chain->parsers[i];

        if (protocol_parser_check_timeout_poll(parser)) {
            any_timeout = true;
        }
    }
    return any_timeout;
}
