/**
 * @file log_file.c
 * @brief 日志落盘实现 —— 双文件轮换
 *
 * @details
 * 设计取舍 (每条都对应一个本工程的具体约束, 改动前请先读):
 *
 *   - **走项目 VFS, 不用 stdio**: 目标平台 (Zephyr + littlefs) 未开
 *     VFS_ENABLE_STDIO, fopen 没有后端; 文件访问唯一入口是 vfs_open 等。
 *   - **交替写, 不用 rename**: VFS 的 vfs_rename 由 VFS_ENABLE_RENAME
 *     裁剪 (默认 0), 编译期就不存在。交替写只要 stat/open/write/close。
 *   - **上电按长度认定"最新"**: VFS 的 stat 只有类型与长度, 没有 mtime;
 *     详见 log_pick_current() 的不变式推导。
 *   - **不长期持有句柄**: VFS 文件句柄池只有 VFS_MAX_OPEN_FILES(6) 个,
 *     日志常驻占一个会挤压 ymodem 收发; 且 vfs_deinit() 之后旧 fd 号可能
 *     被别的模块复用, 那时再 write 就把日志写进别人的文件。代价只是每段
 *     一次 open/close —— 后端是缓冲输出 (ELOG_BUF_OUTPUT), 一段通常上千
 *     字节, 开销可忽略。
 *   - **懒打开 + 失败节流**: 日志初始化早于文件系统挂载 (本工程
 *     log_init() 在 file_storage_init() 之前), 一次性失败会丢掉整段启动
 *     日志, 因此首次失败后按 LOG_FILE_RETRY_MS 重试而非永久放弃。
 */

#include "log_file.h"

#if LOG_FILE_ENABLE

#include <stdbool.h>
#include "vfs.h"
#include "port.h" /* system_get_time_ms() */

/* 编译期护栏: 路径必须放得进 VFS 的路径缓冲 (轮换后缀也要算进去) */
#if (LOG_FILE_MAX_LEN) < 1
#error "LOG_FILE_MAX_LEN must be >= 1"
#endif
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(LOG_FILE_PATH LOG_FILE_BAK_SUFFIX) <= VFS_PATH_MAX,
               "LOG_FILE_PATH + LOG_FILE_BAK_SUFFIX exceeds VFS_PATH_MAX");
#endif

/** @brief 两个轮换文件 (下标与 s_rotate.cur 对应) */
static const char *const s_log_paths[2] = {
    LOG_FILE_PATH,
    LOG_FILE_PATH LOG_FILE_BAK_SUFFIX,
};

/** @brief 轮换状态 (由调用方的输出锁串行化, 见 log_file_write 的 @note) */
static struct
{
    uint8_t cur;           /**< 当前(最新)文件下标: 0 主文件 / 1 备份文件 */
    bool cur_known;        /**< cur 是否已是"盘上真实状态"(首次写成功后置位) */
    uint32_t last_fail_ms; /**< 上次打开失败的时刻 (0 = 上次成功) */
} s_rotate;

/** @brief 取第 idx 个文件的当前长度 (不存在/未挂载按 0 处理) */
static uint32_t log_file_size(uint8_t idx)
{
    vfs_stat_t st;
    return (vfs_stat(s_log_paths[idx], &st) == 0) ? st.size : 0;
}

/**
 * @brief 按盘上长度推导"当前(最新)文件"下标 — 上电时唯一的依据
 *
 * 轮换只在"当前文件写满"时发生, 因此稳态下两个文件里恒有一个 < 上限
 * (最新) 、另一个 >= 上限 (上一轮):
 *   - 恰有一个未满  → 未满的那个就是最新;
 *   - 两个都未满    → 取较长者 (只有一边存在时, 长的那边显然在写);
 *   - 两个都满      → 取较长者, 首次落盘即触发轮换 (异常掉电后的兜底)。
 *
 * @note 不能简单假定"最新 = 主文件": 上次若在备份文件上结束 (cur==1),
 *      主文件恰好是满的, 假定 0 会把备份文件当成"满的旧文件"清掉 ——
 *      丢掉的正是最近一次运行、最该保留的那段日志。
 * @note VFS 没有 mtime, 所以判定只能靠长度这条不变式。
 */
