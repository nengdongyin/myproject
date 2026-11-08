/**
 * @file param_service.c
 * @brief 参数管理子系统自带的两个可选模块 — 框架元数据 (MODULE_META) + 运维服务 (MODULE_SYS)
 *
 * @details
 * 本文件把"任何产品都会重复写一遍"的两件事收进框架, 但仍保持**两个独立模块、
 * 两个独立 init 入口**:
 *
 *   1. MODULE_META (0x00) — `PID_SCHEMA_VER` 存储格式版本号 (只读 + 持久)
 *   2. MODULE_SYS  (0x02) — 分区切换 / 存盘 / 加载 / 热切换, 全部挂成**普通参数**
 *
 * 设计要点 (为什么旋钮是参数而不是专用命令):
 *   协议层不需要任何专用命令通道 —— imperx 桥只做 "地址 → param_id →
 *   param_read / param_write", 写一条参数就等于执行一次动作。
 *
 *   | 参数 (MODULE_SYS)  | 方向  | 语义 |
 *   |--------------------|-------|------|
 *   | 0x00 SYS_PID_BOOT_IDX   | 读/写 | 读 = 当前启动分区索引; 写 = 设置下次启动分区 |
 *   | 0x01 SYS_PID_CUR_PART   | 只读  | 运行时正在使用的存储分区 |
 *   | 0x02 SYS_PID_CMD_SAVE   | 写    | 非 0 → param_save_all() |
 *   | 0x03 SYS_PID_CMD_LOAD   | 写    | 非 0 → param_load_all() |
 *   | 0x04 SYS_PID_CMD_SWITCH | 写    | 值 = 分区索引 → 热切换 (换驱动 + 重载) |
 *
 *   两类"分区"的区别 (容易混):
 *     - **启动分区** (BOOT_IDX): 落在后端私有的 param_boot 文件里, 只影响下次
 *       上电; 写入后当前运行状态不变。
 *     - **运行时分区** (CMD_SWITCH): 立刻换掉 param_manager 的存储驱动并重新
 *       加载, 不改变下次启动分区 (除非另外写 BOOT_IDX)。
 *
 *   命令参数一律不带 PARAM_FLAG_PERSIST, 因此不会被写进 ini; 写它们只是触发器。
 *
 * @par 迁移须知 (breaking change)
 * 本文件原先是 `src/app/init/sys_param.c` + `src/app/init/meta_param.c`。任何
 * 产品若自己复制过那两份, **必须删除自己那份**再改用本文件, 否则
 * `param_module_register_node()` 按 module_id 查重会返回 PARAM_ERR_ALREADY_REG
 * (MODULE_META / MODULE_SYS 已被占用)。参数 ID 数值不变, 已存的 param_*.ini
 * 不受影响。
 *
 * @par 不链接本单元时的降级
 * 版本键的**正确性**由 core 保证 (param_migrate_storage / param_schema_version_ensure
 * 只经 drv->load/save, 不依赖条目); 失去的只是可观测性: ini 里名字为 `"?"`,
 * 且 `param_read(PID_SCHEMA_VER)` 不可用。
 */

#include "param_manager.h"
#include "param_manager_internal.h" /* param_schema_version_read */
#include "app_param_manager.h"
#include "module_ids.h"
#include "param_service.h"

/* ================================================================
 *  日志 — 服务层不是纯内存逻辑, 但也不该无条件把 stdio 拉进框架
 *
 *  框架 core/ 目录整体零 printf, 唯一的 stdio 使用者 param_dump.c 是被
 *  PARAM_DUMP_ENABLE 刻意隔离的 ("避免拉入 stdio 库")。本文件沿用同一策略:
 *  裸机产品把 PARAM_SERVICE_LOG_ENABLE 置 0 即可完全去掉日志。
 * ================================================================ */
#if PARAM_SERVICE_LOG_ENABLE
#include <stdio.h>
#define SVC_LOG(...) printf(__VA_ARGS__)
#else
#define SVC_LOG(...) ((void)0)
#endif

/* ═══════════════════════════════════════════════════════════════════════
 *  第 1 段: 框架元数据模块 (MODULE_META 0x00) — 存储格式版本号
 * ═══════════════════════════════════════════════════════════════════════ */

