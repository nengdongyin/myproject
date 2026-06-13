/**
 * @file    isc_core.c
 * @brief   ISC 核心实现
 */

#include "isc_internal.h"
#include <string.h>

/* ──── 全局状态 (S0 全静态分配) ──── */
static uint8_t                g_sensor_count;
static const isc_sensor_ops_t *g_sensors[ISC_MAX_SENSORS];
static const isc_port_t       *g_port;
static const isc_fpga_ops_t   *g_fpga_ops;
static isc_dev_t              g_devs[ISC_MAX_DEVS];
static uint8_t                g_initialized;

/* ──── 内部辅助 ──── */

/**
 * @brief 重置设备槽为初始空闲状态
 */
static void dev_slot_reset(isc_dev_t *d)
{
    memset(d, 0, sizeof(*d));
    d->state = ISC_STATE_FREE;
}

/**
 * @brief 按名称查找传感器驱动
 * @return 索引, 未找到返回 ISC_MAX_SENSORS
 */
static uint8_t find_sensor(const char *model)
{
    for (uint8_t i = 0; i < g_sensor_count; i++) {
        if (g_sensors[i] != NULL && g_sensors[i]->model != NULL
            && strcmp(g_sensors[i]->model, model) == 0) {
            return i;
        }
    }
    return ISC_MAX_SENSORS;
}

/**
 * @brief 分配空闲设备槽
 * @return 索引, 无空闲返回 ISC_MAX_DEVS
 */
static uint8_t alloc_dev(void)
{
    for (uint8_t i = 0; i < ISC_MAX_DEVS; i++) {
        if (g_devs[i].state == ISC_STATE_FREE) {
            return i;
        }
    }
    return ISC_MAX_DEVS;
}

/**
 * @brief 根据设备指针查找索引
 */
static int8_t dev_index(const isc_dev_t *dev)
{
    if (dev == NULL) return -1;
    for (uint8_t i = 0; i < ISC_MAX_DEVS; i++) {
        if (&g_devs[i] == dev) return (int8_t)i;
    }
    return -1;
}

/**
 * @brief 在控制缓存中查找 CID 索引
 * @return 索引, 未找到返回 ISC_MAX_CTRLS
 */
static uint8_t find_cache(const isc_dev_t *dev, uint32_t cid)
{
    for (uint8_t i = 0; i < dev->num_ctrls; i++) {
        if (dev->ctrl_cache[i].cid == cid) return i;
    }
    return ISC_MAX_CTRLS;
}

/**
 * @brief 将控制值钳位到 [min,max] 并对齐 step
 */
static void clamp_value(isc_ctrl_value_t *val, const isc_ctrl_desc_t *desc)
{
    switch (desc->type) {
    case ISC_CTRL_TYPE_INTEGER:
    case ISC_CTRL_TYPE_ENUM:
        if (val->i64 < desc->min.i64) val->i64 = desc->min.i64;
        if (val->i64 > desc->max.i64) val->i64 = desc->max.i64;
        if (desc->step.i64 > 0) {
            val->i64 = desc->min.i64 +
                ((val->i64 - desc->min.i64) / desc->step.i64) * desc->step.i64;
        }
        break;
    case ISC_CTRL_TYPE_BOOLEAN:
        if (val->b > 1) val->b = 1;
        break;
    case ISC_CTRL_TYPE_FLOAT:
        if (val->f < desc->min.f) val->f = desc->min.f;
        if (val->f > desc->max.f) val->f = desc->max.f;
        /* step.f ≤ 0 表示无步进约束, 不做对齐。负步进未定义, 忽略 */
        if (desc->step.f > 0.0f) {
            /* 纯 float 运算, 无 double 依赖 (嵌入式 FPU 友好) */
            float s = desc->step.f;
            int32_t steps = (int32_t)((val->f - desc->min.f) / s + 0.5f);
            if (steps < 0) steps = 0;
            val->f = desc->min.f + (float)steps * s;
            if (val->f > desc->max.f) val->f = desc->max.f;  /* 浮点舍入边界防护 */
        }
        break;
    }
}

