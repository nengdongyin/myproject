/**
 * @file param_service.c
 * @brief 框架可选模块 — 元数据 (MODULE_META) + 用户集控制 (MODULE_SYS)
 *
 * @details
 * 两个独立模块、两个独立 init 入口, 共用本编译单元:
 *
 *   | 参数 (MODULE_SYS)             | 类型  | 语义 (GenICam SFNC §16) |
 *   |-------------------------------|-------|--------------------------|
 *   | 0x00 PID_SYS_USERSET_SELECTOR | UINT  | [16.2] 写 = 切落盘目标 set |
 *   | 0x01 PID_SYS_USERSET_LOAD     | EXEC  | [16.4] 选中 set → 活动值 (含硬件) |
 *   | 0x02 PID_SYS_USERSET_SAVE     | EXEC  | [16.5] 活动值 → 选中 set |
 *   | 0x03 PID_SYS_USERSET_DEFAULT  | UINT  | [16.6] 上电加载哪个 set |
 *   | 0x04 PID_SYS_USERSET_RESET    | EXEC  | [16.7] 清空选中 set |
 *
 * local_id 随 SFNC 特征编号递增 (16.3 / 16.8 / 16.9-10 未实现, 故编号有跳)。
 *
 * "旋钮即参数": 协议层只做 地址→param_id→read/write, 无需专用命令通道。
 * set 编号 = 存储分区索引: 0 = Default (param_factory), 1..N = UserSetN。
 *
 * @note 不提供 "当前加载了哪个 set" 的查询——SFNC 无此特征, 标准里的活动
 *       状态就是"设备当前的寄存器值"本身。SELECTOR 与 DEFAULT 都已可读回。
 *
 * @note 从旧 sys_param.c / meta_param.c 迁过来的产品必须先删掉自己那份, 否则
 *       module_id 撞车 (PARAM_ERR_ALREADY_REG)。参数 ID 数值未变。
 */

#include "param_manager.h"
#include "param_manager_internal.h" /* param_schema_version_read */
#include "app_param_manager.h"
#include "module_ids.h"
#include "param_service.h"

/* 日志: core/ 目录整体零 printf, 这里沿用 param_dump.c 的策略 —— 由
   PARAM_SERVICE_LOG_ENABLE 隔离, 裸机置 0 即可完全去掉 stdio。 */
#if PARAM_SERVICE_LOG_ENABLE
#include <stdio.h>
#define SYS_LOG(...) printf(__VA_ARGS__)
#else
#define SYS_LOG(...) ((void)0)
#endif

/* ═══════════════════════════════════════════════════════════════════════
 *  第 1 段: 框架元数据模块 (MODULE_META) — 存储格式版本号
 * ═══════════════════════════════════════════════════════════════════════ */

/**
 * @brief 存储格式版本号 (PID_SCHEMA_VER)
 *
 * 不变式: `entry_cache == PARAM_SCHEMA_VERSION` 恒成立 —— 否则 param_save_all
 * 会用旧缓存把已推进的版本号写回去, 下次启动即触发 erase_all。
 * 靠三点保证: 默认值取宏 (升版自动同步) + READONLY 挡住全部写入口 +
 * 只有 param_migrate_storage() 能推进它。
 *
 * @note 宽度契约: PERSIST 值类型按 sizeof(param_value_t)=4 字节落盘, 与
 *       param_migrate_storage / param_schema_version_ensure 必须一致, 否则
 *       后端 hex_decode 会因记录超长返回 PARAM_ERR_OUT_OF_RANGE。
 */
PARAM_UINT(schema_ver, PID_SCHEMA_VER,
           PARAM_FLAG_PERSIST | PARAM_FLAG_READONLY,
           PARAM_SCHEMA_VERSION, 0, UINT32_MAX);

PARAM_TABLE(meta_params, &schema_ver.base);

/** @brief 元数据模块: 纯静态数据, 无任何回调 */
PARAM_MODULE_DEFINE(param_meta, MODULE_META, "Meta",
                    NULL,           /* ctx */
                    NULL,           /* init  */
                    NULL,           /* read  — 直接返缓存 */
                    NULL,           /* write — READONLY, 到不了这里 */
                    NULL,           /* exec  */
                    NULL);          /* flush — 无硬件 */

PARAM_MODULE_INIT(param_meta, meta_params);

/* ═══════════════════════════════════════════════════════════════════════
 *  第 2 段: 用户集控制模块 (MODULE_SYS) — GenICam SFNC §16 UserSetControl
 * ═══════════════════════════════════════════════════════════════════════ */

/**
 * [SFNC 16.2] 选中 SAVE / LOAD / RESET 作用的 set。
 *
 * 写入即**切换存储目标** (param_set_storage): 只换驱动、不读内容, 缓存仍是
 * 活动值 —— 所以随后的 SAVE 会把它写进新选中的 set。要载入内容须再触发
 * UserSetLoad。
 *
 * @note 刻意不启用范围校验 (min==max 使 has_range==0): 否则 pre_write 会把
 *       越界值静默裁剪成合法值 (99→4) 然后真的切过去; 这里要求显式拒绝。
 */
