/**
 * @file mem_access.h
 * @brief 物理地址读写的白名单闸门 + md / mw 命令
 *
 * @par 为什么需要这一层
 * MCU 上"按地址读写"是调试利器, 但踩到非法地址就是当场 HardFault (Cortex-M
 * 没有可恢复的页错误)。所以本模块不给"任意地址读写", 只给**白名单内的**读写:
 * 白名单由板级注入 (`mem_access_configure`), 表里每段还带一个 `writable` 标志,
 * 只读段 (如 XIP flash) 一律拒绝写。
 *
 * @par fail-closed
 * 没有调用过 `mem_access_configure()` 时, `md`/`mw` **全部拒绝**并按
 * "no whitelist" 报错 —— 绝不做"默认全放开"。这样即使某个产品忘了注入表,
 * 命令也只是不可用, 而不是变成一颗地雷。
 *
 * @par 判定规则
 * `[addr, addr+len)` 必须**整段落在同一段白名单内**(跨段即拒), 写操作还要求
 * 该段 `writable != 0`; `len == 0` 或地址区间回绕一律拒。
 *
 * @par 已知风险 (白名单挡不住的部分)
 * 命中白名单也不代表"读一定安全": 某些外设寄存器有读副作用 (读清标志、
 * 触发 FIFO 弹出), 写外设可能锁总线。所以板级白名单默认**只给
 * flash(只读) + sram(可读写)**, 外设段要用请显式加, 并自己承担后果。
 *
 * @note 依赖纪律: 只依赖 shell_api.h (门面) + stdint, 不认识 RTOS、不认识板子。
 *       命令在协议接收线程 / 调试串口线程里同步执行, 输出已封顶。
 */

#ifndef CORE_DEBUG_MEM_ACCESS_H
#define CORE_DEBUG_MEM_ACCESS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 *  白名单
 * ================================================================ */

/** @brief 一段允许访问的内存区间 */
typedef struct
{
    const char *name;     /**< 诊断名 ("flash" / "sram" / ...) —— 放首位: 表里一眼看出是哪段 */
    uintptr_t   base;     /**< 段起始物理地址 */
    size_t      size;     /**< 段长度 (字节) */
    uint8_t     writable; /**< 0 = 只读段 (mw 拒绝); 1 = 可读写 */
} mem_region_t;

/**
 * @brief 注入白名单表 (板级资源, 由 app 在启动阶段调用)
 *
 * @param tbl 段表 (须具备静态生命周期, 本模块只存指针)
 * @param n   段数
 * @return 0 成功; -EINVAL tbl 为 NULL 或 n == 0 (此时白名单保持为空 → 命令全拒)
 *
 * @note 传 NULL/0 可以显式清空白名单 (同样是 fail-closed)。
 */
int mem_access_configure(const mem_region_t *tbl, size_t n);

/**
 * @brief 检查一段区间是否允许访问
 *
 * @param addr       起始地址
 * @param len        长度 (字节, 须 > 0)
 * @param want_write 0 = 只读访问; 非 0 = 写访问 (要求段可写)
 * @return 0 允许; -EPERM 不在白名单 / 段只读 / 未配置白名单;
 *         -EINVAL len 为 0 或 [addr, addr+len) 回绕
 */
int mem_access_check(uintptr_t addr, size_t len, int want_write);

/**
 * @brief 已配置的段数 (诊断/测试用)
 * @return 段数 (0 = 未配置)
 */
size_t mem_access_region_count(void);

#ifdef __cplusplus
}
#endif

#endif /* CORE_DEBUG_MEM_ACCESS_H */
