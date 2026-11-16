/**
 * @file port_impl.c
 * @brief 平台移植实现 — 所有 OS 分支 (#if 分派)
 */

#include "port.h"
#include "port_rtos.h"
#include <errno.h>

/**
 * @brief 有界字符串拷贝 (不依赖 libc): 保证 dst 以 '\0' 结尾
 *
 * 线程名/状态名可能超长或为 NULL, 命令侧只关心"能打印出来"。
 * static inline 定义在文件顶部, 未使用的分支不会产生 unused 警告。
 */
static inline void port_str_copy(char *dst, size_t cap, const char *src)
{
    size_t i = 0;

    if (dst == NULL || cap == 0)
        return;
    if (src != NULL)
    {
        for (; i + 1 < cap && src[i] != '\0'; i++)
            dst[i] = src[i];
    }
    dst[i] = '\0';
}

#if PORT_OS_CUSTOM

/* 用户全量自实现, 此文件不编译任何对象代码 */

#elif PORT_OS_FREERTOS

/* ================================================================
 *  FreeRTOS
 * ================================================================ */

#include "FreeRTOS.h"
#include "semphr.h"

static SemaphoreHandle_t g_mutex[SYS_LOCK_COUNT];

void system_lock(sys_lock_id_t id)
{
    if (id < SYS_LOCK_COUNT && g_mutex[id])
        xSemaphoreTake(g_mutex[id], portMAX_DELAY);
}

void system_unlock(sys_lock_id_t id)
{
    if (id < SYS_LOCK_COUNT && g_mutex[id])
        xSemaphoreGive(g_mutex[id]);
}

void system_mutex_init(void)
{
    for (int i = 0; i < SYS_LOCK_COUNT; i++)
        g_mutex[i] = xSemaphoreCreateMutex();
}

void *system_malloc(size_t size)
{
    return pvPortMalloc(size);
}

void system_free(void *ptr)
{
    vPortFree(ptr);
}

/* ---- 句柄式互斥 ---- */
struct sys_mutex
{
    SemaphoreHandle_t h;
    uint8_t used;
};
static struct sys_mutex g_sys_mutex_pool[SYS_MUTEX_POOL_SIZE];

sys_mutex_t *sys_mutex_create(void)
{
    for (int i = 0; i < SYS_MUTEX_POOL_SIZE; i++)
    {
        if (!g_sys_mutex_pool[i].used)
        {
            SemaphoreHandle_t h = xSemaphoreCreateMutex();
            if (!h)
                return NULL;
            g_sys_mutex_pool[i].h = h;
            g_sys_mutex_pool[i].used = 1;
            return &g_sys_mutex_pool[i];
        }
    }
    return NULL;
}

void sys_mutex_destroy(sys_mutex_t *m)
{
    if (m && m->used)
    {
        vSemaphoreDelete(m->h);
        m->h = NULL;
        m->used = 0;
    }
}

void sys_mutex_lock(sys_mutex_t *m)
{
    if (m && m->h)
        xSemaphoreTake(m->h, portMAX_DELAY);
}

void sys_mutex_unlock(sys_mutex_t *m)
{
    if (m && m->h)
        xSemaphoreGive(m->h);
}

/* ================================================================
 *  线程/栈信息 (ps / stacks)
 *
 *  需 configUSE_TRACE_FACILITY = 1; 否则 TaskStatus_t / usStackHighWaterMark
 *  根本不存在 → 退化 -ENOTSUP。
 *
 *  @note FreeRTOS 的 uxTaskGetSystemState() 在**数组装不下全部任务时返回 0**,
 *        所以任务数超过 PORT_FREERTOS_STAT_MAX 时这里也返回 0 (似为空), 命令侧
 *        会显示 "(none)" —— 调大下面的常量即可。
 * ================================================================ */
#if (configUSE_TRACE_FACILITY == 1)

#define PORT_FREERTOS_STAT_MAX 24

static TaskStatus_t s_task_status[PORT_FREERTOS_STAT_MAX];

static const char *port_task_state_name(eTaskState st)
{
    switch (st)
    {
    case eRunning:   return "running";
    case eReady:     return "ready";
    case eBlocked:   return "blocked";
    case eSuspended: return "suspended";
    case eDeleted:   return "deleted";
    default:         return "invalid";
    }
}

int port_thread_stat(size_t idx, port_thread_info_t *out)
{
    UBaseType_t n;

    if (out == NULL)
        return -EINVAL;

    /* 每次调用都重取一次快照: 命令是低频调试路径, 代码简单比缓存新重要 */
    n = uxTaskGetSystemState(s_task_status, PORT_FREERTOS_STAT_MAX, NULL);
    if (idx >= (size_t)n)
        return 0;

    port_str_copy(out->name, sizeof(out->name), s_task_status[idx].pcTaskName);
    port_str_copy(out->state, sizeof(out->state),
                  port_task_state_name(s_task_status[idx].eCurrentState));
    out->prio = (int32_t)s_task_status[idx].uxCurrentPriority;
    out->stack_size = 0u; /* TaskStatus_t 不提供栈总大小 */
    out->stack_free_min = (uint32_t)s_task_status[idx].usStackHighWaterMark *
                          (uint32_t)sizeof(StackType_t);
    return 1;
}

