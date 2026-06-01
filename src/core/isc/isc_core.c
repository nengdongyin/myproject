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

/* 标准 CID 全集 (用于 NEXT_CTRL 枚举时的快速跳过) */
static const uint32_t g_all_cids[] = {
    ISC_CID_HFLIP, ISC_CID_VFLIP,
    ISC_CID_EXPOSURE, ISC_CID_ANALOG_GAIN, ISC_CID_DIGITAL_GAIN,
    ISC_CID_EXPOSURE_AUTO, ISC_CID_GAIN_AUTO, ISC_CID_FRAME_RATE,
    ISC_CID_TEST_PATTERN, ISC_CID_BLACK_LEVEL, ISC_CID_TEMPERATURE,
    ISC_CID_LANE_COUNT, ISC_CID_PIXEL_CLOCK, ISC_CID_BIT_DEPTH,
    ISC_CID_LINE_LENGTH, ISC_CID_FRAME_LENGTH,
};
#define G_NUM_CIDS (sizeof(g_all_cids) / sizeof(g_all_cids[0]))

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

/* ═══════════════════════════════════════════════════════════════════════════
 * 生命周期
 * ═══════════════════════════════════════════════════════════════════════════ */

int isc_init(const isc_port_t *port,
             const isc_fpga_ops_t *fpga_ops,
             const isc_sensor_ops_t *const sensors[],
             uint8_t sensor_count)
{
    if (port == NULL || fpga_ops == NULL || sensors == NULL || sensor_count == 0
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
    d->port       = g_port;
    d->fpga_ops   = g_fpga_ops;

    int rc = ops->init(d);
    if (rc != ISC_OK) {
        dev_slot_reset(d);
        return ISC_ERR_IO;
    }

    rc = ops->probe(d);
    if (rc != ISC_OK) {
        if (ops->deinit) ops->deinit(d);
        dev_slot_reset(d);
        return ISC_ERR_NOT_FOUND;
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
    d->timing_valid  = 0;
    d->last_ctrl_cid = 0;

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
            /* 流停止失败 — 保留设备状态, 不强制释放 */
            return ISC_ERR_IO;
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
 * @brief 查询传感器能力
 *
 * 策略: 优先使用传感器驱动声明的 capabilities 字段;
 * 若驱动未声明 (capabilities==0), 回退为根据 ops 可用性推断。
 * NUM_FORMATS / NUM_CTRLS 始终通过枚举计算 (驱动可缓存但核心不假设)。
 */
int isc_query_cap(isc_dev_t *dev, isc_cap_t *cap)
{
    if (dev == NULL || cap == NULL) return ISC_ERR_INVALID_ARG;
    if (dev->state < ISC_STATE_OPEN) return ISC_ERR_NOT_OPEN;

    memset(cap, 0, sizeof(*cap));

    if (dev->ops->model) {
        strncpy(cap->model, dev->ops->model, ISC_MAX_MODEL_NAME - 1);
    }
    if (dev->ops->vendor) {
        strncpy(cap->vendor, dev->ops->vendor, ISC_MAX_VENDOR_NAME - 1);
    }
    cap->bus_type = dev->port->bus_type;

    /* 枚举格式数 */
    if (dev->ops->enum_fmts) {
        isc_fmt_desc_t desc;
        uint8_t idx = 0;
        while (idx < 255 && dev->ops->enum_fmts(dev, idx, &desc) == ISC_OK) idx++;
        cap->num_formats = idx;
    }

    /* 枚举控制项数 (标准 CID) */
    cap->num_ctrls = 0;
    if (dev->ops->query_ctrl) {
        isc_ctrl_desc_t cd;
        for (uint8_t i = 0; i < G_NUM_CIDS; i++) {
            memset(&cd, 0, sizeof(cd));
            cd.cid = g_all_cids[i];
            if (dev->ops->query_ctrl(dev, &cd) == ISC_OK) cap->num_ctrls++;
        }
    }

    /* ── 能力位: 优先使用传感器主动声明 ── */
    if (dev->ops->capabilities != 0) {
        cap->capabilities = dev->ops->capabilities;
    } else {
        /* 回退: 根据 ops 可用性推断 (不精确, 仅向后兼容旧驱动) */
        if (dev->ops->query_timing) {
            cap->capabilities |= ISC_CAP_TIMING_QUERY;
        }
        if (dev->ops->query_constraint) {
            cap->capabilities |= ISC_CAP_CONSTRAINT_QUERY;
        }
        if (dev->ops->sensor_ioctl) {
            cap->capabilities |= ISC_CAP_TRIGGER_CONTROL;
        }
        if (dev->ops->try_fmt) {
            /* 检测 binning / subsample 支持: 用 try_fmt 试探 */
            isc_fmt_t test;
            memset(&test, 0, sizeof(test));
            test.reduction = ISC_REDUCTION_BIN_2;
            if (dev->ops->try_fmt(dev, &test) == ISC_OK) {
                cap->capabilities |= ISC_CAP_BINNING;
            }
            memset(&test, 0, sizeof(test));
            test.reduction = ISC_REDUCTION_SKIP_2;
            if (dev->ops->try_fmt(dev, &test) == ISC_OK) {
                cap->capabilities |= ISC_CAP_SUBSAMPLE;
            }
            /* ROI + bin 共存: crop 非全阵列 + bin2 */
            memset(&test, 0, sizeof(test));
            test.crop_width  = 64;
            test.crop_height = 64;
            test.reduction   = ISC_REDUCTION_BIN_2;
            if (dev->ops->try_fmt(dev, &test) == ISC_OK &&
                test.reduction == ISC_REDUCTION_BIN_2 &&
                test.crop_width > 0) {
                cap->capabilities |= ISC_CAP_ROI_WITH_BINNING;
            }
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
    if (dev->state != ISC_STATE_OPEN) return ISC_ERR_BUSY;
    if (dev->ops->set_fmt == NULL) return ISC_ERR_NOT_SUPPORTED;

    /* crop 全零时由驱动填充实际值 (驱动 set_fmt 负责处理) */
    int rc = dev->ops->set_fmt(dev, fmt);
    if (rc != ISC_OK) return rc;

    dev->current_fmt = *fmt;
    dev->timing_valid = 0;  /* 格式变更, 时序缓存失效 */
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
 * @brief 枚举下一个控制项 (共用实现 — isc_query_ctrl + isc_query_next_ctrl)
 */
static int query_next_ctrl_impl(isc_dev_t *dev, isc_ctrl_desc_t *desc)
{
    uint32_t start = dev->last_ctrl_cid;
    uint32_t next  = 0;
    int found = 0;

    /* 标准 CID 空间 */
    for (uint8_t i = 0; i < G_NUM_CIDS; i++) {
        if (g_all_cids[i] > start) {
            isc_ctrl_desc_t test;
            memset(&test, 0, sizeof(test));
            test.cid = g_all_cids[i];
            if (dev->ops->query_ctrl(dev, &test) == ISC_OK) {
                next  = g_all_cids[i];
                found = 1;
                break;
            }
        }
    }

    /* 厂商私有 CID 空间 */
    if (!found) {
        uint32_t priv_start = (start >= ISC_CID_PRIVATE_BASE)
            ? start : (ISC_CID_PRIVATE_BASE - 1);
        for (uint32_t cid = priv_start + 1; cid < ISC_CID_PRIVATE_BASE + 256; cid++) {
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
    memset(desc, 0, sizeof(*desc));
    desc->cid = next;
    return dev->ops->query_ctrl(dev, desc);
}

int isc_query_ctrl(isc_dev_t *dev, isc_ctrl_desc_t *desc)
{
    if (dev == NULL || desc == NULL) return ISC_ERR_INVALID_ARG;
    if (dev->state < ISC_STATE_OPEN) return ISC_ERR_NOT_OPEN;
    if (dev->ops->query_ctrl == NULL) return ISC_ERR_NOT_SUPPORTED;

    if (!(desc->cid & ISC_CTRL_FLAG_NEXT_CTRL)) {
        /* 直接查询指定 CID — 重置枚举迭代器, 避免跳过已查 CID 之前的项 */
        dev->last_ctrl_cid = 0;
        return dev->ops->query_ctrl(dev, desc);
    }

    /* @deprecated NEXT_CTRL 路径 — 委托给共用实现 */
    return query_next_ctrl_impl(dev, desc);
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

    /* 确认是 ENUM 类型 */
    isc_ctrl_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.cid = cid;
    int rc = dev->ops->query_ctrl(dev, &desc);
    if (rc != ISC_OK) return rc;
    if (desc.type != ISC_CTRL_TYPE_ENUM) return ISC_ERR_NOT_SUPPORTED;
    if (index < (uint32_t)desc.min.i64 || index > (uint32_t)desc.max.i64) {
        return ISC_ERR_CTRL_RANGE;
    }

    /* 优先使用驱动提供的 query_menu 回调 */
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
            /* 缓存已满 — 无法缓存但写硬件已成功; 清除旧缓存中
             * 最后一个非 VOLATILE 项的标记以强制下次 get 穿透 */
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
    dev->cached_timing = *timing;
    dev->timing_valid  = 1;
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

    /* FALLBACK: 驱动未提供 try_timing → 用 query_timing 获取当前物理状态。
     * ⚠ 注意: 返回的是当前模式的时序, 而非目标格式的预期时序。
     * 若当前模式与目标格式差异显著 (格式/分辨率/crop), 结果不准确。
     * 仅作为安全基准供调用者参考, 最终仍需 isc_set_fmt 提交后
     * 再通过 isc_query_timing 获取真正生效的时序。 */
    if (dev->ops->query_timing != NULL) {
        int rc = dev->ops->query_timing(dev, timing);
        if (rc == ISC_OK) compute_timing(timing);
        return rc;
    }
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
