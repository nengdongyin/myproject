/**
 * @file    vsc_loader.c
 * @brief   Bitstream Loader — instance create + system init.
 *
 * JSON parsing is done at build-time by vsc_prop_gen.py.
 * This file contains only the runtime init logic.
 */

#include "vsc_loader.h"
#include "vsc_driver_registry.h"
#include "vsc_feature.h"
#include "vsc_resolver.h"
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════
 *  Instance creation
 * ═══════════════════════════════════════════════════════════════════════ */

/**
 * @brief 从驱动模板创建 Entity 实例并应用参数覆盖
 * @details 创建流程：
 *          1. 清零 Entity 内存
 *          2. 从 driver->transform_template 拷贝变换描述符模板
 *          3. 遍历 overrides[]，对匹配的键收紧约束：
 *             - CROP 类型：可收紧 max_width / max_height（但不可放宽，
 *               放宽会返回 VSC_ERR_UNREACHABLE）
 *             - BINNING 类型：可收紧 factor_x / factor_y
 *          4. 调用 driver->ops.init() 初始化驱动私有上下文
 *          5. 将 ops 虚表赋值给 Entity（不拷贝，仅指针共享）
 * @param[in]  driver    已注册的驱动模板
 * @param[in]  base_addr 硬件基地址
 * @param[in]  overrides 约束覆盖值数组
 * @param[in]  num_over  覆盖值数量
 * @param[out] entity    创建的 Entity 实例
 * @return VSC_OK 或 VSC_ERR_UNREACHABLE（覆盖值放宽了约束）
 * @note overrides 只能收紧约束，不可放宽——这是设计约束，防止应用层
 *       通过覆盖值绕过硬件能力限制。
 */
