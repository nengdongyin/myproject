/**
 * @file    vsc_feature.h
 * @brief   VSC Feature System — capability query API for application portability.
 *
 * Features are derived from the Driver registry and Property schema at init
 * time.  Applications query features with vsc_has_feature() instead of
 * checking hardware-specific capabilities directly.
 *
 *   if (vsc_has_feature(VSC_FEATURE_AUTO_EXPOSURE))
 *       ae_init();
 *
 * The same application code works regardless of whether the underlying
 * statistics source is a Histogram IP, an AE Statistics Engine, or ROI stats.
 */

#ifndef VSC_FEATURE_H
#define VSC_FEATURE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 *  Feature IDs
 * ======================================================================== */

typedef enum {
    VSC_FEATURE_STREAMING          = 0,   /* always true if pipeline built   */
    VSC_FEATURE_AUTO_EXPOSURE      = 1,   /* AE: statistics source + exposure ctrl */
    VSC_FEATURE_AUTO_WHITE_BALANCE = 2,   /* AWB: statistics source + WB ctrl     */
    VSC_FEATURE_HDR                = 3,   /* HDR-capable sensor                    */
    VSC_FEATURE_TRIGGER            = 4,   /* external trigger support              */
    VSC_FEATURE_CROP               = 5,   /* crop / ROI IP present                 */
    VSC_FEATURE_BINNING            = 6,   /* binning IP present                    */
    VSC_FEATURE_HISTOGRAM          = 7,   /* histogram statistics available         */

    VSC_FEATURE_COUNT
} vsc_feature_id_t;

#define VSC_MAX_FEATURES  16   /* reserve headroom */

/* ========================================================================
 *  Feature state (per-feature)
 * ======================================================================== */

typedef struct {
    const char *name;
    const char *description;
    bool        available;
} vsc_feature_t;

/* ========================================================================
 *  API
 * ======================================================================== */

/**
 * @brief Derive all features from the current Driver registry.
 *
 * Must be called once after vsc_system_init().
 * Examines registered drivers, their properties, and transforms.
 */
void vsc_feature_derive(void);

/**
 * @brief Query whether a feature is available.
 *
 * @return true if the feature was derived as available.
 */
bool vsc_has_feature(vsc_feature_id_t feature);

/**
 * @brief Get the full feature descriptor (name + description + available).
 */
const vsc_feature_t *vsc_feature_get(vsc_feature_id_t feature);

/**
 * @brief Print all features and their availability (debug / CLI).
 */
void vsc_feature_dump(void);

/* ── test / debug helpers ── */
void vsc_feature_set(vsc_feature_id_t feature, bool available);
bool vsc_feature_is_derived(void);
void vsc_feature_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* VSC_FEATURE_H */
