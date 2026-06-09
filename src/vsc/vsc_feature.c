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

/**
 * @brief 全局特性注册表（编译期静态初始化，运行时由 vsc_feature_derive() 更新）
 * @details 每个条目的 name/description 在编译期固化，
 *          available 字段在运行时由 vsc_feature_derive() 根据驱动注册表动态设置。
 */
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

/**
 * @brief 从已注册驱动自动推导特性可用性
 * @details 算法分为两个阶段：
 *          **阶段 1 — 能力扫描**
 *          遍历所有已注册驱动（vsc_driver_by_index），收集其 capabilities
 *          位掩码中的各项能力标志。
 *          **阶段 2 — 特性合成**
 *          - 直接映射：CROP/BINNING 等特性直接对应 capability 位。
 *          - 组合特性：AUTO_EXPOSURE 和 AUTO_WHITE_BALANCE 需要多个
 *            capability 同时存在（AND 逻辑）。
 *            · AUTO_EXPOSURE → STATISTICS && EXPOSURE_CTRL
 *            · AUTO_WHITE_BALANCE → STATISTICS && SENSOR
 *          STREAMING 特性始终为 true（有管线即支持流传输）。
 *          完成后设置全局标志 g_derived = true。
 * @note 本函数不是幂等的——每次调用都会重新扫描并覆盖所有特性状态。
 *       应在 vsc_system_init() 之后调用一次。
 */
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
 *  Query API（详细文档见 vsc_feature.h）
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
