/**
 * @file vsc_lite.h
 * @brief VSC Lite — 简化版视频管线求解器
 *
 * 与完整 VSC 相同的 API 语义，但:
 *   - 线性管线拓扑（不做图遍历），最多 VSC_LITE_MAX_STAGES 阶段
 *   - 仅前向传播（不反向、不收敛）
 *   - 无 schema 元数据、无调整追踪
 *   - 驱动层复用 vsc_ip_ops_t 接口
 *   - 实例由应用层编译期静态分配，同一 driver 可多实例
 *
 * 用法:
 * @code
 *   static binning_vsc_inst_t g_bin0 = { .hw = { .base_addr = 0x43C10000, .factor_x = 2, .factor_y = 2 } };
 *   static crop_vsc_inst_t    g_crop0 = { .hw = { .base_addr = 0x43C00000 } };
 *
 *   const vsc_lite_stage_def_t stages[] = {
 *       { &sensor_imx477_vsc_driver, &g_sensor0 },
 *       { &crop_vsc_driver,           &g_crop0   },
 *       { &binning_vsc_driver,        &g_bin0    },
 *   };
 *   vsc_lite_pipeline_t pipe;
 *   vsc_lite_pipeline_init(&pipe, stages, 3);
 *   vsc_lite_try_fmt(&pipe, &intent, &result);
 *   vsc_lite_commit_fmt(&pipe, &result.primary_fmt);
 * @endcode
 */

#ifndef VSC_LITE_H
#define VSC_LITE_H

#include "vsc_core_types.h"
#include "vsc_ctrl_ids.h"

#define VSC_LITE_MAX_STAGES 6

/** @brief Stage 配置定义 — 编译期静态分配，driver + 实例绑定 */
typedef struct {
    const vsc_driver_t *driver;    /* 驱动类描述符（共享 ROM）       */
    void               *inst;      /* 实例内存（编译期静态分配）      */
} vsc_lite_stage_def_t;

typedef struct {
    const vsc_driver_t *driver;
    void               *drv_ctx;
    vsc_mbus_fmt_t      sink_fmt;
    vsc_mbus_fmt_t      source_fmt;
} vsc_lite_stage_t;

typedef struct {
    vsc_lite_stage_t stages[VSC_LITE_MAX_STAGES];
    uint8_t          count;
} vsc_lite_pipeline_t;

/**
 * @brief 初始化管线，按顺序注册驱动与实例
 * @param pipe      管线实例
 * @param stages    stage 配置数组（driver + inst 对）
 * @param count     stage 数量
 * @return VSC_OK 成功
 */
int vsc_lite_pipeline_init(vsc_lite_pipeline_t *pipe,
                           const vsc_lite_stage_def_t *stages, uint8_t count);

/**
 * @brief 格式协商 — 从 sensor 到端点逐级前向传播
 * @param pipe   已初始化的管线
 * @param intent 用户意图格式
 * @param result 协商结果
 * @return VSC_OK 成功
 */
int vsc_lite_try_fmt(vsc_lite_pipeline_t *pipe,
                     const vsc_mbus_fmt_t *intent,
                     vsc_resolver_result_t *result);

/**
 * @brief 提交最终格式到硬件
 * @param pipe      已初始化的管线
 * @param final_fmt 最终格式
 * @return VSC_OK 成功
 */
int vsc_lite_commit_fmt(vsc_lite_pipeline_t *pipe,
                        const vsc_mbus_fmt_t *final_fmt);

/**
 * @brief 按 pipeline 顺序查询能力，返回第一个可用提供者
 * @param pipe     已初始化的管线
 * @param cap_id   能力 ID（VSC_CAP_BINNING / VSC_CAP_CROP / …）
 * @param out      能力描述符缓冲区（类型取决于 cap_id）
 * @param out_len  [in/out] 输入=缓冲区大小，输出=实际写入字节数
 * @return VSC_OK 成功（out 已填充且 available==true）；
 *         VSC_ERR_NOT_SUPPORTED 所有 stage 均不提供；
 *         VSC_ERR_PARAM 无效参数
 */
int vsc_lite_query_cap(vsc_lite_pipeline_t *pipe, uint32_t cap_id,
                       void *out, uint8_t *out_len);

/**
 * @brief 设置控制值 — 自动路由到提供该能力的 stage
 * @param pipe    已初始化的管线
 * @param cap_id  能力 ID（用于定位目标 stage）
 * @param ctrl_id 控制 ID（VSC_CTRL_*）
 * @param value   目标值
 * @return VSC_OK 成功；VSC_ERR_NOT_SUPPORTED 所有 stage 均不识别
 */
int vsc_lite_set_ctrl(vsc_lite_pipeline_t *pipe, uint32_t cap_id,
                      uint32_t ctrl_id, uint32_t value);

/**
 * @brief 读取控制值 — 从提供该能力的 stage 读取
 * @param pipe    已初始化的管线
 * @param cap_id  能力 ID
 * @param ctrl_id 控制 ID（VSC_CTRL_*）
 * @param value  [out] 当前值
 * @return VSC_OK 成功；VSC_ERR_NOT_SUPPORTED 所有 stage 均不识别
 */
int vsc_lite_get_ctrl(vsc_lite_pipeline_t *pipe, uint32_t cap_id,
                      uint32_t ctrl_id, uint32_t *value);

#endif /* VSC_LITE_H */
