/**
 * @file    isc_internal.h
 * @brief   ISC 模块内部定义 (仅 isc_core.c 及传感器驱动 .c 引用)
 *
 * 包含 isc_dev_t 完整结构体、状态枚举、控制缓存、时序缓存。
 */

#ifndef ISC_INTERNAL_H
#define ISC_INTERNAL_H

#include "isc.h"

/* ──── 设备状态枚举 ──── */
typedef enum {
    ISC_STATE_INIT       = 0,   /**< 全局初始化完成, 无活动设备                    */
    ISC_STATE_OPEN       = 1,   /**< 传感器已识别、可配置                           */
    ISC_STATE_STREAMING  = 2,   /**< 传感器正在输出 LVDS 数据                       */
} isc_state_t;

/* ──── 控制项缓存 ──── */
typedef struct isc_ctrl_cache {
    uint32_t          cid;        /**< 控制 ID                                    */
    isc_ctrl_value_t  value;      /**< 缓存值                                      */
    uint32_t          flags;      /**< 缓存标志 (含 ISC_CTRL_FLAG_VOLATILE 副本)    */
} isc_ctrl_cache_t;

/* ──── 传感器设备实例 ──── */
struct isc_dev_t {
    /* 身份 */
    const isc_sensor_ops_t *ops;            /**< 传感器驱动 ops 表                  */
    uint8_t                 sensor_idx;     /**< 在全局传感器表中的索引              */
    isc_state_t             state;          /**< 当前状态                           */

    /* 平台接口 (从 isc_init 继承) */
    const isc_port_t       *port;           /**< 传感器通信原语                      */
    const isc_fpga_ops_t   *fpga_ops;       /**< FPGA 同步接口                       */

    /* 格式 */
    isc_fmt_t               current_fmt;    /**< 当前生效格式 (含 crop)              */

    /* 控制 */
    isc_ctrl_cache_t        ctrl_cache[ISC_MAX_CTRLS]; /**< 控制值缓存               */
    uint8_t                 num_ctrls;      /**< 实际控制项数量                      */
    uint32_t                last_ctrl_cid;  /**< NEXT_CTRL 枚举迭代状态              */

    /* 时序 */
    isc_timing_t            cached_timing;  /**< 最近一次 query_timing 快照          */
    uint8_t                 timing_valid;   /**< 缓存是否有效 (0=无效, 1=有效)       */

    /* 回调 */
    isc_on_ctrl_change_t    on_ctrl_change; /**< 控制变更通知 (可选)                 */
    void                   *cb_user_data;   /**< 回调用户数据                        */
    isc_on_error_t          on_error;       /**< 异常通知 (可选)                     */
    void                   *err_user_data;  /**< 异常回调用户数据                     */
};

/* ═══════════════════════════════════════════════════════════════════════════
 * 注: 全局状态 (g_*) 仅在 isc_core.c 中定义为 static 变量。
 * 传感器驱动通过 dev->port / dev->fpga_ops 访问平台接口，
 * 不可直接访问全局设备表。
 * ═══════════════════════════════════════════════════════════════════════════ */

#endif /* ISC_INTERNAL_H */
