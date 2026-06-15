/**
 * @file mkv.h
 * @brief 极简 KV 存储库 — 面向嵌入式 NOR Flash 的追加写键值存储
 *
 * @details
 * 设计原则:
 *   - 固定 4 字节整数键 (param_id)，Blob 值
 *   - 追加写 (append-only) + 双 sector 轮转 GC
 *   - 每条记录 10 字节头 + CRC16 校验
 *   - 无动态内存分配、无缓存、无遍历器
 *   - 依赖 FAL (Flash Abstraction Layer) 进行 flash 操作
 *
 * Sector 布局 (每分区 2 个 sector):
 * @code
 * ┌──────────────┬────────┬────────┬──────┬─────────────────────┐
 * │ sec_hdr (8B) │ rec #1 │ rec #2 │ ...  │      empty          │
 * └──────────────┴────────┴────────┴──────┴─────────────────────┘
 * @endcode
 *
 * Sector Header (8 字节):
 * ┌────────┬────────┬───────┐
 * │ magic  │  seq   │ crc16 │
 * │ 2B     │  4B    │  2B   │
 * │0xB0B0  │        │       │
 * └────────┴────────┴───────┘
 *
 * Record (10 字节头 + value + 2 字节 CRC16):
 * ┌─────────┬──────────┬───────────┬──────────────┬────────┐
 * │  magic  │ param_id │ value_len │    value     │ crc16  │
 * │  2B     │  4B      │  2B       │   N bytes    │  2B    │
 * │ 0xA5C3  │          │           │              │        │
 * └─────────┴──────────┴───────────┴──────────────┴────────┘
 *  CRC16 覆盖范围: [param_id + value_len + value]
 *
 * value_len = 0xFFFF → 墓碑记录 (删除标记)
 */

#ifndef MKV_H
#define MKV_H

#include <stdint.h>
#include <stdbool.h>

/** @brief 记录头大小 (magic + id + len) */
#define MKV_REC_HEAD_SIZE   8

/** @brief CRC16 大小 */
#define MKV_CRC_SIZE        2

/** @brief 记录固定开销 (头 + CRC，不含 value) */
#define MKV_REC_OVERHEAD    (MKV_REC_HEAD_SIZE + MKV_CRC_SIZE)

/** @brief Sector Header 大小 */
#define MKV_SEC_HDR_SIZE    8

/** @brief 记录有效魔数 */
#define MKV_REC_MAGIC       0xA5C3

/** @brief Sector Header 魔数 */
#define MKV_SEC_MAGIC       0xB0B0

/** @brief 墓碑标记 (value_len = 0xFFFF 表示已删除) */
#define MKV_TOMBSTONE       0xFFFF

/** @brief 最小 sector 大小 (必须 ≥ 2×最大记录) */
#define MKV_MIN_SECTOR_SIZE 256

/**
 * @brief 极简 KV 实例句柄
 *
 * 每个 FAL 分区对应一个 mkv_t 实例。
 * 所有字段由 mkv_init() 填充，用户只读。
 */
typedef struct
{
    const struct fal_partition *part;  /**< FAL 分区描述符 */
    uint32_t sector_size;              /**< 单个 sector 大小 (字节) */
    uint32_t active_base;              /**< 当前活跃 sector 起始偏移 */
    uint32_t active_seq;               /**< 当前活跃 sector 的 seq */
    uint32_t write_offset;             /**< 当前 sector 内写入偏移 */
    bool     initialized;              /**< 是否已初始化 */
} mkv_t;

/**
 * @brief 初始化 KV 实例
 *
 * 扫描两个 sector header，选择 seq 较大且 CRC 正确的作为活跃 sector。
 * 全新的分区 (两个 sector 均无效) 自动格式化 sector 0。
 *
 * @param kv            未初始化的 mkv_t 实例
 * @param fal_part_name FAL 分区名 (如 "param_user0")
 * @return 0 成功，-1 失败 (分区不存在)
 */
int mkv_init(mkv_t *kv, const char *fal_part_name);

/**
 * @brief 读取 param_id 的最新有效值
 *
 * 从活跃 sector 头部顺序扫描，找到 param_id 匹配的最后一条记录:
 *   - 若最后一条是墓碑 (value_len=0xFFFF) → 返回 0 (不存在)
 *   - 若最后一条有效 → 拷贝 value 到 buf，返回实际字节数
 *
 * @param kv      已初始化的实例
 * @param id      参数 ID
 * @param buf     输出缓冲区
 * @param max_len 缓冲区最大长度
 * @return >0 实际读取字节数，0 不存在，-1 参数错误
 */
int mkv_get(mkv_t *kv, uint32_t id, uint8_t *buf, uint16_t max_len);

/**
 * @brief 写入 param_id 的值 (追加写)
 *
 * 若当前 sector 剩余空间不足，触发 GC 后再写入。
 * 同 id 多次写入产生多条记录，get 返回最新一条。
 *
 * @param kv   已初始化的实例
 * @param id   参数 ID
 * @param data 值数据
 * @param len  值长度 (0~65534, 不能为 0xFFFF)
 * @return 0 成功，-1 失败
 */
int mkv_set(mkv_t *kv, uint32_t id, const uint8_t *data, uint16_t len);

/**
 * @brief 删除 param_id (追加墓碑记录)
 *
 * 写入一条 value_len=0xFFFF 的记录表示删除。
 *
 * @param kv 已初始化的实例
 * @param id 参数 ID
 * @return 0 成功，-1 失败
 */
int mkv_del(mkv_t *kv, uint32_t id);

/**
 * @brief 擦除整个分区 (两个 sector 全部擦除并重新格式化)
 *
 * @param kv 已初始化的实例
 * @return 0 成功，-1 失败
 */
int mkv_erase_all(mkv_t *kv);

/**
 * @brief 反初始化 (当前为空操作，保留用于未来扩展)
 *
 * @param kv 已初始化的实例
 */
void mkv_deinit(mkv_t *kv);

#endif /* MKV_H */