/**
 * @brief 裁剪坐标保守步进对齐 (核心层回退, 驱动未提供 try_fmt 时使用)
 */
static void core_crop_align(isc_fmt_t *fmt, const isc_fmt_t *base)
{
    /* 按 2 对齐 (最保守, 多数传感器至少支持 2 列对齐) */
    fmt->crop_left   &= ~1u;
    fmt->crop_top    &= ~1u;
    fmt->crop_width  &= ~1u;
    fmt->crop_height &= ~1u;

    if (fmt->crop_width  == 0 && base != NULL) fmt->crop_width  = base->crop_width;
    if (fmt->crop_height == 0 && base != NULL) fmt->crop_height = base->crop_height;

    if (fmt->width  == 0) fmt->width  = fmt->crop_width;
    if (fmt->height == 0) fmt->height = fmt->crop_height;
}

/**
 * @brief 计算时序派生值
 */
static void compute_timing(isc_timing_t *t)
{
    if (t->line_length_pclk > 0 && t->pixel_clock_hz > 0) {
        t->line_period_ns = (uint32_t)(
            (1000000000ULL * t->line_length_pclk) / t->pixel_clock_hz);
    }
    if (t->line_period_ns > 0) {
        t->frame_period_ns   = t->line_period_ns * t->frame_length_lines;
        t->readout_time_ns   = t->line_period_ns * t->readout_lines;
        t->exposure_time_ns  = t->line_period_ns * t->exposure_lines;
        t->exposure_max_ns   = t->line_period_ns * t->exposure_max_lines;
    }
}

/**
 * @brief 通知 FPGA 当前格式
 */
static void notify_fpga_format(const isc_dev_t *dev)
{
    if (dev->fpga_ops != NULL && dev->fpga_ops->ioctl != NULL) {
        dev->fpga_ops->ioctl(ISC_FPGA_FORMAT_CHANGED,
                             (void *)&dev->current_fmt,
                             dev->fpga_ops->user_data);
    }
}

/**
 * @brief 通知 FPGA 流状态
 */
static void notify_fpga_stream(const isc_dev_t *dev, uint8_t streaming)
{
    if (dev->fpga_ops != NULL && dev->fpga_ops->ioctl != NULL) {
        dev->fpga_ops->ioctl(ISC_FPGA_STREAM_STATE, &streaming,
                             dev->fpga_ops->user_data);
    }
}

/* ──── 前向声明 (在 isc_open 前调用但定义在后面的函数) ──── */
static void probe_capabilities(isc_dev_t *dev);

/* ═══════════════════════════════════════════════════════════════════════════
 * 生命周期
 * ═══════════════════════════════════════════════════════════════════════════ */

int isc_init(const isc_port_t *port,
             const isc_fpga_ops_t *fpga_ops,
             const isc_sensor_ops_t *const sensors[],
             uint8_t sensor_count)
{
    /* port 可为 NULL — 若所有传感器在 ops->port 中自带总线接口,
     * 全局 port 只作为回退默认值。fpga_ops / sensors 仍为必须。 */
    if (fpga_ops == NULL || sensors == NULL || sensor_count == 0
        || sensor_count > ISC_MAX_SENSORS) {
        return ISC_ERR_INVALID_ARG;
    }

    if (g_initialized) return ISC_OK;

    g_port         = port;
    g_fpga_ops     = fpga_ops;
    g_sensor_count = sensor_count;
    for (uint8_t i = 0; i < sensor_count; i++) {
        g_sensors[i] = sensors[i];
    }
    for (uint8_t i = sensor_count; i < ISC_MAX_SENSORS; i++) {
        g_sensors[i] = NULL;
    }

    for (uint8_t i = 0; i < ISC_MAX_DEVS; i++) {
        dev_slot_reset(&g_devs[i]);
    }

    g_initialized = 1;
    return ISC_OK;
}