#else /* !configUSE_TRACE_FACILITY */

int port_thread_stat(size_t idx, port_thread_info_t *out)
{
    (void)idx;
    (void)out;
    return -ENOTSUP;
}

#endif /* configUSE_TRACE_FACILITY */

#elif PORT_OS_ZEPHYR

/* ================================================================
 *  Zephyr
 * ================================================================ */

#include <zephyr/kernel.h>

K_MUTEX_DEFINE(g_mutex_isc);
K_MUTEX_DEFINE(g_mutex_param);
K_MUTEX_DEFINE(g_mutex_file);

static struct k_mutex *g_mutex[SYS_LOCK_COUNT];

void system_lock(sys_lock_id_t id)
{
    if (id < SYS_LOCK_COUNT && g_mutex[id])
        k_mutex_lock(g_mutex[id], K_FOREVER);
}

void system_unlock(sys_lock_id_t id)
{
    if (id < SYS_LOCK_COUNT && g_mutex[id])
        k_mutex_unlock(g_mutex[id]);
}

void system_mutex_init(void)
{
    g_mutex[SYS_LOCK_PARAM] = &g_mutex_param;
    g_mutex[SYS_LOCK_ISC]   = &g_mutex_isc;
    g_mutex[SYS_LOCK_FILE]  = &g_mutex_file;
}

void *system_malloc(size_t size)
{
    return k_malloc(size);
}

void system_free(void *ptr)
{
    k_free(ptr);
}

/* ---- 句柄式互斥 ---- */
struct sys_mutex
{
    struct k_mutex m;
    uint8_t used;
};
static struct sys_mutex g_sys_mutex_pool[SYS_MUTEX_POOL_SIZE];

sys_mutex_t *sys_mutex_create(void)
{
    for (int i = 0; i < SYS_MUTEX_POOL_SIZE; i++)
    {
        if (!g_sys_mutex_pool[i].used)
        {
            k_mutex_init(&g_sys_mutex_pool[i].m);
            g_sys_mutex_pool[i].used = 1;
            return &g_sys_mutex_pool[i];
        }
    }
    return NULL;
}

void sys_mutex_destroy(sys_mutex_t *m)
{
    if (m)
        m->used = 0;
}

void sys_mutex_lock(sys_mutex_t *m)
{
    if (m && m->used)
        k_mutex_lock(&m->m, K_FOREVER);
}

void sys_mutex_unlock(sys_mutex_t *m)
{
    if (m && m->used)
        k_mutex_unlock(&m->m);
}

/* ================================================================
 *  线程/栈信息 (ps / stacks)
 *
 *  三个 Kconfig 必须同时开, 缺一不可:
 *    CONFIG_THREAD_MONITOR     → k_thread_foreach() 可遍历
 *    CONFIG_THREAD_STACK_INFO  → thread->stack_info.size 有效
 *    CONFIG_INIT_STACKS        → 栈以 0xAA 填充, 才能量出"历史未用量"
 *  任一缺失 → 返回 -ENOTSUP, 命令侧打印"当前后端不支持"。
 * ================================================================ */
#if defined(CONFIG_THREAD_MONITOR) && defined(CONFIG_INIT_STACKS) && \
    defined(CONFIG_THREAD_STACK_INFO)

struct port_thread_walk
{
    size_t              want;
    size_t              seen;
    port_thread_info_t *out;
    int                 found;
};

static void port_thread_walk_cb(const struct k_thread *t, void *user_data)
{
    struct port_thread_walk *w = (struct port_thread_walk *)user_data;
    size_t unused = 0;

    if (w->found || w->seen++ != w->want)
        return;

    port_str_copy(w->out->name, sizeof(w->out->name),
                  k_thread_name_get((k_tid_t)t));
    (void)k_thread_state_str((k_tid_t)t, w->out->state, sizeof(w->out->state));
    w->out->prio = (int32_t)t->base.prio;
    w->out->stack_size = (uint32_t)t->stack_info.size;
    /* 返回的是"从未被写过的字节数", 即历史最小剩余量 (需 INIT_STACKS) */
    w->out->stack_free_min =
        (k_thread_stack_space_get(t, &unused) == 0) ? (uint32_t)unused : 0u;
    w->found = 1;
}

int port_thread_stat(size_t idx, port_thread_info_t *out)
{
    struct port_thread_walk w;

    if (out == NULL)
        return -EINVAL;

    w.want = idx;
    w.seen = 0;
    w.out = out;
    w.found = 0;
    /* k_thread_foreach 内部持调度器锁, 遍历期间不会新建/销毁线程 */
    k_thread_foreach(port_thread_walk_cb, &w);
    return w.found;
}

#else /* 所需 Kconfig 未全部开启 */

