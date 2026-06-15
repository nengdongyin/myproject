#ifndef PARAM_DATA_OPS_H
#define PARAM_DATA_OPS_H

#include "param_manager.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @file param_data_ops.h
     * @brief App 与 IP 共享的参数数据操作层
     *
     * 将 cache_update / read / save / load / reset 五种纯数据操作从
     * App/IP 各自的 vtable 实现中抽出，消除代码重复。
     *
     * App 和 IP 的控制策略层 (pre_write+apply vs. write+dirty_map)
     * 保持不变，仅数据通路共用。
     */

    typedef void (*param_data_cache_update_fn)(param_entry_t *e, param_value_t value);
    typedef int (*param_data_read_fn)(param_entry_t *e, param_value_t *value);
    typedef int (*param_data_save_fn)(param_entry_t *e);
    typedef int (*param_data_load_fn)(param_entry_t *e);
    typedef int (*param_data_reset_fn)(param_entry_t *e);

    typedef struct
    {
        param_data_cache_update_fn cache_update;
        param_data_read_fn read;
        param_data_save_fn save;
        param_data_load_fn load;
        param_data_reset_fn reset;
    } param_data_ops_t;

    /**
     * @brief 编译期常量分派表 (存入 .rodata)
     *
     * 按 PARAM_TYPE_xxx 索引，每种类型对应 5 个函数指针。
     * App 和 IP 的 vtable 实现均可直接查此表。
     */
    extern const param_data_ops_t g_param_data_ops[PARAM_TYPE_COUNT];

    /**
     * @brief 共享 vtable save 函数 — App 和 IP 共用
     *
     * 消除 app_save / ip_param_save 的代码重复。
     * 直接可赋值给 vtable 的 .save 字段。
     *
     * @param e 参数条目
     * @return PARAM_OK 成功，PARAM_ERR_TYPE_MISMATCH 类型越界
     */
    int param_vtable_save(param_entry_t *e);

    /**
     * @brief pre_write 校验函数签名 — 写入前校验/裁剪
     *
     * 返回 false 表示拒绝写入 (枚举值非法)；返回 true 表示通过。
     * value 可能被原地修改 (范围裁剪)。
     */
    typedef bool (*param_pre_write_fn)(param_entry_t *e, param_value_t *value);

    /**
     * @brief pre_write 编译期分派表 (存入 .rodata)
     *
     * 按 PARAM_TYPE_xxx 索引。App 和 IP 统一使用此表做写入前校验。
     * - UINT/INT/FLOAT → 范围裁剪
     * - ENUM           → 枚举值合法性校验
     * - 其余类型       → nop
     */
    extern const param_pre_write_fn g_param_pre_write[PARAM_TYPE_COUNT];

    /**
     * @brief 将参数值裁剪到 [min, max] 范围 (App 和 IP 共用)
     *
     * 若 has_range==0 则直接返回原值。
     * 支持 UINT / INT / FLOAT 三种数值类型。
     *
     * @param re    数值范围型参数条目
     * @param value 待裁剪的值
     * @return 裁剪后的值
     */
    param_value_t param_clamp_value_to_range(const param_range_entry_t *re, param_value_t value);

    /**
     * @brief 裁剪参数缓存值到合法范围 (App 和 IP 共用)
     *
     * 仅对 UINT / INT / FLOAT 类型生效。
     * 若参数不在合法范围则修正到 min~max 区间。
     *
     * @param e 参数条目指针
     */
    void param_clamp_entry(param_entry_t *e);

#ifdef __cplusplus
}
#endif

#endif /* PARAM_DATA_OPS_H */