int isc_deinit(void)
{
    if (!g_initialized) return ISC_OK;

    for (uint8_t i = 0; i < ISC_MAX_DEVS; i++) {
        if (g_devs[i].state != ISC_STATE_FREE) {
            isc_close(&g_devs[i]);
        }
    }

    memset(g_devs, 0, sizeof(g_devs));
    for (uint8_t i = 0; i < ISC_MAX_DEVS; i++) {
        g_devs[i].state = ISC_STATE_FREE;
    }
    g_port         = NULL;
    g_fpga_ops     = NULL;
    g_sensor_count = 0;
    g_initialized  = 0;
    return ISC_OK;
}

/**
 * @brief 为设备槽初始化传感器 ops 并执行 init+probe 序列
 * @return ISC_OK 成功, <0 失败 (设备槽已复位)
 */
static int dev_try_open(isc_dev_t *d, const isc_sensor_ops_t *ops, uint8_t s_idx)
{
    dev_slot_reset(d);
    d->ops        = ops;
    d->sensor_idx = s_idx;
    d->port       = (ops->port != NULL) ? ops->port : g_port;
    d->fpga_ops   = (ops->fpga_ops != NULL) ? ops->fpga_ops : g_fpga_ops;

    /* 若传感器未自带 port 且全局 port 也为 NULL → 无法通信 */
    if (d->port == NULL) return ISC_ERR_INVALID_ARG;

    int rc = ops->init(d);
    if (rc != ISC_OK) {
        dev_slot_reset(d);
        return rc;  /* 传递驱动 init 的真实错误码 */
    }

    rc = ops->probe(d);
    if (rc != ISC_OK) {
        if (ops->deinit) {
            int drc = ops->deinit(d);
            if (drc != ISC_OK) rc = drc;  /* deinit 失败优先报告 */
        }
        dev_slot_reset(d);
        return rc;
    }

    d->state = ISC_STATE_OPEN;
    return ISC_OK;
}

int isc_open(const char *model, isc_dev_t **dev)
{
    uint8_t s_idx, d_idx;

    if (dev == NULL || !g_initialized) return ISC_ERR_INVALID_ARG;

    d_idx = alloc_dev();
    if (d_idx >= ISC_MAX_DEVS) return ISC_ERR_NO_MEM;

    isc_dev_t *d = &g_devs[d_idx];

    if (model != NULL) {
        /* ── 精确匹配 ── */
        s_idx = find_sensor(model);
        if (s_idx >= ISC_MAX_SENSORS) return ISC_ERR_NOT_FOUND;

        int rc = dev_try_open(d, g_sensors[s_idx], s_idx);
        if (rc != ISC_OK) return rc;
    } else {
        /* ── 自动探测 ── */
        for (s_idx = 0; s_idx < g_sensor_count; s_idx++) {
            const isc_sensor_ops_t *ops = g_sensors[s_idx];
            if (ops == NULL) continue;

            int rc = dev_try_open(d, ops, s_idx);
            if (rc == ISC_OK) break;
        }
        if (s_idx >= g_sensor_count) return ISC_ERR_NOT_FOUND;
    }

    d->num_ctrls     = 0;
    d->last_ctrl_cid = 0;

    /* 一次性探测传感器能力 */
    probe_capabilities(d);

    /* 获取默认格式并同步 current_fmt, 通知 FPGA */
    if (d->ops->get_fmt) {
        isc_fmt_t init_fmt;
        memset(&init_fmt, 0, sizeof(init_fmt));
        int rc = d->ops->get_fmt(d, &init_fmt);
        if (rc == ISC_OK) {
            d->current_fmt = init_fmt;
            notify_fpga_format(d);
        }
    }

    *dev = d;
    return ISC_OK;
}

int isc_is_initialized(void)
{
    return (int)g_initialized;
}

int isc_register_ctrl_callback(isc_dev_t *dev, isc_on_ctrl_change_t cb,
                               void *user_data)
{
    if (dev == NULL) return ISC_ERR_INVALID_ARG;
    if (dev->state < ISC_STATE_OPEN) return ISC_ERR_NOT_OPEN;

    dev->on_ctrl_change = cb;
    dev->cb_user_data   = user_data;
    return ISC_OK;
}

