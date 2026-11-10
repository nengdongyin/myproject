/**
 * @file param_example_module.c
 * @brief 参数模块模板 — 新模块的复制起点
 *
 * 用法 (新增一个模块的完整步骤, 详见 docs/param-manager-guide.md §4):
 *   1. module_ids.h 的 PARAM_MODULE_LIST 登记一行:
 *        X(MODULE_EXAMPLE, 0x04u)   ← ID 数值自行分配 (App: 0x01~0x7F)
 *      行序即 init/flush 顺序 (IP 硬件在前, 业务模块在后)。
 *   2. 本文件: PID 宏 → 条目宏 → PARAM_TABLE → PARAM_MODULE_DEFINE →
 *      PARAM_MODULE_INIT (自动生成 example_module_init)。
 *   3. src/app/init/param_manager_init.c 的 register_modules()
 *      补一行 example_module_init(); (或开启 PARAM_MODULE_AUTO_REGISTER_ENABLE)
 *
 * 本模板覆盖: 全部 8 种参数类型 + apply 校验回调 + 回调内自修改
 * (param_write_cache 防重入) + exec 命令 + flush + 运行时动态 range。
 * 直接编译: 此文件自备本地 ID 常量, 不改 module_ids.h 也能编过
 * (仅作语法模板; 真实接入请按上面 3 步换成清单中的 ID)。
 *
 * 另见 exp_module.c + params/exp.params.yaml: 同一模块的 YAML 代码生成写法
 * —— 条目/参数表/PID 宏由 tools/param_gen.py 生成, 回调框架 (含 switch)
 * 也可由 `--scaffold` 一次生成, 手写部分只剩 case 里的业务逻辑。
 */

#include "app_param_manager.h"
#include "module_ids.h"
#include <string.h>

/* 模板本地 ID — 真实接入时换成 PARAM_MODULE_LIST 中登记的 ID */
#define EXAMPLE_LOCAL_MODULE 0x7Fu
#define EXAMPLE_PID(c) MAKE_PARAM_ID(EXAMPLE_LOCAL_MODULE, (c))

/* ---- 参数 ID (模块内追加分配, 永不插中间/改号) ---- */
#define PID_EXAMPLE_GAIN     EXAMPLE_PID(0x00)
#define PID_EXAMPLE_OFFSET   EXAMPLE_PID(0x01)
#define PID_EXAMPLE_SCALE    EXAMPLE_PID(0x02)
#define PID_EXAMPLE_ENABLED  EXAMPLE_PID(0x03)
#define PID_EXAMPLE_MODE     EXAMPLE_PID(0x04)
#define PID_EXAMPLE_NAME     EXAMPLE_PID(0x05)
#define PID_EXAMPLE_CALIB    EXAMPLE_PID(0x06)
#define PID_EXAMPLE_RECALIB  EXAMPLE_PID(0x07)

/* ---- 模块私有上下文 ---- */
typedef struct
{
    int recalib_count;
} example_ctx_t;
static example_ctx_t g_example;

/* ---- BLOB / STRING 缓冲 (单缓冲配置: 静态可写, 生命周期 = 运行期) ---- */
static char    g_example_name[16] = "example";
static uint8_t g_example_calib[8] = {0};

/* ---- 枚举合法值表 ---- */
static const int32_t g_example_modes[] = {0, 1, 2};

/* ---- 条目定义 (范围 min/max: 越界写入将被拒绝; 固定值加 PARAM_FLAG_FIXED) ---- */
PARAM_UINT  (example_gain,    PID_EXAMPLE_GAIN,    PARAM_FLAG_PERSIST, 10, 1, 100);
PARAM_INT   (example_offset,  PID_EXAMPLE_OFFSET,  PARAM_FLAG_PERSIST, 0, -50, 50);
PARAM_FLOAT (example_scale,   PID_EXAMPLE_SCALE,   PARAM_FLAG_PERSIST, 1.0f, 0.1f, 10.0f);
PARAM_BOOL  (example_enabled, PID_EXAMPLE_ENABLED, PARAM_FLAG_PERSIST, true);
PARAM_ENUM  (example_mode,    PID_EXAMPLE_MODE,    PARAM_FLAG_PERSIST, 0, g_example_modes, 3);
PARAM_STRING(example_name,    PID_EXAMPLE_NAME,    PARAM_FLAG_PERSIST, g_example_name,  sizeof(g_example_name) - 1);
PARAM_BLOB  (example_calib,   PID_EXAMPLE_CALIB,   PARAM_FLAG_PERSIST,
             g_example_calib, sizeof(g_example_calib));
