#ifndef PARAM_STORAGE_COMMON_H
#define PARAM_STORAGE_COMMON_H

/**
 * @file param_storage_common.h
 * @brief 存储后端共用定义 — 分区索引 → 分区名 的唯一数据源
 *
 * flashdb / mkv / iniparser 三个后端都按同一张表把分区索引翻译成分区名。
 * 本头文件是该映射的唯一定义处: 修改 PARAM_PARTITION_COUNT 只需改这里,
 * 不必在几个后端里人工同步 (见 param_manager_config.h 的相关注释)。
 *
 * @note header-only (static inline): 不新增编译单元, 因此三个后端的
 *       构建清单 (CMakeLists.txt / test_build/build.rsp / .vscode/tasks.json)
 *       无需改动。每个引用它的 TU 各持一份内容相同的 .rodata 表,
 *       链接器通常会合并。
 */

#include "param_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 分区索引 → 分区名 的唯一数据源
 *
 * 索引: PARAM_PARTITION_FACTORY(0) = 出厂分区,
 *       1 ~ PARAM_PARTITION_USER_MAX = 用户分区。
 * 新增/重排分区时只改这里; 未填充槽位为 NULL, 按无效索引处理。
 */
#define PARAM_PARTITION_NAME_LIST                \
    [PARAM_PARTITION_FACTORY] = "param_factory", \
    [1] = "param_user0",                         \
    [2] = "param_user1",                         \
    [3] = "param_user2",                         \
    [4] = "param_user3"

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(MAX_INSTANCES >= PARAM_PARTITION_COUNT,
               "MAX_INSTANCES must be >= PARAM_PARTITION_COUNT");
#endif

/**
 * @brief 按索引取分区名 (越界或未填充 → factory 名)
 *
 * @param index 分区索引 (启动索引来自裸分区/文件, 可能是分区表外的脏值 0xFF)
 * @return 分区名, 永不为 NULL
 */
static inline const char *param_partition_name(uint8_t index)
{
    static const char *const names[PARAM_PARTITION_COUNT] = { PARAM_PARTITION_NAME_LIST };

    if (index < PARAM_PARTITION_COUNT && names[index])
        return names[index];
    return names[PARAM_PARTITION_FACTORY];
}

#ifdef __cplusplus
}
#endif

#endif /* PARAM_STORAGE_COMMON_H */
