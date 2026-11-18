/**
 * @file config.h
 * @brief 工程（产品）配置覆盖层 —— 唯一该按产品改动的配置文件
 *
 * @par 机制
 * 本文件由构建系统**强制预处理**（`-include config.h`，见 CMakeLists.txt /
 * test_build/build.rsp），因此在每个编译单元里都先于任何 `#include` 展开。
 *
 * 各模块的头文件写成这样：
 * @code
 *   #ifndef VFS_SH_CAT_MAX
 *   #define VFS_SH_CAT_MAX 4096      // 模块默认值
 *   #endif
 * @endcode
 * 于是**只要在这里定义一次，模块内的默认值就自动让位** ——
 * 模块文件一个字节都不用改，升级时可直接整体覆盖模块目录。
 *
 * @par 规则（三条）
 * 1. **只写偏离默认的项**。默认值留在模块里（那是模块的文档与自洽性）。
 * 2. **只能写 `#ifndef` 守卫的宏**。覆盖裸 `#define` 会变成宏重定义告警或静默失效；
 *    校验脚本 `tools/check_config_overlay.py` 负责事先抓出来。
 * 3. **不得覆盖"接口契约类"宏** —— 见文件末尾的禁止清单。
 *
 * @par 与 Zephyr Kconfig 的分工
 *   - Kconfig（prj.conf / *.conf）管 **Zephyr 自身**：内核、驱动、外部模块；
 *   - 本文件管 **本工程自己的模块旋钮**（core/、protocols/、platform/、app/）；
 *   - letter-shell 的旋钮走它自己的 overlay 机制（`SHELL_CFG_USER`
 *     → src/core/shell/shell_config.h），不在本文件重复。
 */

#ifndef PROJECT_CONFIG_H
#define PROJECT_CONFIG_H

/* ================================================================
 *  板级
 * ================================================================ */

/** @brief 通讯串口波特率 (默认 460800, 需与上位机一致) */
#define TEST_BAUD_RATE              460800

/* ================================================================
 *  参数存储后端 (三选一, 由 param_manager_config.h 的 #error 互斥校验)
 * ================================================================ */

/** @brief mkv 后端 (当前产品选择; flashdb / iniparser 保持模块默认关闭) */
#define PARAM_MKV_ENABLE            1

/* ================================================================
 *  诊断 / 日志
 *
 *  调试口开关、日志落盘、日志级别等都可在这里覆盖, 默认全部沿用模块默认:
 *    BSP_DBG_TX_ENABLE     0/1   调试口输出开关        (platform/bsp/bsp_uart.c)
 *    ELOG_OUTPUT_LVL       ELOG_LVL_* 日志级别         (lib/EasyLogger 的 elog_cfg.h)
 *    LOG_FILE_ENABLE       0/1   日志落盘 + 双文件轮换 (core/log/log_file.h)
 *    LOG_FILE_PATH         "..." 落盘路径
 *    LOG_FILE_MAX_LEN      数值  单文件上限
 * ================================================================ */

/* ================================================================
 *  其余模块旋钮 (示例, 默认不覆盖)
 *
 *  参数系统  (core/param_manager/config/param_manager_config.h)
 *    PARAM_HASH_SIZE / PARAM_MAX_ENTRIES / PARAM_SCHEMA_VERSION ...
 *  文件系统  (core/vfs/vfs_config.h)
 *    VFS_ENABLE_SHELL / VFS_SH_CAT_MAX / VFS_MAX_OPEN_FILES ...
 *  串口/协议 (platform/bsp/bsp_uart.c, core/file_transfer)
 *    BSP_UART_PIPE_SIZE / BSP_UART_RX_CHUNK / FT_LOG_ENABLE ...
 *  应用      (app/init/app_config.h)
 *    STACK_SIZE / INIT_STACK_SIZE / THREAD_PRIORITY / *_BUF_SIZE ...
 * ================================================================ */

/* ================================================================
 *  ⛔ 禁止覆盖 (接口契约 / 兼容性): 写进本文件会破坏升级兼容性或直接编译失败
 *
 *   - module_ids.h 的模块 ID 与模块初始化顺序 —— 注释已写"ID 永不漂移"；
 *   - MAKE_PARAM_ID / PARAM_MODULE_LIST 等编码宏 —— 改了等于换协议；
 *   - param_manager_config.h 的 PARAM_PARTITION_* (分区布局派生值)；
 *   - SHELL_CFG_USER —— letter-shell 自己的 overlay 入口；
 *   - 任何结构体成员顺序 / 枚举值 / 存储记录格式相关的常量。
 *
 *  校验脚本 tools/check_config_overlay.py 会拒绝这些名字。
 * ================================================================ */

#endif /* PROJECT_CONFIG_H */