static uint8_t log_pick_current(void)
{
    const uint32_t s0 = log_file_size(0);
    const uint32_t s1 = log_file_size(1);
    const bool full0 = (s0 >= (uint32_t)LOG_FILE_MAX_LEN);
    const bool full1 = (s1 >= (uint32_t)LOG_FILE_MAX_LEN);

    if (full0 != full1)
        return full0 ? 1u : 0u; /* 未满的那个是最新 */
    return (s1 > s0) ? 1u : 0u; /* 都满 / 都不满: 取较长者 */
}

void log_file_init(void)
{
    /* 首次落盘推迟到第一段日志 (此时文件系统通常还没挂载); cur 也不预设,
       由 log_pick_current 在首次写盘时按盘上长度认定 */
    s_rotate.cur = 0;
    s_rotate.cur_known = false;
    s_rotate.last_fail_ms = 0;
}

/**
 * @brief 把一段日志追加落盘, 必要时切到另一个文件
 *
 * @note 不做任何加锁: 由调用方的输出锁串行化; 因此本函数**不得**再取锁,
 *       否则与端口层的非递归锁冲突 (vfs 内部另有挂载点锁, 必须在本锁之下
 *       获取 —— 依赖方向 elog 输出锁 → vfs 挂载锁, 反向不存在)。
 * @note 任何失败都只放弃本段: 未挂载/只读/句柄池满/短写一律不报错上抛。
 */
void log_file_write(const uint8_t *data, uint32_t len)
{
    if (!data || len == 0)
        return;

    /* 1) 刚失败过就跳过本段 (节流): 文件系统没挂上时, 每段都 open 一次
          只是纯 syscall 风暴 */
    const uint32_t now = system_get_time_ms();
    if (s_rotate.last_fail_ms != 0 &&
        (uint32_t)(now - s_rotate.last_fail_ms) < LOG_FILE_RETRY_MS)
        return;

    /* 2) 尚未写过盘 → 按盘上长度认定"当前文件"; 不能沿用上电初值 0
          (理由见 log_pick_current) */
    if (!s_rotate.cur_known)
        s_rotate.cur = log_pick_current();

    /* 3) 当前文件已满 或 本段会越过上限 → 目标切到另一个, 并清空它
          (size==0 时不轮换, 避免单段超长日志来回空转) */
    uint8_t idx = s_rotate.cur;
    const uint32_t size = log_file_size(idx);
    const bool rotate = (size > 0) &&
                        (size >= (uint32_t)LOG_FILE_MAX_LEN ||
                         len > (uint32_t)LOG_FILE_MAX_LEN - size);
    if (rotate)
        idx = (uint8_t)(idx ^ 1u);

    /* 4) 打开 → 写 → 关闭 (close 即提交落盘, 不依赖后端 close 之外的 sync) */
    const vfs_fd_t fd = vfs_open(s_log_paths[idx],
                                 VFS_O_WRONLY | VFS_O_CREAT |
                                     (rotate ? VFS_O_TRUNC : VFS_O_APPEND));
    if (fd < 0)
    {
        s_rotate.last_fail_ms = now; /* 未挂载/句柄池满/只读 → 下个窗口再试 */
        return;
    }
    s_rotate.last_fail_ms = 0;

    (void)vfs_write(fd, data, len); /* 短写/写失败都到此为止 */
    (void)vfs_close(fd);

    /* 5) 与盘上状态对齐: 此后 cur 只在轮换时推进。写成功才坐实 —— 挂载前
          stat 拿不到长度, 那时推导出的 cur 不算数 */
    s_rotate.cur = idx;
    s_rotate.cur_known = true;
}

#endif /* LOG_FILE_ENABLE */