int port_thread_stat(size_t idx, port_thread_info_t *out)
{
    (void)idx;
    (void)out;
    return -ENOTSUP;
}

#endif /* CONFIG_THREAD_MONITOR && CONFIG_INIT_STACKS && CONFIG_THREAD_STACK_INFO */

#elif PORT_OS_RTTHREAD

/* ================================================================
 *  RT-Thread
 * ================================================================ */

#include <rtthread.h>

static rt_mutex_t g_mutex[SYS_LOCK_COUNT];

void system_lock(sys_lock_id_t id)
{
    if (id < SYS_LOCK_COUNT && g_mutex[id])
        rt_mutex_take(g_mutex[id], RT_WAITING_FOREVER);
}

void system_unlock(sys_lock_id_t id)
{
    if (id < SYS_LOCK_COUNT && g_mutex[id])
        rt_mutex_release(g_mutex[id]);
}

void system_mutex_init(void)
{
    g_mutex[SYS_LOCK_PARAM] = rt_mutex_create("pm", RT_IPC_FLAG_PRIO);
    g_mutex[SYS_LOCK_ISC]   = rt_mutex_create("isc", RT_IPC_FLAG_PRIO);
    g_mutex[SYS_LOCK_FILE]  = rt_mutex_create("file", RT_IPC_FLAG_PRIO);
}

void *system_malloc(size_t size)
{
    return rt_malloc(size);
}

void system_free(void *ptr)
{
    rt_free(ptr);
}

/* ---- 句柄式互斥 ---- */
struct sys_mutex
{
    rt_mutex_t h;
    uint8_t used;
};
static struct sys_mutex g_sys_mutex_pool[SYS_MUTEX_POOL_SIZE];

sys_mutex_t *sys_mutex_create(void)
{
    for (int i = 0; i < SYS_MUTEX_POOL_SIZE; i++)
    {
        if (!g_sys_mutex_pool[i].used)
        {
            rt_mutex_t h = rt_mutex_create("vfsm", RT_IPC_FLAG_PRIO);
            if (!h)
                return NULL;
            g_sys_mutex_pool[i].h = h;
            g_sys_mutex_pool[i].used = 1;
            return &g_sys_mutex_pool[i];
        }
    }
    return NULL;
}

void sys_mutex_destroy(sys_mutex_t *m)
{
    if (m && m->used)
    {
        rt_mutex_delete(m->h);
        m->h = NULL;
        m->used = 0;
    }
}

void sys_mutex_lock(sys_mutex_t *m)
{
    if (m && m->h)
        rt_mutex_take(m->h, RT_WAITING_FOREVER);
}

void sys_mutex_unlock(sys_mutex_t *m)
{
    if (m && m->h)
        rt_mutex_release(m->h);
}

/* 线程/栈枚举未实现: RT-Thread 的内部对象容器遍历依赖具体版本, 暂不支持 */
int port_thread_stat(size_t idx, port_thread_info_t *out)
{
    (void)idx;
    (void)out;
    return -ENOTSUP;
}

#elif PORT_OS_LIBC

/* ================================================================
 *  libc (裸机或 PC 测试)
 * ================================================================ */

#include <stdlib.h>

#if PORT_LOCK_IRQ

static uint32_t g_critical_nest;

void system_lock(sys_lock_id_t id)
{
    (void)id;
    if (!g_critical_nest)
        __disable_irq();
    g_critical_nest++;
}

void system_unlock(sys_lock_id_t id)
{
    (void)id;
    if (!g_critical_nest)
        return;
    if (!--g_critical_nest)
        __enable_irq();
}

#else

void system_lock(sys_lock_id_t id)   { (void)id; }

void system_unlock(sys_lock_id_t id) { (void)id; }

#endif /* PORT_LOCK_IRQ */

void system_mutex_init(void) { }

void *system_malloc(size_t size)
{
    return malloc(size);
}

void system_free(void *ptr)
{
    free(ptr);
}

/* ---- 句柄式互斥（单线程，空实现） ---- */
struct sys_mutex
{
    uint8_t used;
};
static struct sys_mutex g_sys_mutex_pool[SYS_MUTEX_POOL_SIZE];

sys_mutex_t *sys_mutex_create(void)
{
    for (int i = 0; i < SYS_MUTEX_POOL_SIZE; i++)
    {
        if (!g_sys_mutex_pool[i].used)
        {
            g_sys_mutex_pool[i].used = 1;
            return &g_sys_mutex_pool[i];
        }
    }
    return NULL;
}

void sys_mutex_destroy(sys_mutex_t *m) { if (m) m->used = 0; }
void sys_mutex_lock(sys_mutex_t *m)    { (void)m; }
void sys_mutex_unlock(sys_mutex_t *m)  { (void)m; }

/* 无 RTOS: 没有"线程"可枚举 */
int port_thread_stat(size_t idx, port_thread_info_t *out)
{
    (void)idx;
    (void)out;
    return -ENOTSUP;
}

#endif /* PORT_OS_* */