PARAM_UINT(userset_selector, PID_SYS_USERSET_SELECTOR, 0,
           PARAM_PARTITION_FACTORY,
           PARAM_PARTITION_FACTORY, PARAM_PARTITION_FACTORY);

/** 命令型参数: 无负载, 只能被 param_exec() / param_write_raw() 触发
 *  (param_write 对 EXEC 返回 PARAM_ERR_READONLY)。动作取 SELECTOR 当前值。 */
PARAM_EXEC(userset_load, PID_SYS_USERSET_LOAD);
PARAM_EXEC(userset_save, PID_SYS_USERSET_SAVE);

/** [SFNC 16.6] 上电加载哪个 set: 读写, 范围 factory..user_max */
PARAM_UINT(userset_default, PID_SYS_USERSET_DEFAULT, 0,
           PARAM_PARTITION_FACTORY,
           PARAM_PARTITION_FACTORY, PARAM_PARTITION_USER_MAX);

/** 命令型参数: 同上 */
PARAM_EXEC(userset_reset, PID_SYS_USERSET_RESET);

PARAM_TABLE(sys_params,
            &userset_selector.base,
            &userset_load.base,
            &userset_save.base,
            &userset_default.base,
            &userset_reset.base);

/* ═══════════════════════════════════════════════════════════════════════
 *  UserSet 操作 — Select / Load / Save / Reset
 * ═══════════════════════════════════════════════════════════════════════ */

/**
 * @brief 把存储目标切到指定 set — 只换驱动, 不读内容 (SELECTOR 的落点)
 *
 * @note 必须在参数锁之外调用 (param_set_storage 自行进出锁)。
 * @note 只换指针不写盘, 所以选中一个全新 set 不会凭空造出文件。
 */
static int sys_select(uint8_t idx)
{
    const param_storage_drv_t *drv = param_get_storage_partition(idx);
    if (!drv) {
        SYS_LOG("[SYS] select: set %u has no driver\n", idx);
        return PARAM_ERR_NOT_FOUND;
    }

    /* 版本探测 (只读): 不把按别的格式组织过的分区当成可写目标。
       刻意不调 param_migrate_storage —— 它在版本不匹配时会 erase_all,
       选个 set 就默默清库是不可接受的副作用。 */
    uint32_t ver = 0;
    if (param_schema_version_read(drv, &ver) == PARAM_OK &&
        ver != (uint32_t)PARAM_SCHEMA_VERSION) {
        SYS_LOG("[SYS] select: set %u schema v%u != v%u, refused\n",
                idx, (unsigned)ver, (unsigned)PARAM_SCHEMA_VERSION);
        return PARAM_ERR_NOT_SUPPORTED;
    }

    param_set_storage(drv);
    SYS_LOG("[SYS] select: target set = %u\n", idx);
    return PARAM_OK;
}

/**
 * @brief 取 SELECTOR 当前值并校验
 *
 * @note SELECTOR 不启用 pre_write 裁剪, 越界值会原样留在缓存里, 所以读取侧
 *       也要拒绝。值不另存一份 —— 参数缓存就是唯一真相源 (另存会与
 *       param_reset_* / 其他写入路径分叉)。
 */
static int sys_selected_get(uint8_t *idx)
{
    param_value_t v;
    if (param_read_cache(PID_SYS_USERSET_SELECTOR, &v) != PARAM_OK)
        return PARAM_ERR_NOT_SUPPORTED;

    if (v.u32 > PARAM_PARTITION_USER_MAX)
        return PARAM_ERR_OUT_OF_RANGE;

    *idx = (uint8_t)v.u32;
    return PARAM_OK;
}

/**
 * @brief [SFNC 16.4] 把选中 set 的内容加载为活动参数 (param_load_all)
 *
 * @note 总是真的重读 (不因 selected==active 跳过), 因此也可当"丢弃未保存
 *       修改、从存储重读"用; 先幂等重指一次, 防驱动与 selected 漂移。
 */
static int sys_userset_load(void)
{
    uint8_t idx = 0;
    int ret = sys_selected_get(&idx);
    if (ret != PARAM_OK)
        return ret;

    ret = sys_select(idx);
    if (ret != PARAM_OK)
        return ret;

    ret = param_load_all();

    SYS_LOG("[SYS] load: set %u -> active, load_all=%d\n", idx, ret);
    return ret;
}

/** @brief [SFNC 16.5] 把活动值存入当前选中的 set
 *  SELECTOR 已把驱动指向目标, 所以这里只需 param_save_all()。 */
static int sys_userset_save(void)
{
    uint8_t idx = 0;
    int ret = sys_selected_get(&idx);
    if (ret != PARAM_OK)
        return ret;

    ret = param_save_all();
    SYS_LOG("[SYS] save: active -> set %u, save_all=%d\n", idx, ret);
    return ret;
}

/**
 * @brief [SFNC 16.7] 清空当前选中的 set (后端 erase_all)
 *
 * @note 不动活动缓存 —— 重置活动 set 后需再 LOAD 一次才回到默认值。
 * @warning flashdb 后端的 erase_all 是 deinit+init, 在该后端上本命令不生效。
 */