int vsc_instance_create(const vsc_driver_t *driver,
                        uint32_t base_addr,
                        const vsc_override_t *overrides,
                        uint8_t num_over, vsc_entity_t *entity)
{
    memset(entity, 0, sizeof(*entity));
    /* name set by caller (vsc_system_init uses node id) */

    /* copy transform template, then apply overrides (tighten only) */
    if (driver->transform_template) {
        entity->transform_desc = *driver->transform_template;

        for (uint8_t i = 0; i < num_over; i++) {
            const char *key = overrides[i].key;
            uint32_t val   = overrides[i].value;
            vsc_fmt_transform_desc_t *td = &entity->transform_desc;

            if (td->type == VSC_TRANSFORM_CROP) {
                if (strcmp(key, "crop.max_width") == 0 || strcmp(key, "max_width") == 0) {
                    if (val > td->params.crop.max_w) return VSC_ERR_UNREACHABLE;
                    td->params.crop.max_w = val;
                } else if (strcmp(key, "crop.max_height") == 0 || strcmp(key, "max_height") == 0) {
                    if (val > td->params.crop.max_h) return VSC_ERR_UNREACHABLE;
                    td->params.crop.max_h = val;
                }
            } else if (td->type == VSC_TRANSFORM_BINNING) {
                if (strcmp(key, "binning.factor_x") == 0 || strcmp(key, "factor_x") == 0) {
                    if (val > td->params.binning.factor_x) return VSC_ERR_UNREACHABLE;
                    td->params.binning.factor_x = (uint8_t)val;
                } else if (strcmp(key, "binning.factor_y") == 0 || strcmp(key, "factor_y") == 0) {
                    if (val > td->params.binning.factor_y) return VSC_ERR_UNREACHABLE;
                    td->params.binning.factor_y = (uint8_t)val;
                }
            }
            /* future: SCALER, etc. */
        }
    }

    /* initialise driver context via ops->init() */
    if (driver->ops.init) {
        void *ctx = NULL;
        int rc = driver->ops.init(&ctx, base_addr, overrides, num_over);
        if (rc != VSC_OK) return rc;
        entity->drv_ctx = ctx;
    }

    entity->ops = &driver->ops;
    return VSC_OK;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  System init
 * ═══════════════════════════════════════════════════════════════════════ */

/**
 * @brief 在管线中按名称查找 Entity 索引
 * @param[in] p  管线指针
 * @param[in] id Entity 名称
 * @return Entity 在 entities[] 中的索引，未找到返回 0xFF
 */
static uint8_t find_by_id(vsc_pipeline_t *p, const char *id) {
    for (uint8_t i = 0; i < p->num_entities; i++)
        if (strcmp(p->entities[i].name, id) == 0) return i;
    return 0xFF;
}

/**
 * @brief 从系统描述符和板级配置构建完整管线
 * @details 按以下 6 个步骤完成系统初始化：
 *          **Step 1 — 创建 SENSOR Entity**
 *          根据 board->sensor_type 查找对应的传感器驱动，
 *          传入 I2C 总线号和地址作为覆盖参数。传感器 Entity 总是
 *          排在 entities[] 的首位。
 *          **Step 2 — 创建 FPGA 节点 Entity**
 *          遍历 desc->nodes[]，为每个 FPGA IP 核调用
 *          vsc_instance_create()。如果驱动未找到且节点标记为 optional，
 *          则跳过；否则返回 VSC_ERR_CANNOT_AUTO_BRIDGE。
 *          带 STATISTICS capability 的节点自动归类为 ANALYZER，
 *          其余为 STREAM。
 *          **Step 3 — 传感器到首节点的隐式连接**
 *          如果存在传感器和至少一个 FPGA 节点，自动创建一条
 *          sensor → first_fpga 的 STREAM 链接。
 *          **Step 4 — 显式链接**
 *          根据 desc->links[] 创建用户声明的链接（STREAM/TAP）。
 *          通过 find_by_id() 将字符串 ID 解析为 Entity 索引。
 *          **Step 5 — 拓扑构建**
 *          调用 vsc_pipeline_build() 进行拓扑排序、邻接表构建
 *          和端点/TAP 收集。
 *          **Step 6 — 特性推导**
 *          调用 vsc_feature_derive() 根据已注册驱动推导特性可用性。
 * @param[in]  desc     解析后的系统描述符（来自 system_graph.json）
 * @param[in]  board    解析后的板级配置（来自 board.json）
 * @param[out] pipeline 构建完成的管线
 * @return VSC_OK 或负值错误码
 */
int vsc_system_init(const vsc_system_desc_t *desc,
                    const vsc_board_config_t *board,
                    vsc_pipeline_t *pipeline)
{
    memset(pipeline, 0, sizeof(*pipeline));

    /* Step 1: create SENSOR entity from board config */
    uint8_t sensor_idx = 0xFF;
    if (board->sensor_type[0] != '\0') {
        const vsc_driver_t *sdrv = vsc_driver_find(board->sensor_type);
        if (sdrv && (sdrv->capabilities & VSC_CAP_SENSOR)) {
            if (pipeline->num_entities >= VSC_MAX_ENTITIES)
                return VSC_ERR_TOPOLOGY_BROKEN;
            vsc_entity_t *sent = &pipeline->entities[pipeline->num_entities];
            /* sensors have no base_addr (external chip), use I2C address */
            vsc_override_t i2c_ov[2];
            uint8_t i2c_count = 0;
            i2c_ov[i2c_count].key[0] = '\0';
            strncpy(i2c_ov[i2c_count].key, "i2c_bus", 63);
            i2c_ov[i2c_count].value = board->i2c_bus; i2c_count++;
            i2c_ov[i2c_count].key[0] = '\0';
            strncpy(i2c_ov[i2c_count].key, "i2c_addr", 63);
            i2c_ov[i2c_count].value = board->i2c_addr; i2c_count++;
            int rc = vsc_instance_create(sdrv, 0, i2c_ov, i2c_count, sent);
            if (rc != VSC_OK) return rc;
            strncpy(sent->name, board->sensor_type, VSC_MAX_ENTITY_NAME - 1);
            sent->entity_class = VSC_ENTITY_SENSOR;
            sensor_idx = pipeline->num_entities;
            pipeline->num_entities++;
        }
    }

    /* Step 2: create entities from FPGA nodes */
    for (uint8_t i = 0; i < desc->num_nodes; i++) {
        const vsc_node_desc_t *nd = &desc->nodes[i];
        const vsc_driver_t *drv = vsc_driver_find(nd->type);
        if (!drv) {
            if (nd->optional) continue;
            return VSC_ERR_CANNOT_AUTO_BRIDGE;
        }
        if (pipeline->num_entities >= VSC_MAX_ENTITIES) return VSC_ERR_TOPOLOGY_BROKEN;
        vsc_entity_t *ent = &pipeline->entities[pipeline->num_entities];
        int rc = vsc_instance_create(drv, nd->base_addr,
                                     nd->overrides, nd->num_overrides, ent);
        if (rc != VSC_OK) return rc;
        strncpy(ent->name, nd->id, VSC_MAX_ENTITY_NAME - 1);
        if (drv->capabilities & VSC_CAP_STATISTICS) ent->entity_class = VSC_ENTITY_ANALYZER;
        else ent->entity_class = VSC_ENTITY_STREAM;
        pipeline->num_entities++;
    }

    /* Step 3: sensor → first FPGA node link (implicit) */
    if (sensor_idx != 0xFF && desc->num_nodes > 0) {
        uint8_t first_fpga = sensor_idx + 1;  /* sensor is always first */
        if (first_fpga < pipeline->num_entities &&
            pipeline->num_links < VSC_MAX_LINKS) {
            pipeline->links[pipeline->num_links].src_entity = sensor_idx;
            pipeline->links[pipeline->num_links].dst_entity = first_fpga;
            pipeline->links[pipeline->num_links].type       = VSC_LINK_STREAM;
            pipeline->num_links++;
        }
    }

    /* Step 4: explicit links from system_graph.json */
    for (uint8_t i = 0; i < desc->num_links; i++) {
        const vsc_link_desc_t *ld = &desc->links[i];
        uint8_t src = find_by_id(pipeline, ld->src_id);
        uint8_t dst = find_by_id(pipeline, ld->dst_id);
        if (src == 0xFF || dst == 0xFF) continue;
        if (pipeline->num_links >= VSC_MAX_LINKS) return VSC_ERR_TOPOLOGY_BROKEN;
        pipeline->links[pipeline->num_links].src_entity = src;
        pipeline->links[pipeline->num_links].dst_entity = dst;
        pipeline->links[pipeline->num_links].type = ld->type;
        pipeline->num_links++;
    }

    /* Step 5: topology */
    int rc = vsc_pipeline_build(pipeline);
    if (rc != VSC_OK) return rc;

    /* Step 6: features (dump only when explicitly requested) */
    vsc_feature_derive();
    return VSC_OK;
}
