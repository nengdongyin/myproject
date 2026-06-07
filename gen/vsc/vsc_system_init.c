/* ═══════════════════════════════════════════════════════════
 *  GENERATED FILE — DO NOT EDIT
 *
 *  Compiler:  vsc_prop_gen.py  v1.0.0
 *  File:      vsc_system_init.c
 *  Checksum:  0x21B0C029
 *  Generated: 2026-06-07T10:23:13Z
 * ═══════════════════════════════════════════════════════════ */

#include "vsc_loader.h"

/* ═══════════ system_graph.json ═══════════ */
static const vsc_system_desc_t _vsc_system_desc = {
    .num_nodes = 4,
    .nodes = {
        {
            .type       = "crop",
            .id         = "crop_0",
            .base_addr  = 0x43C00000,
            .optional   = false,
            .num_overrides = 0,
            /* overrides */
        },
        {
            .type       = "binning",
            .id         = "binning_0",
            .base_addr  = 0x43C10000,
            .optional   = false,
            .num_overrides = 0,
            /* overrides */
        },
        {
            .type       = "decoder",
            .id         = "decoder_0",
            .base_addr  = 0x43C20000,
            .optional   = false,
            .num_overrides = 0,
            /* overrides */
        },
        {
            .type       = "histogram",
            .id         = "histogram_0",
            .base_addr  = 0x43C30000,
            .optional   = false,
            .num_overrides = 0,
            /* overrides */
        },
    },
    .num_links = 3,
    .links = {
        { "crop_0", "binning_0", VSC_LINK_STREAM },
        { "binning_0", "decoder_0", VSC_LINK_STREAM },
        { "binning_0", "histogram_0", VSC_LINK_TAP },
    },
};

/* ═══════════ board.json ═══════════ */
static const vsc_board_config_t _vsc_board_config = {
    .sensor_type = "sensor_imx477",
    .i2c_bus     = 0,
    .i2c_addr    = 26,
};

int vsc_system_init_default(vsc_pipeline_t *pipeline)
{
    return vsc_system_init(&_vsc_system_desc,
                           &_vsc_board_config, pipeline);
}