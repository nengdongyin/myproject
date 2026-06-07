/* ═══════════════════════════════════════════════════════════
 *  GENERATED FILE — DO NOT EDIT
 *
 *  Compiler:  vsc_prop_gen.py  v1.0.0
 *  File:      vsc_dependency_map.c
 *  Checksum:  0x21B0C029
 *  Generated: 2026-06-07T10:23:13Z
 * ═══════════════════════════════════════════════════════════ */

#include "vsc_types.h"
#include "vsc_prop_ids.h"

/* topologically-sorted dependency map */
static const vsc_prop_dep_t _global_dependencies[] = {
    { VSC_PROP_HISTOGRAM_ACTIVE_BINS, VSC_PROP_HISTOGRAM_MAX_BINS },
    { VSC_PROP_CROP_ROI_Y, VSC_PROP_CROP_MAX_HEIGHT },
    { VSC_PROP_CROP_ROI_HEIGHT, VSC_PROP_CROP_MAX_HEIGHT },
    { VSC_PROP_CROP_ROI_WIDTH, VSC_PROP_CROP_MAX_WIDTH },
    { VSC_PROP_CROP_ROI_X, VSC_PROP_CROP_MAX_WIDTH },
};
const uint8_t _global_dep_count = 5;