/**
 * @brief 存储格式版本号 (0x00000000 = MODULE_META 的 local_id 0x00)
 *
 * 三条性质共同保证一个不变式 —— **`entry_cache == PARAM_SCHEMA_VERSION` 恒成立**,
 * 即缓存与存储永不背离 (否则 param_save_all 用旧缓存覆写, 会把已推进的版本号
 * 降级, 下次启动即触发 erase_all):
 *
 *   1. 默认值 = `PARAM_SCHEMA_VERSION` (宏引用, 版本升版时自动同步);
 *      分区缺键时 value_type_load 对 NOT_FOUND 是"保持默认值", 于是缓存
 *      恰好等于当前版本, 下一次 param_save_all 自然把它补写出去。
 *   2. `PARAM_FLAG_READONLY` — param_write / _write_cache / _write_immediate /
 *      _write_raw 四个入口内均有 `entry_flags(e) & PARAM_FLAG_READONLY` 的
 *      提前返回, 缓存无法被用户改写; 版本号只能经后端 storage->save() 推进。
 *   3. param_migrate_storage() 是唯一会推进它的地方, 且只写 PARAM_SCHEMA_VERSION。
 *
 * @note 范围 [0, UINT32_MAX] 恒成立, 且 READONLY 在 pre_write 之前就返回,
 *       范围校验实际不会执行 —— 写全只是表达"无约束"。
 * @note **宽度契约**: PERSIST 的值类型参数按 sizeof(param_value_t) = 4 字节
 *       落盘 (value_type_save)。param_migrate_storage() / param_schema_version_ensure()
 *       必须用同一宽度读写, 否则后端 hex_decode 会因记录长度超出目标容量返回
 *       PARAM_ERR_OUT_OF_RANGE。
 */
PARAM_UINT(g_schema_ver, PID_SCHEMA_VER,
           PARAM_FLAG_PERSIST | PARAM_FLAG_READONLY,
           PARAM_SCHEMA_VERSION, 0, UINT32_MAX);

PARAM_TABLE(meta_params, &g_schema_ver.base);

/** @brief 元数据模块: 无 ctx, 无 read/write/exec/flush 回调 (纯静态数据) */
PARAM_MODULE_DEFINE(param_meta, MODULE_META, "Meta",
                    NULL,           /* ctx */
                    NULL,           /* init  */
                    NULL,           /* read  — 直接返缓存 */
                    NULL,           /* write — READONLY, 到不了这里 */
                    NULL,           /* exec  */
                    NULL);          /* flush — 无硬件 */

PARAM_MODULE_INIT(param_meta, meta_params);

/* ═══════════════════════════════════════════════════════════════════════
 *  第 2 段: 运维服务模块 (MODULE_SYS 0x02) — 分区 / 存盘 / 加载 / 热切换
 * ═══════════════════════════════════════════════════════════════════════ */

/** 启动分区索引: 范围限定在 factory..user_max, 越界写会被 pre_write 裁剪 */
PARAM_UINT(g_boot_idx, SYS_PID_BOOT_IDX, 0,
           PARAM_PARTITION_FACTORY,
           PARAM_PARTITION_FACTORY, PARAM_PARTITION_USER_MAX);

/** 运行时分区索引: 只读, 值由 read 回调实时提供 (缓存仅作回退) */
PARAM_UINT(g_cur_part, SYS_PID_CUR_PART, PARAM_FLAG_READONLY,
           PARAM_PARTITION_FACTORY,
           PARAM_PARTITION_FACTORY, PARAM_PARTITION_USER_MAX);

/** 命令: 保存 / 重载 (非持久; 写 0 视为空操作以免误触发) */
PARAM_UINT(g_cmd_save, SYS_PID_CMD_SAVE, 0, 0, 0, 1);
PARAM_UINT(g_cmd_load, SYS_PID_CMD_LOAD, 0, 0, 0, 1);

/** 命令: 运行时热切换 (值 = 分区索引) */
PARAM_UINT(g_cmd_switch, SYS_PID_CMD_SWITCH, 0,
           PARAM_PARTITION_FACTORY,
           PARAM_PARTITION_FACTORY, PARAM_PARTITION_USER_MAX);

