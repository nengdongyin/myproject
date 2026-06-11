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
 * @brief 从当前 Driver 注册表推导所有特性的可用性
 * @details 遍历所有已注册的驱动，根据其 capabilities 位掩码
 *          推导各项特性是否可用：
 *          - 直接特性（CROP/BINNING/HDR/TRIGGER/HISTOGRAM）：
 *            对应 capability 位存在即视为可用。
 *          - 组合特性（AUTO_EXPOSURE / AUTO_WHITE_BALANCE）：
 *            采用 AND 逻辑——需要多个 capability 位同时存在。
 *            AUTO_EXPOSURE = STATISTICS && EXPOSURE_CTRL；
 *            AUTO_WHITE_BALANCE = STATISTICS && SENSOR。
 *          必须在 vsc_system_init() 之后调用一次。
 */
void vsc_feature_derive(void);

/**
 * @brief 查询某项特性是否可用
 * @param[in] feature 特性 ID（vsc_feature_id_t 枚举值）
 * @return true 表示该特性在推导后被标记为可用
 * @note 调用前必须先调用 vsc_feature_derive()，否则所有特性均为 false
 */
bool vsc_has_feature(vsc_feature_id_t feature);

/**
 * @brief 获取特性的完整描述符
 * @param[in] feature 特性 ID（vsc_feature_id_t 枚举值）
 * @return 指向特性描述符的指针（含 name/description/available）
 * @retval NULL feature 超出 VSC_FEATURE_COUNT 范围
 */
const vsc_feature_t *vsc_feature_get(vsc_feature_id_t feature);

/**
 * @brief 打印所有特性及其可用性（调试 / 命令行用途）
 * @details 通过 printf 输出一个格式化表格，每行显示：
 *          特性名 | 描述 | [OK]/[--]
 */
void vsc_feature_dump(void);

/* ── test / debug helpers ── */

/**
 * @brief 手动覆盖某项特性的可用性（仅用于测试）
 * @param[in] feature  特性 ID
 * @param[in] available 强制设为 true 或 false
 * @note 调用后会自动标记 g_derived = true
 */
void vsc_feature_set(vsc_feature_id_t feature, bool available);

/**
 * @brief 查询 vsc_feature_derive() 是否已被调用过
 * @return true 表示已调用过 derive 或 set
 */
bool vsc_feature_is_derived(void);

/**
 * @brief 将所有特性重置为不可用状态（仅用于测试）
 * @note 同时清除 g_derived 标志
 */
void vsc_feature_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* VSC_FEATURE_H */
