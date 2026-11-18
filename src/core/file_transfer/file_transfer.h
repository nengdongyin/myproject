/**
 * @file file_transfer.h
 * @brief 文件传输组件 —— ymodem 会话引擎的公共接口
 *
 * @par 设计模型 (与 param_service 同构的"旋钮即参数")
 * 本组件由两个编译单元组成, 各有独立入口:
 *
 *   | 单元                    | 职责                                            | 入口                        |
 *   |-------------------------|-------------------------------------------------|-----------------------------|
 *   | file_transfer.c         | 协议侧: parser 创建 / 挂链 / 收发事件 / 文件 I/O   | file_transfer_attach()      |
 *   | file_transfer_param.c   | 参数侧: 2 条 STRING 配置 + 2 条 EXEC 命令         | file_transfer_module_init() |
 *
 * 协议层只做 `地址 → param_id → param_read/write/exec`, 不需要专用命令通道。
 *
 * @par 组件边界 (四条)
 *  - **不认识 UART**: 字节出口经 @ref ft_tx_fn 注入, 组件只管把 tx buffer 交出去。
 *  - **不认识 chain 生命周期**: chain 句柄由调用方注入, 组件只 add/remove parser。
 *  - **不认识协议帧格式**: 触发点由上层做地址映射后调 param_exec()。
 *  - **会借用串口消费者 (console_mode)**: 会话期间线上必须是纯 ymodem 字节,
 *    所以在启动时 console_mode_save_set(PROTO) 借走串口、在收尾时
 *    console_mode_restore() 归还 (irq_save / irq_restore 语义)。借用槽由
 *    console_mode 内部维护, 本组件不存副本 —— 但恢复点只能由本组件持有,
 *    因为会话结束是异步的, 触发它的命令早已返回。
 *
 * @par 没有周期性维护入口
 * 会话状态不单独存储, 由协议链当前锁定的 parser 派生: 启动时组件把链锁到
 * 收/发 parser 上, 正常收尾在帧事件里解锁, 协议层致命错误时
 * protocol_chain_feed 会自行解锁 (与改造前行为一致)。
 * 因此主循环**不需要**为本组件做任何额外调用 —— 协议超时重传由各 parser
 * 的 poll 钩子驱动, 沿用原有的 protocol_chain_check_timeout_poll()。
 *
 * @par 顺序契约 (必须遵守)
 * @ref file_transfer_attach 必须在 imperx / camyu 的 protocol_chain_add_parser
 * **之前**调用 —— protocol_chain_feed 按添加顺序依次尝试, ymodem 需在链首。
 *
 * @par 装配顺序
 * @code
 *   ctx->chain = protocol_chain_create(4);
 *   file_transfer_cfg_t cfg = { .chain = ctx->chain, .tx_write = uart_tx_write };
 *   file_transfer_attach(&cfg);
 *   ... imperx / camyu 创建并 add_parser ...
 *
 *   // 传输由参数命令触发 (地址映射在 imperx_cmd_map 里做)
 *   param_exec(PID_FT_RECV_START, NULL);
 * @endcode
 */

#ifndef FILE_TRANSFER_H
#define FILE_TRANSFER_H

#include <stdint.h>
#include <stdbool.h>
#include "protocol_chain.h"
#include "vfs.h"
#include "param_manager.h"
#include "module_ids.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* ================================================================
 *  参数 ID (MODULE_FILE_TRANSFER)
 *
 *  集中在此定义, 供 file_transfer_param.c (定义参数表)、
 *  imperx_cmd_map.c (地址 → ID 映射) 与测试共同引用, 避免多处写死字面量。
 *  数值一旦发布不得变更 (module_ids.h: "ID 永不漂移")。
 *
 *  | 参数                            | 类型   | 语义                                     |
 *  |---------------------------------|--------|------------------------------------------|
 *  | 0x00 PID_FT_RECV_PATH           | STRING | 接收目录 (文件名由对端 FILE_INFO 给)       |
 *  | 0x01 PID_FT_SEND_PATH           | STRING | 发送完整路径 (含文件名)                    |
 *  | 0x02 PID_FT_RECV_START          | EXEC   | 启动接收                                  |
 *  | 0x03 PID_FT_SEND_START          | EXEC   | 启动发送                                  |
 * ================================================================ */
/** @brief 接收目录 (如 "/lfs")；对端在 FILE_INFO 包里给出的文件名拼在其后 */
#define PID_FT_RECV_PATH  MAKE_PARAM_ID(MODULE_FILE_TRANSFER, 0x00)
/** @brief 发送文件完整路径 (如 "/lfs/image.bin") */
#define PID_FT_SEND_PATH  MAKE_PARAM_ID(MODULE_FILE_TRANSFER, 0x01)
/** @brief 启动 ymodem 接收 (EXEC) */
#define PID_FT_RECV_START MAKE_PARAM_ID(MODULE_FILE_TRANSFER, 0x02)
/** @brief 启动 ymodem 发送 (EXEC) */
#define PID_FT_SEND_START MAKE_PARAM_ID(MODULE_FILE_TRANSFER, 0x03)

/* ================================================================
 *  容量
 * ================================================================ */

/** @brief 路径字符串最大字符数 (不含结尾 '\0')
 *  @note 取 VFS_PATH_MAX - 1, 保证 "目录 + '/' + 文件名" 拼装后仍不超 VFS 上限 */
#define FT_PATH_MAX (VFS_PATH_MAX - 1)

/* ================================================================
 *  会话状态
 * ================================================================ */