PARAM_TABLE(svc_params,
            &g_boot_idx.base,
            &g_cur_part.base,
            &g_cmd_save.base,
            &g_cmd_load.base,
            &g_cmd_switch.base);

/** @brief 运行时当前分区索引 — 启动时等于启动分区, 热切换后随之改变 */
static uint8_t s_cur_partition = PARAM_PARTITION_FACTORY;

/* ═══════════════════════════════════════════════════════════════════════
 *  运行时热切换
 * ═══════════════════════════════════════════════════════════════════════ */

/**
 * @brief 把存储驱动切到指定分区并重新加载参数
 *
 * @param idx 目标分区索引 (factory 或 user_min..user_max)
 * @return PARAM_OK 成功; PARAM_ERR_NOT_FOUND 该分区拿不到驱动;
 *         PARAM_ERR_NOT_SUPPORTED 目标分区的存储格式版本与当前固件不符, 拒绝切换;
 *         其他为 load_all 的错误码
 *
 * @note 调用点必须在参数锁之外 —— 本函数内部的 param_set_storage /
 *       param_load_all 会自行进出锁 (非递归锁, 锁内调用会自死锁)。
 *       本函数由 write 回调 (apply) 触发, 框架保证回调在锁外执行。
 * @note 切换本身**不写任何东西** (param_set_storage 只换指针), 所以切到一个
 *       全新分区不会凭空造出文件 —— 后端的"空分区回退 factory"因此仍然有效。
 *       存储格式版本键会在该分区**首次落盘**时由框架补齐 (param_save_one /
 *       param_save_all 里的 param_schema_version_ensure)。
 */
