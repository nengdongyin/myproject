#ifndef PARAM_EVENT_LWEVT_H
#define PARAM_EVENT_LWEVT_H

/**
 * @file param_event_lwevt.h
 * @brief 参数事件与 lwevt 事件系统的桥接层
 *
 * @details
 * 本文件提供 param_manager 参数变化事件（param_notify_fn 回调）到
 * lwevt 轻量级事件总线的转换工具函数。
 *
 * 典型用法: 在 param_notify_fn 回调中用 param_event_get_value 提取
 * 参数新值，再封装为 lwevt_t 通过 lwevt_dispatch_ex 分发到订阅者。
 */

#include "param_manager.h"
#include "lwevt/lwevt.h"
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 从 lwevt 事件中提取参数变化的新值
 *
 * 将 evt->msg.param_changed.new_value 的原始字节逐位拷贝为
 * param_value_t 联合体，避免类型双关 (type-punning) 带来的
 * 严格别名违规 (strict aliasing violation)。
 *
 * @param evt lwevt 事件指针，类型必须为 PARAM_EVT_CHANGED
 * @return 参数写入后的新值（best-effort 快照）
 */
static inline param_value_t param_event_get_value(const lwevt_t *evt)
{
    param_value_t v;
    memcpy(&v, &evt->msg.param_changed.new_value, sizeof(v));
    return v;
}

#ifdef __cplusplus
}
#endif

#endif
