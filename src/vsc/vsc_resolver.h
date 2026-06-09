/**
 * @file    vsc_resolver.h
 * @brief   VSC Property Resolver + Pipeline Graph — 公开 API 声明
 * @details 本头文件声明格式协商引擎（Phase 1/2/3/4）和管线拓扑管理
 *          （构建、可选实体移除、硬件提交）的全部公开函数。
 *          所有函数的实现位于 vsc_resolver.c。
 *
 *          架构分层：
 *          - Pipeline Graph（拓扑管理）：vsc_pipeline_build /
 *            vsc_pipeline_remove_optional / vsc_pipeline_commit_fmt
 *          - Property Resolver（格式协商）：vsc_resolver_try_fmt /
 *            vsc_resolver_feasibility_check / vsc_resolver_forward_propagate /
 *            vsc_resolver_converge
 */

#ifndef VSC_RESOLVER_H
#define VSC_RESOLVER_H

#include "vsc_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 *  Pipeline Graph API — 拓扑管理
 * ======================================================================== */

/**
 * @brief 构建管线拓扑：拓扑排序、邻接表、端点/TAP 收集
 * @param[in,out] pipeline 包含 entities[] 和 links[] 的管线
 * @return VSC_OK 或 VSC_ERR_TOPOLOGY_BROKEN（存在环或断连）
 * @details 采用 **Kahn 算法**（基于入度的拓扑排序）处理 STREAM 链路，
 *          同时收集 TAP 观察者和端点列表，预构建邻接矩阵供 BFS 使用。
 *          完成后将管线状态设为 UNCONFIGURED。
 * @note 时间复杂度 O(V + E)，空间复杂度 O(V²)
 * @warning 调用前必须确保 entities[] 和 links[] 已填充完毕
 */
int vsc_pipeline_build(vsc_pipeline_t *pipeline);

/**
 * @brief 移除可选 Entity 并尝试自动桥接
 * @param[in,out] pipeline   管线
 * @param[in]     entity_idx 要移除的 Entity 索引
 * @return VSC_OK、VSC_ERR_CANNOT_AUTO_BRIDGE 或 VSC_ERR_TOPOLOGY_BROKEN
 * @details 自动桥接策略处理三种拓扑情况：
 *          - **SISO（单入单出）**：创建直接桥接链路替代被移除节点
 *          - **MIMO（多入或多出）**：无法自动桥接，返回错误
 *          - **叶节点/根节点**：直接移除，无需创建新链路
 *          操作完成后重新调用 vsc_pipeline_build() 重建拓扑。
 * @warning 会修改 entities[] 和 links[] 的内容和数量，
 *          外部持有的 Entity 索引在调用后失效
 */
int vsc_pipeline_remove_optional(vsc_pipeline_t *pipeline, uint8_t entity_idx);

/**
 * @brief Phase 4 提交：将最终协商格式写入各实体的硬件寄存器
 * @param[in,out] pipeline  管线
 * @param[in]     final_fmt 协商确定的最终格式
 * @return VSC_OK 或 VSC_ERR_BUSY / VSC_ERR_COMMIT_FAILED
 * @details 沿 execution_order 遍历，对每个有 commit_fmt 回调的实体调用之。
 *          STREAMING 状态下拒绝提交；部分失败不回滚，管线标记为 DIRTY。
 * @warning 部分提交失败不回滚——调用者应在重试前重建管线
 */
int vsc_pipeline_commit_fmt(vsc_pipeline_t *pipeline,
                            const vsc_mbus_fmt_t *final_fmt);

/* ========================================================================
 *  Property Resolver API — 格式协商引擎
 * ======================================================================== */

/**
 * @brief 完整格式协商管线：Phase 1（可行性）→ Phase 2（正向传播）→ Phase 3（收敛）
 * @param[in]  pipeline 已构建的管线
 * @param[in]  intent   用户期望的输出格式
 * @param[out] result   协商结果（含状态、主格式、端点格式、轨迹）
 * @return VSC_OK 或负值错误码
 * @details 三阶段流水线：
 *          1. 意图校验（width/height/pixel_format 合法性）
 *          2. Phase 1 — 可行性逆传播检查
 *          3. Phase 2 — BFS 正向传播
 *          4. Phase 3 — 收敛：收集端点格式、构建轨迹、判定状态
 * @note 本函数不执行硬件提交，需显式调用 vsc_pipeline_commit_fmt()
 */
int vsc_resolver_try_fmt(vsc_pipeline_t *pipeline,
                         const vsc_mbus_fmt_t *intent,
                         vsc_resolver_result_t *result);

/**
 * @brief Phase 1 可行性检查：从输出端点逆向传播约束到传感器
 * @param[in]  pipeline 已构建的管线（需已完成拓扑排序）
 * @param[in]  intent   用户期望的输出格式
 * @param[out] result   协商结果（失败时 reachable_max 会填充可达上限）
 * @return VSC_OK 表示可行性通过；VSC_ERR_UNREACHABLE 表示不可达
 * @details 支持逐端点检查和线性回退两条路径。
 *          从端点沿管线逆序遍历，逐实体执行逆变换，
 *          最终检查传感器能力是否覆盖累积约束。
 * @note 不修改管线状态（const pipeline），可安全并发调用
 */
int vsc_resolver_feasibility_check(const vsc_pipeline_t *pipeline,
                                   const vsc_mbus_fmt_t *intent,
                                   vsc_resolver_result_t *result);

/**
 * @brief Phase 2 正向传播：从传感器沿 STREAM 链路 BFS 传播格式
 * @param[in,out] pipeline 管线（会修改每个 Entity 的 prop_state）
 * @param[in]     intent   用户期望的输出格式
 * @return VSC_OK 或 VSC_ERR_NO_SENSOR / VSC_ERR_TOPOLOGY_BROKEN
 * @details 采用 **BFS 算法**：以 SENSOR 为根节点，
 *          沿 STREAM 链路逐级调用 try_fmt_sink / try_fmt_source，
 *          TAP 链路仅做被动验证不入队。
 * @note 每次调用 vsc_resolver_try_fmt() 都会重新执行
 */
int vsc_resolver_forward_propagate(vsc_pipeline_t *pipeline,
                                   const vsc_mbus_fmt_t *intent);

/**
 * @brief Phase 3 收敛：收集端点格式、构建调整轨迹、判定协商状态
 * @param[in]  pipeline 已完成正向传播的管线
 * @param[in]  intent   用户原始意图格式
 * @param[out] result   协商结果（填充 primary_fmt、endpoint_fmts、trace、status）
 * @return VSC_OK 或 VSC_ERR_NO_ACTIVE_ENDPOINT
 * @details 收集所有活跃端点的最终格式，沿执行顺序构建调整轨迹，
 *          根据 primary_fmt 与 intent 的匹配度判定状态：
 *          EXACT / ADJUSTED / PARTIAL（TAP 被拒绝时降级）。
 * @see vsc_resolver_forward_propagate() 必须先执行
 */
int vsc_resolver_converge(vsc_pipeline_t *pipeline,
                          const vsc_mbus_fmt_t *intent,
                          vsc_resolver_result_t *result);

#ifdef __cplusplus
}
#endif

#endif /* VSC_RESOLVER_H */
