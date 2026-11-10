/* ─────────────────────────────────────────────────────────────────────
 *  用户文件 — 由 param_gen.py --scaffold 首次生成, 之后**归你所有**
 *
 *  源:       params/exp.params.yaml
 *  数据:     exp_params.h / exp_params.c  (生成物, 请勿手改)
 *  检查缺失项: python tools/param_gen.py params gen/params --scaffold <dir>
 *
 *  生成器不会覆盖本文件; 回调里的 switch 按 YAML 初次展开, 内容由你填。
 * ───────────────────────────────────────────────────────────────────── */
#include "app_param_manager.h"
#include "module_ids.h"
#include "exp_params.h"

/* ── 模块私有上下文 (按需换成真实类型/字段) ── */
typedef struct
{
    int reserved; /* TODO */
} exp_ctx_t;

static exp_ctx_t g_exp;

/** @brief 模块初始化 (param_load_all 的 INIT 阶段) */
static int exp_init(void *ctx)
{
    exp_ctx_t *e = (exp_ctx_t *)ctx;
    (void)e;
    return PARAM_OK;
}

/** @brief 读取: 返回 PARAM_OK 才覆盖缓存; 默认让框架回退缓存 */
static int exp_read(void *ctx, uint32_t id, param_value_t *value)
{
    (void)ctx;
    (void)id;
    (void)value;
    return PARAM_ERR_INVALID_ID;
}

/** @brief 写入校验/转换 (apply): 返回非 OK = 拒绝且缓存回滚 */
static int exp_write(void *ctx, uint32_t id, param_value_t value)
{
    (void)ctx;
    (void)value;

    switch (id)
    {
    case PID_EXP_GAIN:
        break;
    case PID_EXP_OFFSET:
        break;
    case PID_EXP_SCALE:
        break;
    case PID_EXP_ENABLED:
        break;
    case PID_EXP_MODE:
        break;
    case PID_EXP_DEV_NAME:
        break;
    case PID_EXP_CALIB:
        break;
    default:
        return PARAM_OK;
    }
    return PARAM_OK;
}

/** @brief 命令执行 (EXEC) */
static int exp_exec(void *ctx, uint32_t id, param_value_t arg)
{
    (void)ctx;
    (void)arg;

    switch (id)
    {
    case PID_EXP_RECALIB:
        break;
    default:
        return PARAM_ERR_NOT_FOUND;
    }
    return PARAM_OK;
}

/** @brief 批量下发 (App): 失败保留 dirty, 下一轮 param_flush 重试 */
static int exp_flush(void *ctx)
{
    (void)ctx;
    return PARAM_OK;
}

/* ── 模块定义 + 注册 ── */
PARAM_MODULE_DEFINE(exp, MODULE_EXAMPLE_XM, "ExampleXMacro",
                    .ctx = &g_exp,
                    .init = exp_init,
                    .read = exp_read,
                    .write = exp_write,
                    .exec = exp_exec,
                    .flush = exp_flush);

PARAM_MODULE_INIT(exp, exp_params);
