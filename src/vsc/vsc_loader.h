/**
 * @file    vsc_loader.h
 * @brief   Bitstream Loader — JSON → Pipeline 自动构建。
 *
 * 读取 system_graph.json（FPGA 比特流生成的拓扑描述）和
 * board.json（PCB 级硬件配置），调用 P0/P1/P2 子系统自动构建
 * 完整的 vsc_pipeline_t。
 */

#ifndef VSC_LOADER_H
#define VSC_LOADER_H

#include <stdint.h>
#include <stdbool.h>
#include "vsc_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 *  System descriptor（解析 system_graph.json 的结果）
 * ======================================================================== */

#define VSC_MAX_OVERRIDES_PER_NODE  8

/* vsc_override_t is defined in vsc_types.h (used by vsc_ip_ops_t.init()) */

typedef struct {
    char          type[32];       /* driver name, e.g. "crop"           */
    char          id[32];         /* instance id, e.g. "crop_0"        */
    uint32_t      base_addr;      /* hardware base address              */
    bool          optional;       /* skip if driver not found           */
    uint8_t       num_overrides;
    vsc_override_t overrides[VSC_MAX_OVERRIDES_PER_NODE];
} vsc_node_desc_t;

typedef struct {
    char           src_id[32];
    char           dst_id[32];
    vsc_link_type_t type;         /* VSC_LINK_STREAM or VSC_LINK_TAP   */
} vsc_link_desc_t;

typedef struct {
    uint8_t          num_nodes;
    vsc_node_desc_t  nodes[VSC_MAX_ENTITIES];
    uint8_t          num_links;
    vsc_link_desc_t  links[VSC_MAX_LINKS];
} vsc_system_desc_t;

/* ========================================================================
 *  Board config（解析 board.json 的结果）
 * ======================================================================== */

typedef struct {
    char     sensor_type[32];     /* "sensor_imx477"                    */
    uint8_t  i2c_bus;
    uint8_t  i2c_addr;
    /* future: trigger_pin, cl_mode, clock_hz, ... */
} vsc_board_config_t;

/* ========================================================================
 *  API
 * ======================================================================== */

/**
 * @brief 从描述符构建完整 Pipeline。
 *
 * 流程：
 *   1. 遍历 nodes[] → vsc_driver_find(type) → vsc_instance_create()
 *   2. 创建 SENSOR Entity（来自 Board Config）
 *   3. 遍历 links[] → 创建 Pipeline Link
 *   4. vsc_pipeline_build() → 拓扑排序 + 自动桥接
 *   5. vsc_feature_derive()
 *
 * @param desc     解析后的 system_graph.json
 * @param board    解析后的 board.json
 * @param pipeline [out] 构建好的管线
 * @return         VSC_OK 或错误码
 */
int vsc_system_init(const vsc_system_desc_t *desc,
                    const vsc_board_config_t *board,
                    vsc_pipeline_t *pipeline);

/**
 * @brief Build-time generated init (produced by vsc_prop_gen.py from
 *        system_graph.json + board.json).
 *
 * Calls vsc_system_init() with pre-initialized descriptor data.
 */
int vsc_system_init_default(vsc_pipeline_t *pipeline);

/**
 * @brief 从 Driver 创建 Instance 并应用 overrides。
 *
 * @param driver     已注册的 Driver
 * @param overrides  收紧约束的覆盖值数组
 * @param num_over   覆盖值数量
 * @param instance   [out] 创建的 Instance（entity 从 Instance 拷贝数据）
 * @return           VSC_OK 或错误码（override 放宽了约束时拒绝）
 */
int vsc_instance_create(const vsc_driver_t *driver,
                        uint32_t base_addr,
                        const vsc_override_t *overrides,
                        uint8_t num_over,
                        vsc_entity_t *entity);

#ifdef __cplusplus
}
#endif

#endif /* VSC_LOADER_H */
