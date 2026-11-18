/**
 * @file console_mode.h
 * @brief 通信串口复用模式 —— 协议解析 / shell 二选一
 *
 * @par 解决什么问题
 * 产品在客户现场可能引不出独立调试串口, 只能借通信串口 (uart1) 应急调试。
 * 本模块把 uart1 的**字节消费者**做成二选一: 要么交给协议链解析, 要么交给
 * shell 逐字节解析。两个消费者互斥 —— 同一时刻只有一个是真相源。
 *
 * @par 为什么互斥而不是"双喂"
 * 双喂会同时踩三件事 (与吞吐无关):
 *   1. letter-shell 对每个插入的字符都回显, 协议二进制数据会被回显到线上;
 *   2. 0x0A/0x0D 在 letter-shell 里是"回车"键 → 二进制垃圾会被当命令行执行;
 *   3. `R`(0x52)/`W`(0x57) 正是 Imperx 帧头, 敲几个字符就能凑出完整帧并触发
 *      参数读写。所以只允许一个消费者。
 *
 * @par 切换入口 (单向进, 命令退出)
 *   - 协议 → shell: Imperx `WRITE 0xF003` (PID_CONSOLE_ENTER_SHELL)
 *   - shell → 协议: shell 命令 `uart_mode proto`
 *   协议侧走"旋钮即参数" (地址 → param_id → EXEC 回调), 与 file_transfer 的
 *   启动命令同一套机制, 不新开命令通道。
 *
 * @note **没有**"协议 → 协议"的旋钮: 进入 shell 模式后 RX 线程不再解析协议帧,
 *       那条命令即使存在也永远走不到 (或只是把已是协议模式的自己再设一遍)。
 *
 * @par 并发
 * 状态只被协议接收线程读写: EXEC 回调 (在 imperx frame_ready 里) 与 shell
 * 命令 (在 shell_rx_feed→shellHandler 里) 都跑在那一个线程上, 因此不需要锁。
 *
 * @note 本模块不做"协议卡死自动逃生", 也不做"shell 空闲自动回协议":
 *       前者属于协议实现的 bug, 后者按设计约定由技术支持人员显式切回
 *       (`uart_mode proto` 命令)。客户侧不接触通信串口。
 */

#ifndef CORE_CONSOLE_CONSOLE_MODE_H
#define CORE_CONSOLE_CONSOLE_MODE_H

#include <stdint.h>
#include "param_manager.h" /* MAKE_PARAM_ID */
#include "module_ids.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 *  模式
 * ================================================================ */

/** @brief 通信串口当前由谁消费 */
typedef enum
{
    CONSOLE_MODE_PROTO = 0, /**< 协议链解析 (默认, 也是产品正常态) */
    CONSOLE_MODE_SHELL = 1, /**< shell 逐字节解析 (应急调试态) */
} console_mode_t;

/**
 * @brief 取当前模式
 * @return CONSOLE_MODE_PROTO / CONSOLE_MODE_SHELL
 */
console_mode_t console_mode_get(void);

/**
 * @brief 切换模式 (幂等)
 *
 * @param mode 目标模式
 * @return PARAM_OK 成功 (含本来就是该模式的空操作);
 *         PARAM_ERR_OUT_OF_RANGE 非法模式;
 *         PARAM_ERR_NOT_SUPPORTED 目标为 shell 但 shell 门面被编译关闭
 *         (SHELL_BACKEND_NONE)
 */
int console_mode_set(console_mode_t mode);

/**
 * @brief 保存当前模式并切到 target —— **irq_save 语义**, 保存槽由本模块内部维护
 *
 * 给"临时占用串口的活动"用 (目前只有 ymodem 文件传输): 活动期间线上必须是纯
 * 协议字节流, 活动结束用 console_mode_restore() 还回去。
 *
 * @param target 活动期间要用的模式 (通常是 CONSOLE_MODE_PROTO)
 * @return **true = 本次调用抢到了保存槽**, 随后的 restore 由调用方负责;
 *         **false = 槽已被别的调用占着**, 本次什么都没做 (也不要调 restore,
 *         否则会把别人的借用提前还掉)。
 *
 * @note 为什么要有返回值: 上一会话尚未收尾就被重新启动时 (协议侧重试),
 *       第二次 save_set 返回 false —— 如果那次启动随后失败并无条件 restore,
 *       就会把还活着的第一次会话"还"回 shell, 让它的接收流被 shell 吃掉。
 * @note 槽只有一份 → 同一时刻只允许一个借用者。需要更复杂的所有权请改造成
 *       返回令牌的形式, 而不是在这里叠引用计数。
 * @note 切换失败 (target 非法 / shell 被编译关闭) 也照样占槽: 后续 restore
 *       会把状态还回去, 串口不会停在半切换状态。
 */
bool console_mode_save_set(console_mode_t target);

/**
 * @brief 恢复保存槽里的模式并清空槽 —— **irq_restore 语义**
 *
 * @note 幂等: 槽为空时什么都不做 (不打印、不改状态)。
 * @note 只有 console_mode_save_set() 返回 true 的那一方该调它; 另外"组件卸载"
 *       这类兜底路径也可以调 (清掉可能残留的借用)。
 */
void console_mode_restore(void);

/* ================================================================
 *  参数 ID (MODULE_CONSOLE)
 *
 *  EXEC: 只能被 param_exec / param_write_raw 触发
 *  (param_write 对 EXEC 返回 PARAM_ERR_EXEC_ONLY)。
 *  回程不用参数, 用 shell 命令 `uart_mode proto` (见上)。
 * ================================================================ */

/** @brief 切到 shell 模式 (EXEC) */
#define PID_CONSOLE_ENTER_SHELL MAKE_PARAM_ID(MODULE_CONSOLE, 0x00)

/**
 * @brief 注册本模块的参数表 (由 PARAM_MODULE_LIST 清单驱动调用)
 *
 * @note 由 module_ids.h 的 PARAM_MODULE_LIST 生成 extern 并在
 *       register_modules() 里调用, 业务代码不需要手工调。
 */
void console_mode_module_init(void);

#ifdef __cplusplus
}
#endif

#endif /* CORE_CONSOLE_CONSOLE_MODE_H */
