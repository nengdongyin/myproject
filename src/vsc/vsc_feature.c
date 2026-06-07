/**
 * @file    vsc_feature.c
 * @brief   Feature derivation + query implementation.
 *
 * Features are derived from:
 *   1. Which Drivers are registered (vsc_driver_find)
 *   2. What Properties exist on those Drivers
 *   3. What Transform capabilities the Drivers declare
 */

#include "vsc_feature.h"
#include "vsc_driver_registry.h"
#include <string.h>
#include <stdio.h>

/* ═══════════════════════════════════════════════════════════════════════
 *  Feature registry
 * ═══════════════════════════════════════════════════════════════════════ */

static vsc_feature_t g_features[VSC_FEATURE_COUNT] = {
    [VSC_FEATURE_STREAMING]          = {"streaming",           "Video streaming output", false},
    [VSC_FEATURE_AUTO_EXPOSURE]      = {"auto_exposure",       "Automatic exposure control", false},
    [VSC_FEATURE_AUTO_WHITE_BALANCE] = {"auto_white_balance",  "Automatic white balance", false},
    [VSC_FEATURE_HDR]                = {"hdr",                 "HDR mode support", false},
    [VSC_FEATURE_TRIGGER]            = {"trigger",             "External trigger input", false},
    [VSC_FEATURE_CROP]               = {"crop",                "Crop / ROI processing", false},
    [VSC_FEATURE_BINNING]            = {"binning",             "Binning / downscaling", false},
    [VSC_FEATURE_HISTOGRAM]          = {"histogram",           "Histogram statistics", false},
};

static bool g_derived = false;

/* ═══════════════════════════════════════════════════════════════════════
 *  Auto-derivation (from registered drivers)
 * ═══════════════════════════════════════════════════════════════════════ */

void vsc_feature_derive(void)
{
    /* ── scan all registered drivers for capabilities ── */
    bool has_sensor      = false;
    bool has_exposure    = false;
    bool has_statistics  = false;
    bool has_hdr         = false;
    bool has_trigger     = false;
    bool has_crop        = false;
    bool has_binning     = false;

    for (int i = 0; ; i++) {
        const vsc_driver_t *drv = vsc_driver_by_index(i);
        if (!drv) break;
        uint32_t caps = drv->capabilities;

        if (caps & VSC_CAP_SENSOR)        has_sensor     = true;
        if (caps & VSC_CAP_EXPOSURE_CTRL) has_exposure   = true;
        if (caps & VSC_CAP_STATISTICS)    has_statistics = true;
        if (caps & VSC_CAP_HDR)           has_hdr        = true;
        if (caps & VSC_CAP_TRIGGER)       has_trigger    = true;
        if (caps & VSC_CAP_CROP)          has_crop       = true;
        if (caps & VSC_CAP_BINNING)       has_binning    = true;
    }

    /* ── direct features ── */
    g_features[VSC_FEATURE_STREAMING].available = true;
    g_features[VSC_FEATURE_CROP].available      = has_crop;
    g_features[VSC_FEATURE_BINNING].available   = has_binning;
    g_features[VSC_FEATURE_HISTOGRAM].available = has_statistics;
    g_features[VSC_FEATURE_HDR].available       = has_hdr;
    g_features[VSC_FEATURE_TRIGGER].available   = has_trigger;

    /* ── composite features (OR logic for statistics sources) ── */
    g_features[VSC_FEATURE_AUTO_EXPOSURE].available =
        has_statistics && has_exposure;

    g_features[VSC_FEATURE_AUTO_WHITE_BALANCE].available =
        has_statistics && has_sensor;

    g_derived = true;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Query API
 * ═══════════════════════════════════════════════════════════════════════ */

bool vsc_has_feature(vsc_feature_id_t feature)
{
    if (feature >= VSC_FEATURE_COUNT) return false;
    return g_features[feature].available;
}

const vsc_feature_t *vsc_feature_get(vsc_feature_id_t feature)
{
    if (feature >= VSC_FEATURE_COUNT) return NULL;
    return &g_features[feature];
}

void vsc_feature_dump(void)
{
    printf("=== VSC Features ===\n");
    for (int i = 0; i < VSC_FEATURE_COUNT; i++) {
        printf("  %-25s %-35s [%s]\n",
               g_features[i].name,
               g_features[i].description,
               g_features[i].available ? " OK " : " -- ");
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Manual override (for testing)
 * ═══════════════════════════════════════════════════════════════════════ */

void vsc_feature_set(vsc_feature_id_t feature, bool available)
{
    if (feature >= VSC_FEATURE_COUNT) return;
    g_features[feature].available = available;
    g_derived = true;
}

bool vsc_feature_is_derived(void)
{
    return g_derived;
}

void vsc_feature_reset(void)
{
    for (int i = 0; i < VSC_FEATURE_COUNT; i++)
        g_features[i].available = false;
    g_derived = false;
}
