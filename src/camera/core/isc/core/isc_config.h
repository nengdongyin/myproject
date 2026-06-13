/**
 * @file    isc_config.h
 * @brief   ISC 编译期配置
 *
 * 所有可调参数集中于此。用户按平台约束修改本文件。
 * 内存策略：S0 — 全静态分配。
 */

#ifndef ISC_CONFIG_H
#define ISC_CONFIG_H

/* ── 传感器驱动数量 ── */
#define ISC_MAX_SENSORS         4   /**< 最大可注册传感器驱动数         */

/* ── 控制项 ── */
#define ISC_MAX_CTRLS           24  /**< 最大控制项数（含标准+私有）   */
#define ISC_MAX_EXT_CTRLS       8   /**< 批量控制最大项数               */
#define ISC_MAX_CTRL_NAME       32  /**< 控制项名称最大字符数          */
#define ISC_MAX_MENU_NAME       32  /**< 菜单项名称最大字符数          */

/* ── 设备 ── */
#define ISC_MAX_DEVS            2   /**< 最大同时打开设备数             */

/* ── 字符串 ── */
#define ISC_MAX_MODEL_NAME      32  /**< 型号名最大字符数               */
#define ISC_MAX_VENDOR_NAME     24  /**< 厂商名最大字符数               */
#define ISC_MAX_FMT_DESC        32  /**< 格式描述最大字符数             */

/* ── 裁剪 ── */
/**
 * 以下宏用于极端 RAM 裁剪 (如 32KB BRAM)，在工作区内定义即生效:
 *   ISC_CTRL_NO_ENUM    — 跳过枚举缓存, 每次线性扫描
 *   ISC_CTRL_NO_NAME    — 不存储控制项可读名称, Flash 省 ~700B (驱动 query_ctrl 不填 name)
 */

#endif /* ISC_CONFIG_H */