PARAM_EXEC  (example_recalib, PID_EXAMPLE_RECALIB);

/* ---- 参数表 ---- */
PARAM_TABLE(example_params,
    &example_gain.base,
    &example_offset.base,
    &example_scale.base,
    &example_enabled.base,
    &example_mode.base,
    &example_name.base,
    &example_calib.base,
    &example_recalib.base,
);

/* ---- 回调: apply (校验/转换; 返回非 OK = 拒绝且缓存回滚, D2) ---- */
static int example_write(void *ctx, uint32_t param_id, param_value_t value)
{
    example_ctx_t *e = (example_ctx_t *)ctx;

    switch (param_id) {
    case PID_EXAMPLE_GAIN:
        /* 例: 业务校验 — 偶数增益才接受 */
        if ((value.u32 & 1u) != 0)
            return PARAM_ERR_OUT_OF_RANGE;
        break;
    case PID_EXAMPLE_MODE:
        /* 例: 模式切换联动 — 回调内改自己用 write_cache (跳 apply 防重入) */
        if (value.i32 == 2) {
            param_value_t scaled = {.u32 = 1};
            (void)param_write_cache(PID_EXAMPLE_GAIN, scaled);
        }
        break;
    default:
        break;
    }
    (void)e;
    return PARAM_OK;
}

/* ---- 回调: 命令执行 (EXEC 参数; 三触发路径的 arg 形状见头文件文档) ---- */
static int example_exec(void *ctx, uint32_t param_id, param_value_t arg)
{
    example_ctx_t *e = (example_ctx_t *)ctx;
    (void)arg;

    if (param_id == PID_EXAMPLE_RECALIB) {
        e->recalib_count++;
        /* 例: 重新校准后, 让缓存值重走一遍写入路径 (裁剪收敛 + notify) */
        return param_reapply(PID_EXAMPLE_GAIN);
    }
    return PARAM_ERR_NOT_FOUND;
}

/* ---- 回调: flush (App: 批量下发到硬件; 失败保留 dirty, 下轮重试) ---- */
static int example_flush(void *ctx)
{
    example_ctx_t *e = (example_ctx_t *)ctx;
    (void)e;
    /* 例: 读缓存批量下发 */
    return PARAM_OK;
}

/* ---- 回调: init (load_all 的 INIT 阶段; 在这里读已恢复的缓存并下发) ---- */
static int example_init(void *ctx)
{
    example_ctx_t *e = (example_ctx_t *)ctx;
    e->recalib_count = 0;

    param_value_t v;
    if (param_read(PID_EXAMPLE_GAIN, &v) == PARAM_OK) {
        /* ... 应用恢复后的初始值 ... */
    }
    return PARAM_OK;
}

/* ---- 模块定义 (回调具名字段, 任意顺序可省略; 杜绝位置参数写反) ---- */
PARAM_MODULE_DEFINE(example, EXAMPLE_LOCAL_MODULE, "ExampleModule",
                    .ctx   = &g_example,
                    .init  = example_init,
                    .write = example_write,
                    .exec  = example_exec,
                    .flush = example_flush);

/* ---- 注册函数 (PARAM_MODULE_INIT 生成 example_module_init; 需在
       param_manager_init.c 的 register_modules() 中调用) ---- */
PARAM_MODULE_INIT(example, example_params);

/* ---- 可选: 运行时动态范围 (如启动校准阶段按硬件能力收紧 range) ---- */
void example_module_calibrate(void)
{
    param_value_t max = {.u32 = 64};
    (void)param_set_range(PID_EXAMPLE_GAIN, NULL, &max);
}