int isc_register_error_callback(isc_dev_t *dev, isc_on_error_t cb,
                                void *user_data)
{
    if (dev == NULL) return ISC_ERR_INVALID_ARG;
    if (dev->state < ISC_STATE_OPEN) return ISC_ERR_NOT_OPEN;

    dev->on_error      = cb;
    dev->err_user_data = user_data;
    return ISC_OK;
}

int isc_close(isc_dev_t *dev)
{
    int8_t idx = dev_index(dev);
    if (idx < 0) return ISC_ERR_INVALID_ARG;

    isc_dev_t *d = &g_devs[(uint8_t)idx];
    if (d->state == ISC_STATE_FREE) return ISC_OK;

    if (d->state == ISC_STATE_STREAMING) {
        int rc = ISC_OK;
        if (d->ops->stream_off != NULL) {
            rc = d->ops->stream_off(d);
        }
        notify_fpga_stream(d, 0);
        if (rc != ISC_OK) {
            /* 流停止失败 — 透传驱动错误码, 保留设备状态 */
            return rc;
        }
    }

    if (d->ops->deinit != NULL) {
        d->ops->deinit(d);
    }

    dev_slot_reset(d);
    return ISC_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 能力与格式
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief 探测并缓存传感器能力 (isc_open 时调用一次, 无副作用)
 */
static void probe_capabilities(isc_dev_t *dev)
{
    dev->cached_caps = dev->ops->capabilities;

    /* 驱动未声明时根据 ops 可用性推断 */
    if (dev->cached_caps == 0) {
        if (dev->ops->query_timing)     dev->cached_caps |= ISC_CAP_TIMING_QUERY;
        if (dev->ops->query_constraint) dev->cached_caps |= ISC_CAP_CONSTRAINT_QUERY;
        if (dev->ops->sensor_ioctl)     dev->cached_caps |= ISC_CAP_TRIGGER_CONTROL;
        if (dev->ops->try_fmt) {
            isc_fmt_t test;
            memset(&test, 0, sizeof(test));
            test.reduction_x = 2; test.reduction_y = 2;
            test.reduction_mode = ISC_REDUCE_BIN_SUM;
            if (dev->ops->try_fmt(dev, &test) == ISC_OK)
                dev->cached_caps |= ISC_CAP_BINNING;
            test.reduction_mode = ISC_REDUCE_SKIP;
            if (dev->ops->try_fmt(dev, &test) == ISC_OK)
                dev->cached_caps |= ISC_CAP_SUBSAMPLE;
            memset(&test, 0, sizeof(test));
            test.crop_width    = 64;
            test.crop_height   = 64;
            test.reduction_x   = 2; test.reduction_y = 2;
            test.reduction_mode = ISC_REDUCE_BIN_SUM;
            if (dev->ops->try_fmt(dev, &test) == ISC_OK &&
                test.reduction_x == 2 && test.crop_width > 0)
                dev->cached_caps |= ISC_CAP_ROI_WITH_BINNING;
        }
    }
}

int isc_query_cap(isc_dev_t *dev, isc_cap_t *cap)
{
    if (dev == NULL || cap == NULL) return ISC_ERR_INVALID_ARG;
    if (dev->state < ISC_STATE_OPEN) return ISC_ERR_NOT_OPEN;

    memset(cap, 0, sizeof(*cap));
    cap->model        = dev->ops->model;
    cap->vendor       = dev->ops->vendor;
    cap->capabilities = dev->cached_caps;

    /* 枚举格式数 */
    if (dev->ops->enum_fmts) {
        isc_fmt_desc_t desc;
        uint8_t idx = 0;
        while (idx < 255 && dev->ops->enum_fmts(dev, idx, &desc) == ISC_OK) idx++;
        cap->num_formats = idx;
    }

    /* 枚举控制项数 */
    cap->num_ctrls = 0;
    if (dev->ops->query_ctrl) {
        isc_ctrl_desc_t cd;
        uint32_t std_end = ISC_CID_STANDARD_BASE + ISC_CID_STANDARD_COUNT;
        for (uint32_t cid = ISC_CID_STANDARD_BASE + 1; cid <= std_end; cid++) {
            memset(&cd, 0, sizeof(cd));
            cd.cid = cid;
            if (dev->ops->query_ctrl(dev, &cd) == ISC_OK) cap->num_ctrls++;
        }
    }
    return ISC_OK;
}

int isc_enum_fmt(isc_dev_t *dev, uint8_t index, isc_fmt_desc_t *desc)
{
    if (dev == NULL || desc == NULL) return ISC_ERR_INVALID_ARG;
    if (dev->state < ISC_STATE_OPEN) return ISC_ERR_NOT_OPEN;
    if (dev->ops->enum_fmts == NULL) return ISC_ERR_NOT_SUPPORTED;

    return dev->ops->enum_fmts(dev, index, desc);
}

int isc_get_fmt(isc_dev_t *dev, isc_fmt_t *fmt)
{
    if (dev == NULL || fmt == NULL) return ISC_ERR_INVALID_ARG;
    if (dev->state < ISC_STATE_OPEN) return ISC_ERR_NOT_OPEN;
    if (dev->ops->get_fmt == NULL) return ISC_ERR_NOT_SUPPORTED;

    /* 纯查询 — 不修改 dev->current_fmt (缓存仅由 isc_set_fmt / isc_open 更新) */
    return dev->ops->get_fmt(dev, fmt);
}

int isc_set_fmt(isc_dev_t *dev, isc_fmt_t *fmt)
{
    if (dev == NULL || fmt == NULL) return ISC_ERR_INVALID_ARG;
    if (dev->state < ISC_STATE_OPEN) return ISC_ERR_NOT_OPEN;
    if (dev->ops->set_fmt == NULL) return ISC_ERR_NOT_SUPPORTED;
    /* 流中修改格式委托驱动判断 — 全局快门传感器可能支持 */

    /* crop 全零时由驱动填充实际值 (驱动 set_fmt 负责处理) */
    int rc = dev->ops->set_fmt(dev, fmt);
    if (rc != ISC_OK) return rc;

    dev->current_fmt = *fmt;
    notify_fpga_format(dev);
    return ISC_OK;
}

int isc_try_fmt(isc_dev_t *dev, isc_fmt_t *fmt)
{
    if (dev == NULL || fmt == NULL) return ISC_ERR_INVALID_ARG;
    if (dev->state < ISC_STATE_OPEN) return ISC_ERR_NOT_OPEN;

    if (dev->ops->try_fmt != NULL) {
        return dev->ops->try_fmt(dev, fmt);
    }

    /* 回退: 用 get_fmt 获取当前值作为基准进行核心层裁剪校验 */
    if (dev->ops->get_fmt == NULL) return ISC_ERR_NOT_SUPPORTED;

    isc_fmt_t cur;
    int rc = dev->ops->get_fmt(dev, &cur);
    if (rc != ISC_OK) return rc;

    /* 格式不变则用当前值 */
    if (fmt->pixel_format == 0) fmt->pixel_format = cur.pixel_format;
    if (fmt->bit_depth    == 0) fmt->bit_depth    = cur.bit_depth;

    core_crop_align(fmt, &cur);

    return ISC_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 控制框架
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief 枚举下一个控制项 (isc_query_next_ctrl 实现)
 */
static int query_next_ctrl_impl(isc_dev_t *dev, isc_ctrl_desc_t *desc)
{
    uint32_t start = dev->last_ctrl_cid;
    uint32_t next  = 0;
    int found = 0;

    /* 标准 CID 空间: ISC_CID_STANDARD_BASE + 1 .. + COUNT */
    {
        uint32_t std_end = ISC_CID_STANDARD_BASE + ISC_CID_STANDARD_COUNT;
        uint32_t scan = (start >= ISC_CID_STANDARD_BASE && start < std_end)
            ? start + 1 : ISC_CID_STANDARD_BASE + 1;
        for (; scan <= std_end; scan++) {
            isc_ctrl_desc_t test;
            memset(&test, 0, sizeof(test));
            test.cid = scan;
            if (dev->ops->query_ctrl(dev, &test) == ISC_OK) {
                next = scan; found = 1; break;
            }
        }
    }

    /* 厂商私有 CID 空间 */
    if (!found) {
        uint32_t priv_start = (start >= ISC_CID_PRIVATE_BASE)
            ? start : (ISC_CID_PRIVATE_BASE - 1);
        for (uint32_t cid = priv_start + 1;
             cid < ISC_CID_PRIVATE_BASE + ISC_PRIVATE_CID_SCAN_COUNT; cid++) {
            isc_ctrl_desc_t test;
            memset(&test, 0, sizeof(test));
            test.cid = cid;
            if (dev->ops->query_ctrl(dev, &test) == ISC_OK) {
                next = cid; found = 1; break;
            }
        }
    }

    if (!found) {
        dev->last_ctrl_cid = 0;
        return ISC_ENUM_END;
    }

    dev->last_ctrl_cid = next;
    desc->cid = next;
    return dev->ops->query_ctrl(dev, desc);  /* 驱动自行 memset */
}

int isc_query_ctrl(isc_dev_t *dev, isc_ctrl_desc_t *desc)
{
    if (dev == NULL || desc == NULL) return ISC_ERR_INVALID_ARG;
    if (dev->state < ISC_STATE_OPEN) return ISC_ERR_NOT_OPEN;
    if (dev->ops->query_ctrl == NULL) return ISC_ERR_NOT_SUPPORTED;

    /* 直接查询指定 CID — 重置枚举游标 */
    dev->last_ctrl_cid = 0;
    return dev->ops->query_ctrl(dev, desc);
}

int isc_query_next_ctrl(isc_dev_t *dev, isc_ctrl_desc_t *desc)
{
    if (dev == NULL || desc == NULL) return ISC_ERR_INVALID_ARG;
    if (dev->state < ISC_STATE_OPEN) return ISC_ERR_NOT_OPEN;
    if (dev->ops->query_ctrl == NULL) return ISC_ERR_NOT_SUPPORTED;

    return query_next_ctrl_impl(dev, desc);
}

int isc_query_menu(isc_dev_t *dev, uint32_t cid, uint32_t index, char *name)
{
    if (dev == NULL || name == NULL) return ISC_ERR_INVALID_ARG;
    if (dev->state < ISC_STATE_OPEN) return ISC_ERR_NOT_OPEN;

    /* 委托驱动 — query_menu 自行校验 CID 类型和 index 范围 */
    if (dev->ops->query_menu != NULL) {
        return dev->ops->query_menu(dev, cid, index, name);
    }
    return ISC_ERR_NOT_SUPPORTED;
}

int isc_get_ctrl(isc_dev_t *dev, uint32_t cid, isc_ctrl_value_t *value)
{
    if (dev == NULL || value == NULL) return ISC_ERR_INVALID_ARG;
    if (dev->state < ISC_STATE_OPEN) return ISC_ERR_NOT_OPEN;
    if (dev->ops->get_ctrl == NULL) return ISC_ERR_NOT_SUPPORTED;

    /* 检查缓存: 非 VOLATILE 且有缓存 → 直接返回 */
    uint8_t ci = find_cache(dev, cid);
    if (ci < dev->num_ctrls) {
        if (!(dev->ctrl_cache[ci].flags & ISC_CTRL_FLAG_VOLATILE)) {
            *value = dev->ctrl_cache[ci].value;
            return ISC_OK;
        }
    }

    int rc = dev->ops->get_ctrl(dev, cid, value);
    if (rc != ISC_OK) return rc;

    /* 更新缓存 — 若为新 CID 且缓存未满则分配槽位 (与 set_ctrl 对称) */
    if (ci >= ISC_MAX_CTRLS && dev->num_ctrls < ISC_MAX_CTRLS) {
        ci = dev->num_ctrls++;
        dev->ctrl_cache[ci].cid = cid;
    }
    if (ci < ISC_MAX_CTRLS) {
        dev->ctrl_cache[ci].value = *value;
    }
    return ISC_OK;
}

int isc_set_ctrl(isc_dev_t *dev, uint32_t cid, isc_ctrl_value_t value)
{
    if (dev == NULL) return ISC_ERR_INVALID_ARG;
    if (dev->state < ISC_STATE_OPEN) return ISC_ERR_NOT_OPEN;
    if (dev->ops->set_ctrl == NULL) return ISC_ERR_NOT_SUPPORTED;
    if (dev->ops->query_ctrl == NULL) return ISC_ERR_NOT_SUPPORTED;

    /* 查询控制项属性以校验和钳位 */
    isc_ctrl_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.cid = cid;
    int rc = dev->ops->query_ctrl(dev, &desc);
    if (rc != ISC_OK) return ISC_ERR_NOT_SUPPORTED;

    /* 检查流中修改权限 */
    if (dev->state == ISC_STATE_STREAMING) {
        if (!(desc.flags & ISC_CTRL_FLAG_STREAMABLE)) {
            return ISC_ERR_BUSY;
        }
    }

    /* 检查只读 */
    if (desc.flags & ISC_CTRL_FLAG_READ_ONLY) {
        return ISC_ERR_NOT_SUPPORTED;
    }

    /* 钳位 */
    clamp_value(&value, &desc);

    rc = dev->ops->set_ctrl(dev, cid, value);
    if (rc != ISC_OK) return rc;

    /* 更新缓存 */
    uint8_t ci = find_cache(dev, cid);
    if (ci >= ISC_MAX_CTRLS) {
        /* 新控制项, 分配槽位 */
        if (dev->num_ctrls < ISC_MAX_CTRLS) {
            ci = dev->num_ctrls++;
            dev->ctrl_cache[ci].cid = cid;
        } else {
            /* 缓存已满 — 写硬件已成功但无法缓存。下次 get 将穿透读硬件
             * (find_cache 未命中 → 调用驱动 get_ctrl), 功能正确仅性能降级 */
            return ISC_OK;
        }
    }
    dev->ctrl_cache[ci].value = value;
    dev->ctrl_cache[ci].flags = desc.flags;

    /* 触发回调 */
    if (dev->on_ctrl_change) {
        dev->on_ctrl_change(dev, cid, value, dev->cb_user_data);
    }

    return ISC_OK;
}

int isc_get_ext_ctrls(isc_dev_t *dev, isc_ext_ctrls_t *ctrls)
{
    if (dev == NULL || ctrls == NULL) return ISC_ERR_INVALID_ARG;
    if (dev->state < ISC_STATE_OPEN) return ISC_ERR_NOT_OPEN;

    ctrls->error_idx = 0;  /* 重置残留值 */
    for (uint32_t i = 0; i < ctrls->count; i++) {
        int rc = isc_get_ctrl(dev, ctrls->items[i].cid,
                              &ctrls->items[i].value);
        if (rc != ISC_OK) {
            ctrls->error_idx = i;
            return rc;
        }
    }
    return ISC_OK;
}

int isc_set_ext_ctrls(isc_dev_t *dev, isc_ext_ctrls_t *ctrls)
{
    if (dev == NULL || ctrls == NULL) return ISC_ERR_INVALID_ARG;
    if (dev->state < ISC_STATE_OPEN) return ISC_ERR_NOT_OPEN;

    ctrls->error_idx = 0;  /* 重置残留值 */
    for (uint32_t i = 0; i < ctrls->count; i++) {
        int rc = isc_set_ctrl(dev, ctrls->items[i].cid,
                              ctrls->items[i].value);
        if (rc != ISC_OK) {
            ctrls->error_idx = i;
            return rc;
        }
    }
    return ISC_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 流控制
 * ═══════════════════════════════════════════════════════════════════════════ */

int isc_stream_on(isc_dev_t *dev)
{
    if (dev == NULL) return ISC_ERR_INVALID_ARG;
    if (dev->state != ISC_STATE_OPEN) return ISC_ERR_STATE;
    if (dev->ops->stream_on == NULL) return ISC_ERR_NOT_SUPPORTED;

    int rc = dev->ops->stream_on(dev);
    if (rc != ISC_OK) return rc;

    dev->state = ISC_STATE_STREAMING;
    notify_fpga_stream(dev, 1);
    return ISC_OK;
}

int isc_stream_off(isc_dev_t *dev)
{
    if (dev == NULL) return ISC_ERR_INVALID_ARG;
    if (dev->state != ISC_STATE_STREAMING) return ISC_ERR_STATE;
    if (dev->ops->stream_off == NULL) return ISC_ERR_NOT_SUPPORTED;

    int rc = dev->ops->stream_off(dev);
    if (rc != ISC_OK) return rc;

    dev->state = ISC_STATE_OPEN;
    notify_fpga_stream(dev, 0);
    return ISC_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 物理状态与约束
 * ═══════════════════════════════════════════════════════════════════════════ */

int isc_query_timing(isc_dev_t *dev, isc_timing_t *timing)
{
    if (dev == NULL || timing == NULL) return ISC_ERR_INVALID_ARG;
    if (dev->state < ISC_STATE_OPEN) return ISC_ERR_NOT_OPEN;
    if (dev->ops->query_timing == NULL) return ISC_ERR_NOT_SUPPORTED;

    memset(timing, 0, sizeof(*timing));
    int rc = dev->ops->query_timing(dev, timing);
    if (rc != ISC_OK) return rc;

    compute_timing(timing);
    return ISC_OK;
}

int isc_try_timing(isc_dev_t *dev, const isc_fmt_t *fmt, isc_timing_t *timing)
{
    if (dev == NULL || fmt == NULL || timing == NULL) return ISC_ERR_INVALID_ARG;
    if (dev->state < ISC_STATE_OPEN) return ISC_ERR_NOT_OPEN;

    memset(timing, 0, sizeof(*timing));

    /* 优先使用驱动提供的 try_timing */
    if (dev->ops->try_timing != NULL) {
        int rc = dev->ops->try_timing(dev, fmt, timing);
        if (rc == ISC_OK) compute_timing(timing);
        return rc;
    }

    /* 驱动未提供 try_timing → 无法计算目标格式的预期时序。
     * 调用者需 isc_set_fmt 提交后通过 isc_query_timing 获取真正生效的时序。 */
    return ISC_ERR_NOT_SUPPORTED;
}

int isc_query_constraint(isc_dev_t *dev, isc_constraint_type_t type,
                         uint32_t index, void *constraint_data,
                         uint32_t data_size)
{
    if (dev == NULL || constraint_data == NULL || data_size == 0) {
        return ISC_ERR_INVALID_ARG;
    }
    if (dev->state < ISC_STATE_OPEN) return ISC_ERR_NOT_OPEN;

    if (dev->ops->query_constraint != NULL) {
        return dev->ops->query_constraint(dev, type, index,
                                          constraint_data, data_size);
    }
    return ISC_ERR_NOT_SUPPORTED;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 传感器扩展
 * ═══════════════════════════════════════════════════════════════════════════ */

int isc_sensor_ioctl(isc_dev_t *dev, uint32_t cmd, void *arg)
{
    if (dev == NULL) return ISC_ERR_INVALID_ARG;
    if (dev->state < ISC_STATE_OPEN) return ISC_ERR_NOT_OPEN;

    if (dev->ops->sensor_ioctl != NULL) {
        return dev->ops->sensor_ioctl(dev, cmd, arg);
    }
    return ISC_ERR_NOT_SUPPORTED;
}