static int sys_userset_reset(void)
{
    uint8_t idx = 0;
    int ret = sys_selected_get(&idx);
    if (ret != PARAM_OK)
        return ret;

    ret = (param_delete_all() == PARAM_OK) ? PARAM_OK : PARAM_ERR_FLASH_FAIL;
    SYS_LOG("[SYS] reset: set %u cleared, ret=%d\n", idx, ret);
    return ret;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  模块回调
 * ═══════════════════════════════════════════════════════════════════════ */

/** @brief 写回调 — DEFAULT / SELECTOR 两个有状态写入 (SAVE/LOAD/RESET 是 EXEC) */
static int sys_write_cb(void *ctx, uint32_t param_id, param_value_t value)
{
    (void)ctx;

    switch (param_id) {
    case PID_SYS_USERSET_DEFAULT: {
        int ret = param_storage_set_active_partition((uint8_t)value.u32);
        SYS_LOG("[SYS] default set <- %u : %s\n", (unsigned)value.u32,
                (ret == PARAM_OK) ? "ok (下次启动生效)" : "failed");
        return ret;
    }
    case PID_SYS_USERSET_SELECTOR:
        /* 越界值不能收: 本条目没有 pre_write 裁剪, 原值会直接到达这里 */
        if (value.u32 > PARAM_PARTITION_USER_MAX) {
            SYS_LOG("[SYS] selector %u out of range\n", (unsigned)value.u32);
            return PARAM_ERR_OUT_OF_RANGE;
        }
        return sys_select((uint8_t)value.u32);
    default:
        return PARAM_OK;
    }
}

/** @brief 命令回调 — UserSetSave / Load / Reset, 均作用于当前选中的 set */
static int sys_exec_cb(void *ctx, uint32_t param_id, param_value_t arg)
{
    (void)ctx;
    (void)arg;

    switch (param_id) {
    case PID_SYS_USERSET_SAVE:
        return sys_userset_save();
    case PID_SYS_USERSET_LOAD:
        return sys_userset_load();
    case PID_SYS_USERSET_RESET:
        return sys_userset_reset();
    default:
        return PARAM_ERR_NOT_FOUND;
    }
}

/** @brief 读回调 — 为 DEFAULT 提供权威值 (其余回退缓存) */
static int sys_read_cb(void *ctx, uint32_t param_id, param_value_t *value)
{
    (void)ctx;

    if (param_id == PID_SYS_USERSET_DEFAULT) {
        uint8_t idx = PARAM_PARTITION_FACTORY;
        if (param_storage_get_active_partition(&idx) != PARAM_OK)
            return PARAM_ERR_NOT_SUPPORTED; /* 无存储/后端不支持 → 回退缓存 */
        value->u32 = idx;
        return PARAM_OK;
    }

    return PARAM_ERR_NOT_FOUND; /* 其余参数: 回退缓存 */
}

/** @brief 初始化 — 从 param_boot 同步 DEFAULT, 并给 SELECTOR 填初值 */
static int sys_module_init(void *ctx)
{
    (void)ctx;

    uint8_t idx = PARAM_PARTITION_FACTORY;
    if (param_storage_get_active_partition(&idx) != PARAM_OK)
        idx = PARAM_PARTITION_FACTORY; /* 后端不支持分区 → 视为 factory */

    /* 缓存先行, 不标脏: 这是"同步状态"而不是"用户修改" */
    (void)param_write_cache(PID_SYS_USERSET_DEFAULT, (param_value_t){ .u32 = idx });
    (void)param_write_cache(PID_SYS_USERSET_SELECTOR, (param_value_t){ .u32 = idx });

    SYS_LOG("[SYS] init: default=%u\n", idx);
    return PARAM_OK;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  模块注册
 * ═══════════════════════════════════════════════════════════════════════ */

PARAM_MODULE_DEFINE(param_service, MODULE_SYS, "UserSetControl",
                    NULL,            /* ctx */
                    sys_module_init, /* init */
                    sys_read_cb,     /* read  — Default 回读 */
                    sys_write_cb,    /* write — Default / Selector */
                    sys_exec_cb,     /* exec  — Save / Load / Reset */
                    NULL);           /* flush — 无硬件 */

PARAM_MODULE_INIT(param_service, sys_params);

/* ═══════════════════════════════════════════════════════════════════════
 *  三合一入口
 * ═══════════════════════════════════════════════════════════════════════ */

int param_service_init(const param_migrate_entry_t *table, uint16_t count)
{
    /* 1. 元数据 — 早于首次写版本键, 让 ini 里留下可读名而非 "?" */
    param_meta_module_init();

    /* 2. 版本校验 / 迁移 */
    int ret = param_migrate_storage(param_get_storage(), table, count);

    /* 3. 用户集旋钮 — 与迁移结果解耦: 迁移失败时更需要它做远程诊断与重试,
          而参数 ID 必须在 param_load_all() 之前注册好。 */
    param_service_module_init();
    return ret;
}