static int svc_switch_partition(uint8_t idx)
{
    if (idx == s_cur_partition) {
        SVC_LOG("[SVC] switch: already on partition %u, skipped\n", idx);
        return PARAM_OK;
    }

    const param_storage_drv_t *drv = param_get_storage_partition(idx);
    if (!drv) {
        SVC_LOG("[SVC] switch: partition %u has no driver\n", idx);
        return PARAM_ERR_NOT_FOUND;
    }

    /* 版本探测 (只读, 无破坏性副作用):
       切到一个记录着**别的**存储格式版本的分区, 相当于把一堆按旧/新布局解
       释不了的记录当成有效数据读回来。因此在此拒绝切换。

       三种探测结果的处理:
         PARAM_OK 且 version == PARAM_SCHEMA_VERSION → 放行
         PARAM_OK 且 version != PARAM_SCHEMA_VERSION → **拒绝** (ERR_NOT_SUPPORTED)
         PARAM_ERR_NOT_FOUND (空分区/首次使用) 或 读失败 → 放行, 由随后的
           param_set_storage 补写版本键 / 由 load_all 上报错误

       @note 刻意**不**在此调 param_migrate_storage: 它在版本不匹配时会
             erase_all —— 热切换时静默清空一个用户分区是不可接受的副作用。
             要不要清库由调用者自己决定 (例如先主动调 migrate)。 */
    uint32_t ver = 0;
    if (param_schema_version_read(drv, &ver) == PARAM_OK &&
        ver != (uint32_t)PARAM_SCHEMA_VERSION) {
        SVC_LOG("[SVC] switch: partition %u schema v%u != v%u, refused\n",
                idx, (unsigned)ver, (unsigned)PARAM_SCHEMA_VERSION);
        return PARAM_ERR_NOT_SUPPORTED;
    }

    /* 纯指针替换 + 重载: 旧驱动仍然有效, 可随时切回 */
    param_set_storage(drv);

    int ret = param_load_all();
    s_cur_partition = idx;
    SVC_LOG("[SVC] switch: -> partition %u, load_all=%d\n", idx, ret);
    return ret;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  模块回调
 * ═══════════════════════════════════════════════════════════════════════ */

/**
 * @brief 写回调 (apply) — 命令在此执行
 *
 * @note 框架保证: 缓存已先行更新、本回调在锁外执行。返回非 OK 时缓存不回滚,
 *       仅不标脏 —— 因此命令失败也不影响后续重试。
 */
static int svc_write_cb(void *ctx, uint32_t param_id, param_value_t value)
{
    (void)ctx;

    if (param_id == SYS_PID_BOOT_IDX) {
        int ret = param_storage_set_active_partition((uint8_t)value.u32);
        SVC_LOG("[SVC] boot partition <- %u : %s\n", (unsigned)value.u32,
                (ret == PARAM_OK) ? "ok (下次启动生效)" : "failed");
        return ret;
    }

    if (param_id == SYS_PID_CMD_SAVE) {
        if (value.u32 == 0) return PARAM_OK;
        int ret = param_save_all();
        SVC_LOG("[SVC] save_all: %d\n", ret);
        return ret;
    }

    if (param_id == SYS_PID_CMD_LOAD) {
        if (value.u32 == 0) return PARAM_OK;
        int ret = param_load_all();
        SVC_LOG("[SVC] load_all: %d\n", ret);
        return ret;
    }

    if (param_id == SYS_PID_CMD_SWITCH) {
        return svc_switch_partition((uint8_t)value.u32);
    }

    return PARAM_OK;
}

/**
 * @brief 读回调 — 只为"运行时状态"类参数提供权威值
 *
 * @note 按框架契约: 只有确实拿到有效值才返回 PARAM_OK; 返回错误则框架回退
 *       到缓存值。命令类参数没有权威源, 一律回退缓存。
 */
static int svc_read_cb(void *ctx, uint32_t param_id, param_value_t *value)
{
    (void)ctx;

    if (param_id == SYS_PID_CUR_PART) {
        value->u32 = s_cur_partition;
        return PARAM_OK;
    }

    if (param_id == SYS_PID_BOOT_IDX) {
        uint8_t idx = PARAM_PARTITION_FACTORY;
        if (param_storage_get_active_partition(&idx) != PARAM_OK)
            return PARAM_ERR_NOT_SUPPORTED; /* 无存储/后端不支持 → 回退缓存 */
        value->u32 = idx;
        return PARAM_OK;
    }

    return PARAM_ERR_NOT_FOUND; /* 其余参数: 回退缓存 */
}

/**
 * @brief 模块初始化 — 把启动分区索引同步到缓存
 *
 * @note 由 param_load_all / param_reset_all 的 INIT 阶段调用。此处只填缓存,
 *       不做任何存储写操作; 运行时分区初值即启动分区。
 */
static int svc_module_init(void *ctx)
{
    (void)ctx;

    uint8_t idx = PARAM_PARTITION_FACTORY;
    if (param_storage_get_active_partition(&idx) != PARAM_OK)
        idx = PARAM_PARTITION_FACTORY; /* 后端不支持分区 → 视为 factory */

    s_cur_partition = idx;

    /* 缓存先行, 不标脏: 这是"同步状态"而不是"用户修改" */
    (void)param_write_cache(SYS_PID_BOOT_IDX, (param_value_t){ .u32 = idx });

    SVC_LOG("[SVC] init: boot partition=%u\n", idx);
    return PARAM_OK;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  模块注册
 * ═══════════════════════════════════════════════════════════════════════ */

PARAM_MODULE_DEFINE(param_service, MODULE_SYS, "System",
                    NULL,            /* ctx */
                    svc_module_init, /* init */
                    svc_read_cb,     /* read  — 运行时状态回读 */
                    svc_write_cb,    /* write — 命令执行 */
                    NULL,            /* exec  — 无 EXEC 命令 (统一用参数触发) */
                    NULL);           /* flush — 无硬件 */

PARAM_MODULE_INIT(param_service, svc_params);

/* ═══════════════════════════════════════════════════════════════════════
 *  三合一入口
 * ═══════════════════════════════════════════════════════════════════════ */

int param_service_init(const param_migrate_entry_t *table, uint16_t count)
{
    /* 1. 元数据 — 早于首次写版本键, 让 ini 里留下可读名而非 "?" */
    param_meta_module_init();

    /* 2. 版本校验 / 迁移 (core 的通用 API, 迁移表由调用方提供) */
    int ret = param_migrate_storage(param_get_storage(), table, count);

    /* 3. 运维旋钮 — 与迁移结果**解耦**: 迁移失败时更需要远程诊断与重试
          (CMD_SAVE / CMD_LOAD / CMD_SWITCH 正是为此), 所以无论成败都注册。
          参数 ID 不变, 因此它必须在 param_load_all() 之前完成。 */
    param_service_module_init();
    return ret;
}
