/**
 * @file file_transfer_param.c
 * @brief 文件传输组件 —— 参数侧 (2 条 STRING 配置 + 2 条 EXEC 命令)
 *
 * @par 模型: "旋钮即参数" (与 param_service.c 同构)
 * 协议层只做 `地址 → param_id → param_read/write/exec`, 不需要专用命令通道。
 *
 *   | 参数                  | 类型   | 语义                                    |
 *   |-----------------------|--------|-----------------------------------------|
 *   | PID_FT_RECV_PATH      | STRING | 接收目录 (文件名由对端 FILE_INFO 包给出)  |
 *   | PID_FT_SEND_PATH      | STRING | 发送完整路径 (含文件名)                   |
 *   | PID_FT_RECV_START     | EXEC   | 启动接收                                 |
 *   | PID_FT_SEND_START     | EXEC   | 启动发送                                 |
 *
 * @par 三条强制语义
 *  1. **EXEC 是"发起"不是"完成"**: 返回 PARAM_OK 只表示握手已布置, 传输要
 *     几秒到几分钟, 完成/失败由 file_transfer.c 的内部状态机收尾。
 *  2. **路径在 EXEC 时才读**: 不在 init 回调里读, 彻底规避 param_load_all()
 *     与模块 init 的先后问题。
 *  3. **重复触发是安全的**: ymodem_*_start() 内部整块复位协议状态, 因此不做
 *     "已在会话中"拒绝 —— 接收器在对端静默消失时会停在 TRANSFERRING/FINISHING,
 *     若拒绝重启, 设备就再也收不了文件。
 *
 * @par 校验位置
 * 框架对 STRING 是"回调先行 + 通过后才提交缓存"(param_manager.c
 * param_entry_write_core 分支 3): 在 ft_write_cb 里返回非 PARAM_OK 时
 * **缓存零污染**。所以路径校验只能、也必须放在这里。
 *
 * @par 不提供的能力 (刻意)
 * 会话状态查询 / 终止会话 / 剩余字节 —— 属于 ymodem 协议处理内容, 不在此。
 */

#include "file_transfer.h"
#include "app_param_manager.h"
#include "storage_config.h"

#include <string.h>

#ifndef FT_LOG_ENABLE
#define FT_LOG_ENABLE 1
#endif

#if FT_LOG_ENABLE
#include <stdio.h>
#define FT_LOG(...) printf(__VA_ARGS__)
#else
#define FT_LOG(...) ((void)0)
#endif

/* ================================================================
 *  路径缓冲区
 *
 *  PARAM_APP_STRING 要求 _def 是可写静态字符数组 (长度 max_len + 1),
 *  严禁字符串字面量 (.rodata 写入会触发写保护故障)。
 *
 *  这个缓冲区就是路径的**唯一真相源**: 参数缓存指向它, 组件运行时用
 *  param_read_string() 读, 不再另存副本 (副本会与 param_reset_* /
 *  其他写入路径分叉)。
 *
 *  @note 单缓冲配置: cache 与 default_val 共用同一指针, param_reset_*
 *        无法恢复默认内容。要恢复默认请显式
 *        param_write_string(PID_FT_RECV_PATH, STORAGE_MNT_FILES)。
 * ================================================================ */
static char s_recv_path[FT_PATH_MAX + 1] = STORAGE_MNT_FILES;
static char s_send_path[FT_PATH_MAX + 1] = STORAGE_MNT_FILES "/upload.bin";

/* ================================================================
 *  参数表
 * ================================================================ */

PARAM_STRING(ft_recv_path, PID_FT_RECV_PATH,
             PARAM_FLAG_PERSIST, s_recv_path, FT_PATH_MAX);

PARAM_STRING(ft_send_path, PID_FT_SEND_PATH,
             PARAM_FLAG_PERSIST, s_send_path, FT_PATH_MAX);

/** 命令型参数: 无负载, 只能被 param_exec() / param_write_raw() 触发
 *  (param_write 对 EXEC 返回 PARAM_ERR_EXEC_ONLY)。 */
PARAM_EXEC(ft_recv_start, PID_FT_RECV_START);
PARAM_EXEC(ft_send_start, PID_FT_SEND_START);

PARAM_TABLE(ft_params,
            &ft_recv_path.base,
            &ft_send_path.base,
            &ft_recv_start.base,
            &ft_send_start.base);

/* ================================================================
 *  模块回调
 * ================================================================ */

/**
 * @brief 初始化 — 只做无副作用的同步
 *
 * @note **不在**这里校验路径: init 可能早于 param_load_all(), 读到的是
 *       编译期默认值而不是落盘值。校验已经在 write 回调完成。
 */
static int ft_module_init(void *ctx)
{
    (void)ctx;

    FT_LOG("[FT] param init: defaults recv_dir=\"%s\" send_path=\"%s\"\n",
           s_recv_path, s_send_path);
    return PARAM_OK;
}

/**
 * @brief 写回调 — 两条 STRING 的校验点
 *
 * @param value .ptr 指向调用者传入的字符串 (尚未截断、尚未提交缓存)
 * @return PARAM_OK 放行; PARAM_ERR_OUT_OF_RANGE 拒绝 (缓存不被污染)
 */
static int ft_write_cb(void *ctx, uint32_t param_id, param_value_t value)
{
    const char *path = (const char *)value.ptr;

    (void)ctx;

    if (!path)
        return PARAM_ERR_OUT_OF_RANGE;

    switch (param_id)
    {
    case PID_FT_RECV_PATH:
        if (strlen(path) > (size_t)FT_PATH_MAX)
            return PARAM_ERR_OUT_OF_RANGE; /* 拒绝, 不让框架静默截断 */
        if (file_transfer_check_path(path, true) != 0)
        {
            FT_LOG("[FT] write: bad recv dir \"%s\"\n", path);
            return PARAM_ERR_OUT_OF_RANGE;
        }
        return PARAM_OK;

    case PID_FT_SEND_PATH:
        if (strlen(path) > (size_t)FT_PATH_MAX)
            return PARAM_ERR_OUT_OF_RANGE;
        if (file_transfer_check_path(path, false) != 0)
        {
            FT_LOG("[FT] write: bad send path \"%s\"\n", path);
            return PARAM_ERR_OUT_OF_RANGE;
        }
        return PARAM_OK;

    default:
        return PARAM_OK;
    }
}

/**
 * @brief 命令回调 — 两条 EXEC 的落点
 *
 * 返回码直接透传组件的启动结果 (PARAM_OK / PARAM_ERR_OUT_OF_RANGE /
 * PARAM_ERR_NOT_SUPPORTED)。
 */
static int ft_exec_cb(void *ctx, uint32_t param_id, param_value_t arg)
{
    (void)ctx;
    (void)arg;

    switch (param_id)
    {
    case PID_FT_RECV_START:
        return file_transfer_start_recv();
    case PID_FT_SEND_START:
        return file_transfer_start_send();
    default:
        return PARAM_ERR_NOT_FOUND;
    }
}

/* ================================================================
 *  模块注册
 * ================================================================ */

PARAM_MODULE_DEFINE(file_transfer, MODULE_FILE_TRANSFER, "FileTransfer",
                    .init  = ft_module_init, /* init  — 同步打印当前配置 */
                    .write = ft_write_cb,    /* write — 路径校验 */
                    .exec  = ft_exec_cb);    /* exec  — 启动收/发 */

PARAM_MODULE_INIT(file_transfer, ft_params);
