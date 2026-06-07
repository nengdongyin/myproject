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
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════
 *  Instance creation
 * ═══════════════════════════════════════════════════════════════════════ */

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

static uint8_t find_by_id(vsc_pipeline_t *p, const char *id) {
    for (uint8_t i = 0; i < p->num_entities; i++)
        if (strcmp(p->entities[i].name, id) == 0) return i;
    return 0xFF;
}

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