/**
 * @brief 会话状态 —— **派生值, 不作为字段存储**
 *
 * 由协议链当前锁定的 parser 推导 (@ref file_transfer_get_state):
 *  - 启动时组件把链锁到收/发 parser 上 → 会话在进行中;
 *  - 正常收尾在帧事件里解锁;
 *  - 协议层致命错误时 protocol_chain_feed 自行解锁。
 *
 * 所以组件没有会话状态变量, 也就不需要任何周期性维护入口。
 */
typedef enum
{
    FT_STATE_IDLE = 0, /**< 空闲 —— 链未锁在本组件的 parser 上 */
    FT_STATE_RECV,     /**< 正在接收 (对端 → 本机) */
    FT_STATE_SEND,     /**< 正在发送 (本机 → 对端) */
} ft_state_t;

/* ================================================================
 *  装配
 * ================================================================ */

/**
 * @brief 字节流出口
 *
 * 组件构造好待发送数据后调用。实现方负责把数据真正写到物理通道
 * (如 uart_poll_out 循环)。组件内部不持有任何 UART 句柄。
 *
 * @param data 待发送数据
 * @param len  字节数 (调用时保证 > 0)
 */
typedef void (*ft_tx_fn)(const uint8_t *data, uint32_t len);

/** @brief 装配配置 */
typedef struct
{
    protocol_chain *chain;    /**< 必填: 已 create、尚未挂满的协议链 */
    ft_tx_fn        tx_write; /**< 必填: 字节流出口 */
} file_transfer_cfg_t;

/**
 * @brief 创建 ymodem 收发 parser、设置回调、挂入协议链
 *
 * 内部持有两块静态收发缓冲区 (1029 + 4 各一对), 无堆分配。
 *
 * @param cfg 装配配置 (不能为 NULL, 且 chain / tx_write 均非 NULL)
 * @return PARAM_OK 成功; PARAM_ERR_NOT_SUPPORTED 参数非法或已装配;
 *         PARAM_ERR_NO_MEMORY 解析器创建失败或链已满
 *
 * @note **顺序契约**: 必须早于 imperx / camyu 的 add_parser (见文件头)。
 * @note 重复调用返回 PARAM_ERR_NOT_SUPPORTED, 不覆盖已有装配。
 * @note 失败路径完整回滚, 不残留注入的 chain / tx_write 指针。
 */
int file_transfer_attach(const file_transfer_cfg_t *cfg);

/**
 * @brief 从协议链摘除并销毁 ymodem 收发 parser
 *
 * 顺序: 关文件句柄 → remove_parser (顺手清锁定) → destroy → 释放会话锁。
 * **必须在 protocol_chain_destroy() 之前调用** (remove 需要 chain)。
 *
 * @note 未装配时为空操作, 可安全重复调用。
 */
void file_transfer_detach(void);

/* ================================================================
 *  查询
 * ================================================================ */

/**
 * @brief 是否正在接收 —— 供收包线程决定批量读管道
 *
 * ymodem 数据帧最大 1029 字节, 一次读满比逐字节读快两个数量级。
 * 判定等价于"协议链当前是否锁在本组件的接收器上", 调用方因此无需认识
 * parser 类型, 也无需维护任何状态。
 *
 * @return true 正在接收
 */
bool file_transfer_wants_bulk_rx(void);

/** @brief 取当前会话状态 (由链的锁定 parser 派生; 测试/诊断用) */
ft_state_t file_transfer_get_state(void);

/**
 * @brief 路径合法性校验 —— 供参数写入回调使用
 *
 * 规则 (全部满足才返回 0):
 *  - 绝对路径 (以 '/' 开头), 长度 ≤ FT_PATH_MAX, 不以 '/' 结尾;
 *  - @p want_dir 为 true: 路径本身必须是已存在的目录;
 *  - @p want_dir 为 false: 必须含文件名 (至少一个 '/' 后还有内容),
 *    且其父目录必须是已存在的目录 (文件本身可以不存在的 —— 可能稍后上传)。
 *
 * @param path     待校验路径
 * @param want_dir true = 期望目录, false = 期望文件
 * @return 0 合法; -1 非法
 */
int file_transfer_check_path(const char *path, bool want_dir);

/* ================================================================
 *  会话启动 —— 由参数 exec 回调调用
 * ================================================================ */

/**
 * @brief 启动一次 ymodem 接收
 *
 * 读 PID_FT_RECV_PATH → 校验目录 → 发握手 'C' → 锁定协议链。
 *
 * @return PARAM_OK 已发起 (注意: **不代表传输完成**);
 *         PARAM_ERR_OUT_OF_RANGE 接收目录配置非法;
 *         PARAM_ERR_NOT_SUPPORTED 未装配或启动失败
 *
 * @note **重复调用是安全的**: ymodem_receiver_start() 内部整块复位协议状态,
 *       上一会话无论正常结束还是异常中断都会被丢弃 (残留句柄由内部重置关掉)。
 *       **刻意不做"已在会话中"拒绝** —— 接收器在对端静默消失时会永久停在
 *       TRANSFERRING / FINISHING, 一旦拒绝重启设备就再也收不了文件。
 */
int file_transfer_start_recv(void);

/**
 * @brief 启动一次 ymodem 发送
 *
 * 读 PID_FT_SEND_PATH → 校验父目录 → 等待对端 'C' → 锁定协议链。
 *
 * @return 同 @ref file_transfer_start_recv
 */
int file_transfer_start_send(void);

#ifdef __cplusplus
}
#endif

#endif /* FILE_TRANSFER_H */
