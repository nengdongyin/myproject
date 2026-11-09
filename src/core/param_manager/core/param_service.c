/**
 * @file param_service.c
 * @brief 框架可选模块 — 元数据 (MODULE_META) + 用户集控制 (MODULE_SYS)
 *
 * @details
 * 两个独立模块、两个独立 init 入口, 共用本编译单元:
 *
 *   | 参数 (MODULE_SYS)                        | 类型 | 语义 (GenICam SFNC §16) |
 *   |------------------------------------------|------|--------------------------|
 *   | 0x00 PID_SYS_USERSET_SELECTOR            | ENUM | [16.2] 写 = 切落盘目标 set |
 *   | 0x01 PID_SYS_USERSET_LOAD                | EXEC | [16.4] 选中 set → 活动值 (含硬件) |
 *   | 0x02 PID_SYS_USERSET_SAVE                | EXEC | [16.5] 活动值 → 选中 set (拒出厂) |
 *   | 0x03 PID_SYS_USERSET_DEFAULT             | ENUM | [16.6] 上电加载哪个 set |
 *   | 0x04 PID_SYS_USERSET_RESET               | EXEC | [16.7] 清空选中 set (拒出厂) |
 *   | 0x05 PID_SYS_USERSET_DEFAULT_SAVE        | EXEC | [厂商扩展] 活动值 → 出厂分区 |
 *   | 0x06 PID_SYS_USERSET_DEFAULT_DELETE      | EXEC | [厂商扩展] 清空出厂分区 |
 *
 * 0x00–0x04 对应 SFNC §16 特征; 0x05/0x06 是**厂商扩展** ——
 * SFNC 不允许操作出厂集 (UserSetDefault 仅用作"上电加载哪个"), 但产线要烧写出厂
 * 默认、售后要重建出厂集, 所以补上这两条。它们不受 SELECTOR 限制, 始终作用于
 * 出厂分区, 并且是**唯一**能写/擦出厂集的入口 (SAVE / RESET 对出厂分区返回 READONLY)。
 * 这两条仅供自用 (产线 / 售后), 不进入对外 GenICam XML。
 *
 * 出厂分区 (set 0) 对 SAVE / RESET 是**只读**的 —— 改出厂分区只走 0x05/0x06,
 * 这与"出厂集不该被普通保存操作覆盖"的语义一致。
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

/** UserSet 枚举值: 0 = Default/出厂, 1..N = UserSetN。
 *  枚举校验是**成员判定**而非范围裁剪, 超界值直接 PARAM_ERR_OUT_OF_RANGE。 */
static const int32_t s_userset_values[] = {
    PARAM_PARTITION_FACTORY, 1, 2, 3, PARAM_PARTITION_USER_MAX,
};
#define USERSET_VALUE_COUNT \
    ((uint16_t)(sizeof(s_userset_values) / sizeof(s_userset_values[0])))

/* 表中 1/2/3 是写死的, 分区数一变必须同步改 —— 用编译期断言拦住漏改 */
_Static_assert(sizeof(s_userset_values) / sizeof(s_userset_values[0]) ==
                   PARAM_PARTITION_COUNT,
               "s_userset_values 与 PARAM_PARTITION_COUNT 不一致, 请同步枚举表");

/**
 * [SFNC 16.2] 选中 SAVE / LOAD / RESET 作用的 set (枚举)。
 *
 * 写入即**切换存储目标** (param_set_storage): 只换驱动、不读内容, 缓存仍是
 * 活动值 —— 所以随后的 SAVE 会把它写进新选中的 set。要载入内容须再触发
 * UserSetLoad。
 */
PARAM_ENUM(userset_selector, PID_SYS_USERSET_SELECTOR, 0, PARAM_PARTITION_FACTORY,
           s_userset_values, USERSET_VALUE_COUNT);

/** 命令型参数: 无负载, 只能被 param_exec() / param_write_raw() 触发
 *  (param_write 对 EXEC 返回 PARAM_ERR_EXEC_ONLY)。 */
PARAM_EXEC(userset_load, PID_SYS_USERSET_LOAD);
PARAM_EXEC(userset_save, PID_SYS_USERSET_SAVE);

/** [SFNC 16.6] 上电加载哪个 set (枚举, 可写) */
PARAM_ENUM(userset_default, PID_SYS_USERSET_DEFAULT, 0, PARAM_PARTITION_FACTORY,
           s_userset_values, USERSET_VALUE_COUNT);

PARAM_EXEC(userset_reset, PID_SYS_USERSET_RESET);

/** [厂商扩展] 出厂分区专用命令 — SAVE/RESET 拒绕出厂分区, 改出厂只走这两个 */
PARAM_EXEC(userset_default_save, PID_SYS_USERSET_DEFAULT_SAVE);
PARAM_EXEC(userset_default_delete, PID_SYS_USERSET_DEFAULT_DELETE);

