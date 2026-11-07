#ifndef PARAM_STORAGE_FAL_H
#define PARAM_STORAGE_FAL_H

/**
 * @file param_storage_fal.h
 * @brief 基于 FAL 裸分区的后端共用实现 — 启动分区索引读写 + FAL 惰性初始化
 *
 * flashdb 与 mkv 两个后端都把"启动分区索引"存在同一个裸分区
 * (@ref PARAM_BOOT_PART_NAME) 里, 读写逻辑逐字相同; 本头文件是该逻辑的
 * 唯一定义处。iniparser 后端把索引存在文本文件 (<base_dir>/param_boot) 里,
 * 介质不同, 不使用本文件。
 *
 * @note header-only (static inline): 不新增编译单元, 因此 flashdb / mkv 的
 *       构建清单无需改动。
 * @note 两个 get/set 函数保留了 void *ctx 形参, 以便直接赋给
 *       param_storage_drv_t 的 get_active_partition / set_active_partition
 *       槽位 (后端不区分实例, ctx 被忽略)。
 */

#include "param_manager.h"
#include "fal.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 启动索引所在的裸分区名 (所有使用 FAL 的后端共用同一个) */
#define PARAM_BOOT_PART_NAME "param_boot"

/**
 * @brief 惰性初始化 FAL — 每个引用本头文件的编译单元最多调一次
 *
 * @note 函数内的 inited 标志是每个 TU 一份。flashdb 与 mkv 若同时编入且
 *       都被调用, 最多各调用一次 fal_init() (fal_init 本身幂等)。
 * @note 非线程安全: 假定启动期单线程调用。
 */
static inline void param_fal_ensure_init(void)
{
    static bool inited = false;

    if (!inited) {
        fal_init();
        inited = true;
    }
}

/**
 * @brief 读启动分区索引 (裸分区首字节)
 *
 * @param ctx   存储上下文 (本实现忽略, 仅为匹配 vtable 回调签名)
 * @param index [out] 分区索引; 越界值折回 PARAM_PARTITION_FACTORY
 * @return 0 成功; -1 参数非法 / 启动分区不存在 / 读失败
 */
static inline int param_boot_index_get(void *ctx, uint8_t *index)
{
    (void)ctx;
    if (!index)
        return -1;

    const struct fal_partition *part = fal_partition_find(PARAM_BOOT_PART_NAME);
    if (!part)
        return -1;

    uint8_t val = 0xFF;
    if (fal_partition_read(part, 0, &val, 1) < 0)
        return -1;

    *index = (val < PARAM_PARTITION_COUNT) ? val : PARAM_PARTITION_FACTORY;
    return 0;
}

/**
 * @brief 写启动分区索引 (擦除整个裸分区后写 1 字节, 下次启动生效)
 *
 * @param ctx   存储上下文 (本实现忽略, 仅为匹配 vtable 回调签名)
 * @param index 目标分区索引, 必须 < PARAM_PARTITION_COUNT
 * @return 0 成功; -1 索引越界 / 启动分区不存在 / 擦除或写入失败
 */
static inline int param_boot_index_set(void *ctx, uint8_t index)
{
    (void)ctx;
    if (index >= PARAM_PARTITION_COUNT)
        return -1;

    const struct fal_partition *part = fal_partition_find(PARAM_BOOT_PART_NAME);
    if (!part)
        return -1;

    if (fal_partition_erase(part, 0, part->len) < 0)
        return -1;

    return (fal_partition_write(part, 0, &index, 1) < 0) ? -1 : 0;
}

#ifdef __cplusplus
}
#endif

#endif /* PARAM_STORAGE_FAL_H */