PARAM_TABLE(sys_params,
            &userset_selector.base,
            &userset_load.base,
            &userset_save.base,
            &userset_default.base,
            &userset_reset.base,
            &userset_default_save.base,
            &userset_default_delete.base);

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

    /* param_set_storage 现已返回状态码 (缺 load/save 的驱动被拒绝) */
    int sret = param_set_storage(drv);
    if (sret != PARAM_OK) {
        SYS_LOG("[SYS] select: set %u rejected by storage layer (%s)\n",
                idx, param_strerror(sret));
        return sret;
    }

    SYS_LOG("[SYS] select: target set = %u\n", idx);
    return PARAM_OK;
}

/**
 * @brief 取 SELECTOR 当前值
 *
 * @note 值不另存副本 —— 参数缓存就是唯一真相源 (副本会与 param_reset_* /
 *       其他写入路径分叉)。枚举类型保证缓存里只可能是合法 set。
 */
static int sys_selected_get(uint8_t *idx)
{
    param_value_t v;
    if (param_read_cache(PID_SYS_USERSET_SELECTOR, &v) != PARAM_OK)
        return PARAM_ERR_NOT_SUPPORTED;

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
 *  SELECTOR 已把驱动指向目标, 所以这里只需 param_save_all()。
 *  @note 出厂分区只读 —— 改出厂集请用 USERSET_DEFAULT_SAVE。 */
static int sys_userset_save(void)
{
    uint8_t idx = 0;
    int ret = sys_selected_get(&idx);
    if (ret != PARAM_OK)
        return ret;

    if (idx == PARAM_PARTITION_FACTORY) {
        SYS_LOG("[SYS] save: factory set is read-only, use DefaultSave\n");
        return PARAM_ERR_READONLY;
    }

    ret = param_save_all();
    SYS_LOG("[SYS] save: active -> set %u, save_all=%d\n", idx, ret);
    return ret;
}

/**
 * @brief [SFNC 16.7] 清空当前选中的 set (后端 erase_all)
 *
 * @note 不动活动缓存 —— 重置活动 set 后需再 LOAD 一次才回到默认值。
 * @note 出厂分区只读 —— 清出厂集请用 USERSET_DEFAULT_DELETE。
 * @warning flashdb 后端的 erase_all 是 deinit+init, 在该后端上本命令不生效。
 */
static int sys_userset_reset(void)
{
    uint8_t idx = 0;
    int ret = sys_selected_get(&idx);
    if (ret != PARAM_OK)
        return ret;

    if (idx == PARAM_PARTITION_FACTORY) {
        SYS_LOG("[SYS] reset: factory set is read-only, use DefaultDelete\n");
        return PARAM_ERR_READONLY;
    }

    ret = param_delete_all();
    SYS_LOG("[SYS] reset: set %u cleared, ret=%d\n", idx, ret);
    return ret;
}

/**
 * @brief 在出厂分区上执行一次落盘操作 (DefaultSave / DefaultDelete 共用)
 *
 * 出厂分区不是 SAVE/RESET 的可操作目标, 所以这里临时把驱动切过去执行,
 * 完成后**一定**恢复到 SELECTOR 指向的分区 —— 否则后续所有落盘都会写进出厂。
 *
 * @param op param_save_all 或 param_delete_all
 */
static int sys_factory_op(int (*op)(void))
{
    uint8_t restore = 0;
    int ret = sys_selected_get(&restore);
    if (ret != PARAM_OK)
        return ret;

    const param_storage_drv_t *factory =
        param_get_storage_partition(PARAM_PARTITION_FACTORY);
    const param_storage_drv_t *back = param_get_storage_partition(restore);
    if (!factory || !back)
        return PARAM_ERR_NOT_FOUND;

    /* param_set_storage 现已返回状态码, 不再需要回读比对 */
    if (param_set_storage(factory) != PARAM_OK)
        return PARAM_ERR_NOT_SUPPORTED;

    ret = op();

    /* 必须恢复: 失败会让后续所有落盘都写进出厂分区 */
    if (param_set_storage(back) != PARAM_OK) {
        SYS_LOG("[SYS] factory op: FATAL, driver left on factory set\n");
        return PARAM_ERR_NOT_SUPPORTED;
    }

    SYS_LOG("[SYS] factory op: ret=%d, driver restored to set %u\n", ret, restore);
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
    case PID_SYS_USERSET_SELECTOR:   /* 枚举已校验, 只会收到合法 set */
        return sys_select((uint8_t)value.u32);
    default:
        return PARAM_OK;
    }
}

/** @brief 命令回调 — 前三个作用于选中 set, 后两个专作用于出厂分区 */
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
    case PID_SYS_USERSET_DEFAULT_SAVE:
        return sys_factory_op(param_save_all);
    case PID_SYS_USERSET_DEFAULT_DELETE:
        return sys_factory_op(param_delete_all);
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
