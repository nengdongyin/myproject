/**
 * @file vfs_core.c
 * @brief VFS 核心 —— 注册表 / 挂载 / 路径解析 / 句柄池 / 分派
 *
 * 本文件是 VFS 的中枢，但与具体文件系统完全解耦：只认 @ref vfs_fs_ops_t。
 *
 * @section flow open 流程
 * @code
 *   vfs_open("/app/foo.txt")
 *     → vfs_resolve()  最长前缀匹配挂载点 "/app"，得相对路径 "foo.txt"
 *     → caps/RO 前置校验（不支持 → -ENOTSUP / 只读 → -EROFS）
 *     → 分配句柄槽 + 加挂载点锁
 *     → backend->open(file, "foo.txt", flags)
 * @endcode
 *
 * @section error_contract 不支持的操作
 * 可选回调为 NULL，或 caps 缺位（如 rawflash 的 APPEND）时，core 在调用
 * backend **之前**统一返回 -ENOTSUP，无需各 backend 自查。
 */

#include "vfs.h"
#include <string.h>
#include <errno.h>

#if VFS_ENABLE_MP_LOCK
#include "port.h"
#define VFS_LOCK_CREATE()   ((void *)sys_mutex_create())
#define VFS_LOCK_DESTROY(l) sys_mutex_destroy((sys_mutex_t *)(l))
#define VFS_LOCK(l)         sys_mutex_lock((sys_mutex_t *)(l))
#define VFS_UNLOCK(l)       sys_mutex_unlock((sys_mutex_t *)(l))
#else
#define VFS_LOCK_CREATE()   ((void *)0)
#define VFS_LOCK_DESTROY(l) ((void)0)
#define VFS_LOCK(l)         ((void)0)
#define VFS_UNLOCK(l)       ((void)0)
#endif

/* ================================================================
 *  内部数据结构
 * ================================================================ */

/** @brief 句柄种类：文件 */
#define VFS_KIND_FILE 0
/** @brief 句柄种类：目录 */
#define VFS_KIND_DIR  1

/** @brief 目录 fd 的高位标志（与文件池下标隔离） */
#define VFS_FD_DIR_FLAG 0x4000

/** @brief 一个打开句柄（文件或目录） */
typedef struct vfs_handle
{
    uint8_t used;             /**< 槽位占用标记 */
    uint8_t kind;             /**< VFS_KIND_FILE / VFS_KIND_DIR */
    int oflags;               /**< 打开时的 VFS_O_* 原始标志 */
    int flock_slot;           /**< 同文件锁槽(-1=无)，见 VFS_ENABLE_FILE_LOCK */
    const vfs_fs_ops_t *fs;   /**< 所属后端虚函数表 */
    vfs_mount_t *mp;          /**< 所属挂载点 */
    vfs_file_t fobj;          /**< 文件会话对象（kind=FILE 时有效） */
    vfs_dir_t dobj;           /**< 目录会话对象（kind=DIR 时有效） */
} vfs_handle_t;

/** @brief 文件系统类型注册项 */
typedef struct
{
    uint8_t used;              /**< 槽位占用标记 */
    uint8_t fstype;            /**< VFS_FSTYPE_* 类型 ID */
    const vfs_fs_ops_t *ops;   /**< 后端虚函数表指针 */
} vfs_reg_entry_t;

/** @brief 块设备注册项 */
typedef struct
{
    const char *name;          /**< 块设备全局唯一名字 */
    const vfs_bdev_t *bdev;    /**< 分区切片指针 */
} vfs_bdev_entry_t;

/* ---- 全局状态（全部静态池，零堆） ---- */
/** @brief 文件系统类型注册表（静态池） */
static vfs_reg_entry_t g_registry[VFS_MAX_TYPES];
/** @brief 块设备注册表（静态池） */
static vfs_bdev_entry_t g_bdevs[VFS_MAX_BDEVS];
/** @brief 已注册块设备个数 */
static int g_bdev_n;
/** @brief 挂载点表（静态池） */
static vfs_mount_t g_mounts[VFS_MAX_MOUNTS];
/** @brief 文件句柄池（静态池，零堆） */
static vfs_handle_t g_files[VFS_MAX_OPEN_FILES];
/** @brief 目录句柄池（静态池，零堆） */
static vfs_handle_t g_dirs[VFS_MAX_OPEN_DIRS];
/** @brief 核心锁：保护注册表/句柄池的短临界区 */
static void *g_core_lock;
/** @brief 已初始化标志（vfs_init 幂等判断） */
static int g_inited;

/* ================================================================
 *  内部辅助
 * ================================================================ */

/** @brief 在类型注册表中按 ID 查找后端虚函数表 */
static const vfs_fs_ops_t *registry_find(uint8_t fstype)
{
    for (int i = 0; i < VFS_MAX_TYPES; i++)
    {
        if (g_registry[i].used && g_registry[i].fstype == fstype)
            return g_registry[i].ops;
    }
    return NULL;
}

/** @brief 在块设备注册表中按名字查找分区切片 */
static const vfs_bdev_t *bdev_find(const char *name)
{
    if (!name)
        return NULL;
    for (int i = 0; i < g_bdev_n; i++)
    {
        if (strcmp(g_bdevs[i].name, name) == 0)
            return g_bdevs[i].bdev;
    }
    return NULL;
}

/**
 * @brief 由句柄指针编码出 vfs_fd_t
 * @details 文件 fd = 文件池下标；目录 fd = 目录池下标 | VFS_FD_DIR_FLAG。
 *          这样 handle_get 仅凭 fd 即可区分句柄种类并做越界检查。
 */
static vfs_fd_t fd_of(const vfs_handle_t *h)
{
    if (h->kind == VFS_KIND_DIR)
        return (vfs_fd_t)(VFS_FD_DIR_FLAG | (uint32_t)(h - g_dirs));
    return (vfs_fd_t)(h - g_files);
}

/**
 * @brief 校验并取回 fd 对应的句柄
 * @param fd        vfs_fd_t 描述符
 * @param want_kind 期望的句柄种类（VFS_KIND_FILE/VFS_KIND_DIR）
 * @return 有效句柄指针；种类不匹配/越界/未占用返回 NULL
 */
static vfs_handle_t *handle_get(vfs_fd_t fd, uint8_t want_kind)
{
    if (fd < 0)
        return NULL;
    if (fd & VFS_FD_DIR_FLAG)
    {
        uint32_t idx = (uint32_t)fd & ~(uint32_t)VFS_FD_DIR_FLAG;
        if (want_kind != VFS_KIND_DIR || idx >= VFS_MAX_OPEN_DIRS)
            return NULL;
        vfs_handle_t *h = &g_dirs[idx];
        return (h->used && h->kind == VFS_KIND_DIR) ? h : NULL;
    }
    uint32_t idx = (uint32_t)fd;
    if (want_kind != VFS_KIND_FILE || idx >= VFS_MAX_OPEN_FILES)
        return NULL;
    vfs_handle_t *h = &g_files[idx];
    return (h->used && h->kind == VFS_KIND_FILE) ? h : NULL;
}

/**
 * @brief 在对应句柄池中分配一个空闲槽（返回前已置 used=1 并清零）
 * @param kind VFS_KIND_FILE / VFS_KIND_DIR
 * @return 分配成功返回句柄指针；池满返回 NULL
 */
static vfs_handle_t *handle_alloc(uint8_t kind)
{
    vfs_handle_t *pool = (kind == VFS_KIND_DIR) ? g_dirs : g_files;
    int n = (kind == VFS_KIND_DIR) ? VFS_MAX_OPEN_DIRS : VFS_MAX_OPEN_FILES;

    VFS_LOCK(g_core_lock);
    for (int i = 0; i < n; i++)
    {
        if (!pool[i].used)
        {
            memset(&pool[i], 0, sizeof(pool[i]));
            pool[i].used = 1;
            pool[i].kind = kind;
            VFS_UNLOCK(g_core_lock);
            return &pool[i];
        }
    }
    VFS_UNLOCK(g_core_lock);
    return NULL;
}

/** @brief 释放句柄槽（清零后归还池） */
static void handle_free(vfs_handle_t *h)
{
    if (!h)
        return;
    VFS_LOCK(g_core_lock);
    memset(h, 0, sizeof(*h));
    VFS_UNLOCK(g_core_lock);
}

/* ================================================================
 *  同文件独占锁（VFS_ENABLE_FILE_LOCK）
 *  仅对"写意图"打开登记 (挂载点, 相对路径)；第二条写打开返回 -EBUSY。
 * ================================================================ */
#if VFS_ENABLE_FILE_LOCK
typedef struct
{
    uint8_t used;          /**< 锁槽占用标记 */
    vfs_mount_t *mp;       /**< 锁定的挂载点 */
    char rel[VFS_PATH_MAX]; /**< 挂载点内相对路径（锁的键） */
} vfs_flock_t;

/** @brief 同文件独占锁池（静态池，仅 VFS_ENABLE_FILE_LOCK=1 时存在） */
static vfs_flock_t g_flocks[VFS_MAX_OPEN_FILES];

/**
 * @brief 登记同文件独占锁（写意图打开时调用）
 * @details 两遍扫描：先查 (mp, rel) 是否已被锁定（冲突返回 -EBUSY），
 *          再找空闲槽登记。锁以 (挂载点, 相对路径) 为键，跨后端统一生效。
 * @param mp       挂载点
 * @param rel      挂载点内相对路径
 * @param slot_out 输出分配的锁槽下标
 * @return 0 成功；-EBUSY 冲突；-ENFILE 锁池满
 */
static int flock_acquire(vfs_mount_t *mp, const char *rel, int *slot_out)
{
    VFS_LOCK(g_core_lock);
    for (int i = 0; i < VFS_MAX_OPEN_FILES; i++)
    {
        if (g_flocks[i].used && g_flocks[i].mp == mp && strcmp(g_flocks[i].rel, rel) == 0)
        {
            VFS_UNLOCK(g_core_lock);
            return -EBUSY;
        }
    }
    for (int i = 0; i < VFS_MAX_OPEN_FILES; i++)
    {
        if (!g_flocks[i].used)
        {
            g_flocks[i].used = 1;
            g_flocks[i].mp = mp;
            strncpy(g_flocks[i].rel, rel, VFS_PATH_MAX - 1);
            g_flocks[i].rel[VFS_PATH_MAX - 1] = '\0';
            VFS_UNLOCK(g_core_lock);
            *slot_out = i;
            return 0;
        }
    }
    VFS_UNLOCK(g_core_lock);
    return -ENFILE;
}

/** @brief 释放一个同文件锁槽 */
static void flock_release(int slot)
{
    VFS_LOCK(g_core_lock);
    if (slot >= 0 && slot < VFS_MAX_OPEN_FILES)
        g_flocks[slot].used = 0;
    VFS_UNLOCK(g_core_lock);
}

/** @brief 释放句柄持有的同文件锁槽（若存在） */
static void flock_release_handle(vfs_handle_t *h)
{
    if (h->flock_slot >= 0)
        flock_release(h->flock_slot);
    h->flock_slot = -1;
}

#define FLOCK_RELEASE(h) flock_release_handle(h)
#else
#define FLOCK_RELEASE(h) ((void)(h))
#endif /* VFS_ENABLE_FILE_LOCK */

/**
 * @brief 路径解析：最长前缀匹配挂载点
 * @details 遍历所有已用挂载点，取能覆盖 path 且前缀最长的那个；同时做边界
 *          检查（前缀后必须是 '/' 或 '\0'），避免 "/app2" 误配 "/app"。
 *
 * @param path    绝对路径（以 '/' 开头）
 * @param out_mp  输出命中的挂载点
 * @param out_rel 输出挂载点内相对路径（去掉前缀与紧随的 '/'）
 * @return 0 成功，-EINVAL 路径非绝对，-ENOENT 无匹配挂载点
 */
static int vfs_resolve(const char *path, vfs_mount_t **out_mp, const char **out_rel)
{
    if (!path || path[0] != '/')
        return -EINVAL;

    vfs_mount_t *best = NULL;
    size_t best_len = 0;

    for (int i = 0; i < VFS_MAX_MOUNTS; i++)
    {
        vfs_mount_t *mp = &g_mounts[i];
        if (!mp->used)
            continue;

        size_t l = mp->mnt_len;
        if (l < best_len || l == 0)
            continue;
        if (strncmp(path, mp->mnt_point, l) != 0)
            continue;
        /* 边界检查：前缀之后必须是 '/' 或字符串结束，避免 "/app2" 误配 "/app" */
        char c = path[l];
        if (c != '\0' && c != '/')
            continue;

        best = mp;
        best_len = l;
    }

    if (!best)
        return -ENOENT;

    const char *rel = path + best_len;
    if (*rel == '/')
        rel++; /* 跳过分隔符，得到挂载点内相对路径 */

    *out_mp = best;
    *out_rel = rel;
    return 0;
}

/** @brief 加每挂载点锁（mp->lock），串行化同一卷的后端调用 */
static void mp_lock(vfs_mount_t *mp)
{
    VFS_LOCK(mp->lock);
}

/** @brief 释放每挂载点锁 */
static void mp_unlock(vfs_mount_t *mp)
{
    VFS_UNLOCK(mp->lock);
}

/** @brief 判断打开标志是否含"写意图"（O_WRONLY/O_RDWR） */
static int flags_is_write(int flags)
{
    int acc = flags & 0x3; /* 0=RDONLY 1=WRONLY 2=RDWR */
    return (acc == VFS_O_WRONLY) || (acc == VFS_O_RDWR);
}

/**
 * @brief 挂载单个卷（内部实现，静态表与动态挂载共用）
 * @details 流程：校验挂载点并规范化尾部 '/' → 查后端注册表 → 持核心锁
 *          查重 + 分配挂载槽 → 绑定 bdev 与后端配置 → 建每挂载点锁 →
 *          调用后端 mount() 填充 fs_ctx。任一步失败即回滚已分配资源。
 * @param cfg 挂载配置（不能为 NULL）
 * @return 0 成功；-EINVAL 参数错，-EEXIST 挂载点重复，-ENOSPC 槽/锁耗尽，
 *         -ENOTSUP 类型未注册，< 0 后端 mount 失败的错误码
 */
static int mount_one(const vfs_mount_cfg_t *cfg)
{
    if (!cfg || !cfg->mnt_point || cfg->mnt_point[0] != '/')
        return -EINVAL;

    size_t plen = strlen(cfg->mnt_point);
    if (plen == 0 || plen >= VFS_PATH_MAX)
        return -EINVAL;

    /* 规范化挂载点：去除尾部 '/' */
    char norm[VFS_PATH_MAX];
    memcpy(norm, cfg->mnt_point, plen + 1);
    while (plen > 1 && norm[plen - 1] == '/')
        norm[--plen] = '\0';

    const vfs_fs_ops_t *ops = registry_find(cfg->fstype);
    if (!ops || !ops->open)
        return -ENOTSUP; /* 文件系统类型未注册 / 不是有效后端 */

    /* 分配挂载槽（持核心锁，并顺带查重） */
    vfs_mount_t *mp = NULL;
    VFS_LOCK(g_core_lock);
    for (int i = 0; i < VFS_MAX_MOUNTS; i++)
    {
        if (g_mounts[i].used && g_mounts[i].mnt_len == plen &&
            strncmp(g_mounts[i].mnt_point, norm, plen) == 0)
        {
            VFS_UNLOCK(g_core_lock);
            return -EEXIST; /* 挂载点重复 */
        }
    }
    for (int i = 0; i < VFS_MAX_MOUNTS; i++)
    {
        if (!g_mounts[i].used)
        {
            mp = &g_mounts[i];
            break;
        }
    }
    if (mp)
    {
        memset(mp, 0, sizeof(*mp));
        mp->used = 1;
    }
    VFS_UNLOCK(g_core_lock);
    if (!mp)
        return -ENOSPC;

    memcpy(mp->mnt_point, norm, plen + 1);
    mp->mnt_len = plen;
    mp->fstype = cfg->fstype;
    mp->fs = ops;
    mp->flags = cfg->flags;
    mp->fs_ctx = (void *)cfg->fs_data; /* backend 可在 mount() 覆盖 */
    mp->storage_dev = bdev_find(cfg->bdev_name);

    /* 每挂载点锁 */
#if VFS_ENABLE_MP_LOCK
    mp->lock = VFS_LOCK_CREATE();
    if (!mp->lock)
    {
        mp->used = 0;
        return -ENOSPC;
    }
#endif

    /* 调后端 mount（可选） */
    if (ops->mount)
    {
        int rc = ops->mount(mp);
        if (rc < 0)
        {
            VFS_LOCK_DESTROY(mp->lock);
            mp->used = 0;
            return rc;
        }
    }
    return 0;
}

/* ================================================================
 *  生命周期
 * ================================================================ */

int vfs_init(void)
{
    if (g_inited)
        return 0;

    memset(g_registry, 0, sizeof(g_registry));
    memset(g_bdevs, 0, sizeof(g_bdevs));
    memset(g_mounts, 0, sizeof(g_mounts));
    memset(g_files, 0, sizeof(g_files));
    memset(g_dirs, 0, sizeof(g_dirs));
#if VFS_ENABLE_FILE_LOCK
    memset(g_flocks, 0, sizeof(g_flocks));
#endif
    g_bdev_n = 0;

    g_core_lock = VFS_LOCK_CREATE();
#if VFS_ENABLE_MP_LOCK
    if (!g_core_lock)
        return -ENOSPC; /* 锁池耗尽：不进入已初始化状态 */
#endif
    g_inited = 1;
    return 0;
}

/**
 * @brief 反初始化 VFS（关闭所有打开句柄、卸载所有挂载点）
 * @details 严格按"先句柄、后挂载"的顺序回收：先关闭文件并释放文件锁，再
 *          关闭目录，然后逐个调用后端 unmount 释放 fs_ctx，最后销毁所有锁
 *          并复位全部静态池。顺序不能颠倒，否则后端 close/unmount 可能访问
 *          已被释放的 mp/fs_ctx。
 */
void vfs_deinit(void)
{
    int i;

    /* 关闭所有打开句柄 */
    for (i = 0; i < VFS_MAX_OPEN_FILES; i++)
    {
        if (g_files[i].used)
        {
            if (g_files[i].fs && g_files[i].fs->close)
                g_files[i].fs->close(&g_files[i].fobj);
            FLOCK_RELEASE(&g_files[i]);
        }
    }
#if VFS_ENABLE_DIR
    for (i = 0; i < VFS_MAX_OPEN_DIRS; i++)
    {
        if (g_dirs[i].used && g_dirs[i].fs && g_dirs[i].fs->closedir)
            g_dirs[i].fs->closedir(&g_dirs[i].dobj);
    }
#endif
    memset(g_files, 0, sizeof(g_files));
    memset(g_dirs, 0, sizeof(g_dirs));

    /* 卸载所有挂载点 */
    for (i = 0; i < VFS_MAX_MOUNTS; i++)
    {
        vfs_mount_t *mp = &g_mounts[i];
        if (!mp->used)
            continue;
        if (mp->fs && mp->fs->unmount)
            mp->fs->unmount(mp);
        VFS_LOCK_DESTROY(mp->lock);
    }
    memset(g_mounts, 0, sizeof(g_mounts));
    memset(g_registry, 0, sizeof(g_registry));
    memset(g_bdevs, 0, sizeof(g_bdevs));
    g_bdev_n = 0;

    VFS_LOCK_DESTROY(g_core_lock);
    g_core_lock = NULL;
    g_inited = 0;
}

int vfs_register(uint8_t fstype, const vfs_fs_ops_t *ops)
{
    if (!ops)
        return -EINVAL;

    VFS_LOCK(g_core_lock);
    for (int i = 0; i < VFS_MAX_TYPES; i++)
    {
        if (g_registry[i].used && g_registry[i].fstype == fstype)
        {
            VFS_UNLOCK(g_core_lock);
            return -EALREADY;
        }
    }
    for (int i = 0; i < VFS_MAX_TYPES; i++)
    {
        if (!g_registry[i].used)
        {
            g_registry[i].used = 1;
            g_registry[i].fstype = fstype;
            g_registry[i].ops = ops;
            VFS_UNLOCK(g_core_lock);
            return 0;
        }
    }
    VFS_UNLOCK(g_core_lock);
    return -ENOSPC;
}

int vfs_bdev_register(const char *name, const vfs_bdev_t *bdev)
{
    if (!name || !bdev)
        return -EINVAL;

    VFS_LOCK(g_core_lock);
    for (int i = 0; i < g_bdev_n; i++)
    {
        if (strcmp(g_bdevs[i].name, name) == 0)
        {
            VFS_UNLOCK(g_core_lock);
            return -EEXIST; /* 重名块设备 */
        }
    }
    if (g_bdev_n >= VFS_MAX_BDEVS)
    {
        VFS_UNLOCK(g_core_lock);
        return -ENOSPC;
    }
    g_bdevs[g_bdev_n].name = name;
    g_bdevs[g_bdev_n].bdev = bdev;
    g_bdev_n++;
    VFS_UNLOCK(g_core_lock);
    return 0;
}

int vfs_mount_table(const vfs_mount_cfg_t *tbl, size_t n)
{
    if (!tbl)
        return -EINVAL;

    int last = 0;
    for (size_t i = 0; i < n; i++)
    {
        int rc = mount_one(&tbl[i]);
        if (rc < 0)
            last = rc; /* 记录，但继续尝试其余（部分失败不致命） */
    }
    return last;
}

#if VFS_ENABLE_DYNAMIC_MOUNT
int vfs_mount(const vfs_mount_cfg_t *cfg)
{
    return mount_one(cfg);
}

int vfs_unmount(const char *mnt_point)
{
    if (!mnt_point)
        return -EINVAL;

    VFS_LOCK(g_core_lock);
    vfs_mount_t *mp = NULL;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++)
    {
        if (g_mounts[i].used && strcmp(g_mounts[i].mnt_point, mnt_point) == 0)
        {
            mp = &g_mounts[i];
            break;
        }
    }
    VFS_UNLOCK(g_core_lock);
    if (!mp)
        return -ENOENT;

    /* 仍有打开句柄则拒绝卸载，避免悬垂 mp/fs_ctx */
    VFS_LOCK(g_core_lock);
    for (int i = 0; i < VFS_MAX_OPEN_FILES; i++)
        if (g_files[i].used && g_files[i].mp == mp)
        {
            VFS_UNLOCK(g_core_lock);
            return -EBUSY;
        }
#if VFS_ENABLE_DIR
    for (int i = 0; i < VFS_MAX_OPEN_DIRS; i++)
        if (g_dirs[i].used && g_dirs[i].mp == mp)
        {
            VFS_UNLOCK(g_core_lock);
            return -EBUSY;
        }
#endif
    VFS_UNLOCK(g_core_lock);

    if (mp->fs && mp->fs->unmount)
    {
        int rc = mp->fs->unmount(mp);
        if (rc < 0)
            return rc;
    }
    VFS_LOCK_DESTROY(mp->lock);
    VFS_LOCK(g_core_lock);
    memset(mp, 0, sizeof(*mp));
    VFS_UNLOCK(g_core_lock);
    return 0;
}
#else /* !VFS_ENABLE_DYNAMIC_MOUNT */
int vfs_mount(const vfs_mount_cfg_t *cfg)
{
    (void)cfg;
    return -ENOTSUP;
}

int vfs_unmount(const char *mnt_point)
{
    (void)mnt_point;
    return -ENOTSUP;
}
#endif /* VFS_ENABLE_DYNAMIC_MOUNT */

const vfs_fs_ops_t *vfs_fstype_lookup(uint8_t fstype)
{
    return registry_find(fstype);
}

/* ================================================================
 *  文件操作
 * ================================================================ */

vfs_fd_t vfs_open(const char *path, int flags)
{
    /* 访问模式合法性：0/1/2 合法，3 非法 */
    if ((flags & 0x3) == 0x3)
        return -EINVAL;
    /* 截断必须带写意图，避免 O_RDONLY|O_TRUNC 触发后端擦除 */
    if ((flags & VFS_O_TRUNC) && (flags & 0x3) == VFS_O_RDONLY)
        return -EINVAL;

    vfs_mount_t *mp;
    const char *rel;
    int rc = vfs_resolve(path, &mp, &rel);
    if (rc < 0)
        return rc;

    const vfs_fs_ops_t *fs = mp->fs;
    if (!fs || !fs->open)
        return -ENOTSUP;

    /* 只读挂载点上的写意图 → -EROFS */
    if ((mp->flags & VFS_MNT_RO) &&
        (flags_is_write(flags) || (flags & (VFS_O_CREAT | VFS_O_TRUNC))))
        return -EROFS;

    /* caps 前置校验：不支持的能力直接返回 -ENOTSUP */
    if ((flags & VFS_O_APPEND) && !(fs->caps & VFS_CAP_APPEND))
        return -ENOTSUP;
    if ((flags & VFS_O_TRUNC) && !(fs->caps & VFS_CAP_TRUNCATE))
        return -ENOTSUP;

    vfs_handle_t *h = handle_alloc(VFS_KIND_FILE);
    if (!h)
        return -ENFILE;

    h->fs = fs;
    h->mp = mp;
    h->oflags = flags;
    h->flock_slot = -1;
    h->fobj.mp = mp;
    h->fobj.priv = NULL;

#if VFS_ENABLE_FILE_LOCK
    if (flags_is_write(flags) || (flags & (VFS_O_TRUNC | VFS_O_CREAT)))
    {
        int slot = -1;
        rc = flock_acquire(mp, rel, &slot);
        if (rc < 0)
        {
            handle_free(h);
            return rc;
        }
        h->flock_slot = slot;
    }
#endif

    mp_lock(mp);
    rc = fs->open(&h->fobj, rel, flags);
    mp_unlock(mp);

    if (rc < 0)
    {
        FLOCK_RELEASE(h);
        handle_free(h);
        return rc;
    }
    return fd_of(h);
}

int vfs_close(vfs_fd_t fd)
{
    vfs_handle_t *h = handle_get(fd, VFS_KIND_FILE);
    if (!h)
        return -EBADF;

    int rc = 0;
    if (h->fs && h->fs->close)
    {
        mp_lock(h->mp);
        rc = h->fs->close(&h->fobj);
        mp_unlock(h->mp);
    }
    FLOCK_RELEASE(h);
    handle_free(h);
    return rc;
}

ssize_t vfs_read(vfs_fd_t fd, void *buf, size_t n)
{
    vfs_handle_t *h = handle_get(fd, VFS_KIND_FILE);
    if (!h)
        return -EBADF;
    if (!h->fs->read)
        return -ENOTSUP;

    mp_lock(h->mp);
    ssize_t rc = h->fs->read(&h->fobj, buf, n);
    mp_unlock(h->mp);
    return rc;
}

ssize_t vfs_write(vfs_fd_t fd, const void *buf, size_t n)
{
    vfs_handle_t *h = handle_get(fd, VFS_KIND_FILE);
    if (!h)
        return -EBADF;
    if (!h->fs->write)
        return -ENOTSUP;

    mp_lock(h->mp);
    ssize_t rc = h->fs->write(&h->fobj, buf, n);
    mp_unlock(h->mp);
    return rc;
}

off_t vfs_lseek(vfs_fd_t fd, off_t off, int whence)
{
    vfs_handle_t *h = handle_get(fd, VFS_KIND_FILE);
    if (!h)
        return -EBADF;
    if (!h->fs->lseek)
        return -ENOTSUP;

    mp_lock(h->mp);
    off_t rc = h->fs->lseek(&h->fobj, off, whence);
    mp_unlock(h->mp);
    return rc;
}

off_t vfs_tell(vfs_fd_t fd)
{
    vfs_handle_t *h = handle_get(fd, VFS_KIND_FILE);
    if (!h)
        return -EBADF;
    if (!h->fs->tell)
        return -ENOTSUP;

    mp_lock(h->mp);
    off_t rc = h->fs->tell(&h->fobj);
    mp_unlock(h->mp);
    return rc;
}

int vfs_sync(vfs_fd_t fd)
{
    vfs_handle_t *h = handle_get(fd, VFS_KIND_FILE);
    if (!h)
        return -EBADF;
    if (!h->fs->sync)
        return 0; /* 无 sync 视为已同步 */

    mp_lock(h->mp);
    int rc = h->fs->sync(&h->fobj);
    mp_unlock(h->mp);
    return rc;
}

ssize_t vfs_pread(vfs_fd_t fd, void *buf, size_t n, off_t off)
{
    vfs_handle_t *h = handle_get(fd, VFS_KIND_FILE);
    if (!h)
        return -EBADF;
    if (!h->fs->lseek || !h->fs->read)
        return -ENOTSUP;

    mp_lock(h->mp);
    off_t seek_rc = h->fs->lseek(&h->fobj, off, VFS_SEEK_SET);
    if (seek_rc < 0)
    {
        mp_unlock(h->mp);
        return seek_rc;
    }
    ssize_t rc = h->fs->read(&h->fobj, buf, n);
    mp_unlock(h->mp);
    return rc;
}

/* ================================================================
 *  文件系统级操作
 * ================================================================ */

int vfs_stat(const char *path, vfs_stat_t *st)
{
    if (!st)
        return -EINVAL;

    vfs_mount_t *mp;
    const char *rel;
    int rc = vfs_resolve(path, &mp, &rel);
    if (rc < 0)
        return rc;

    /* 挂载点根目录本身 */
    if (rel[0] == '\0')
    {
        st->type = VFS_TYPE_DIR;
        st->size = 0;
        return 0;
    }

    if (!mp->fs || !mp->fs->stat)
        return -ENOTSUP;

    mp_lock(mp);
    rc = mp->fs->stat(mp, rel, st);
    mp_unlock(mp);
    return rc;
}

int vfs_unlink(const char *path)
{
    vfs_mount_t *mp;
    const char *rel;
    int rc = vfs_resolve(path, &mp, &rel);
    if (rc < 0)
        return rc;
    if (rel[0] == '\0')
        return -EINVAL; /* 不允许删挂载点根 */
    if (mp->flags & VFS_MNT_RO)
        return -EROFS;
    if (!mp->fs || !mp->fs->unlink)
        return -ENOTSUP;

    mp_lock(mp);
    rc = mp->fs->unlink(mp, rel);
    mp_unlock(mp);
    return rc;
}

int vfs_mkdir(const char *path)
{
    vfs_mount_t *mp;
    const char *rel;
    int rc = vfs_resolve(path, &mp, &rel);
    if (rc < 0)
        return rc;
    if (rel[0] == '\0')
        return -EINVAL;
    if (mp->flags & VFS_MNT_RO)
        return -EROFS;
    if (!mp->fs || !mp->fs->mkdir)
        return -ENOTSUP;

    mp_lock(mp);
    rc = mp->fs->mkdir(mp, rel);
    mp_unlock(mp);
    return rc;
}

#if VFS_ENABLE_RENAME
int vfs_rename(const char *from, const char *to)
{
    if (!from || !to)
        return -EINVAL;

    vfs_mount_t *mp;
    const char *rel_from;
    int rc = vfs_resolve(from, &mp, &rel_from);
    if (rc < 0)
        return rc;

    /* 目标必须落在同一挂载点（相对路径同前缀） */
    const char *rel_to;
    vfs_mount_t *mp_to;
    rc = vfs_resolve(to, &mp_to, &rel_to);
    if (rc < 0)
        return rc;
    if (mp_to != mp)
        return -EINVAL; /* 不支持跨挂载点移动 */
    if (mp->flags & VFS_MNT_RO)
        return -EROFS;
    if (!mp->fs || !mp->fs->rename)
        return -ENOTSUP;

    mp_lock(mp);
    rc = mp->fs->rename(mp, rel_from, rel_to);
    mp_unlock(mp);
    return rc;
}
#endif /* VFS_ENABLE_RENAME */

#if VFS_ENABLE_STATVFS
int vfs_statvfs(const char *path, vfs_statvfs_t *st)
{
    if (!st)
        return -EINVAL;

    vfs_mount_t *mp;
    const char *rel;
    int rc = vfs_resolve(path, &mp, &rel);
    if (rc < 0)
        return rc;
    if (!mp->fs || !mp->fs->statvfs)
        return -ENOTSUP;

    mp_lock(mp);
    rc = mp->fs->statvfs(mp, st);
    mp_unlock(mp);
    return rc;
}
#endif /* VFS_ENABLE_STATVFS */

/* ================================================================
 *  目录操作
 * ================================================================ */
#if VFS_ENABLE_DIR

vfs_fd_t vfs_opendir(const char *path)
{
    vfs_mount_t *mp;
    const char *rel;
    int rc = vfs_resolve(path, &mp, &rel);
    if (rc < 0)
        return rc;
    if (!mp->fs || !mp->fs->opendir)
        return -ENOTSUP;

    vfs_handle_t *h = handle_alloc(VFS_KIND_DIR);
    if (!h)
        return -ENFILE;

    h->fs = mp->fs;
    h->mp = mp;
    h->dobj.mp = mp;
    h->dobj.priv = NULL;

    mp_lock(mp);
    rc = mp->fs->opendir(&h->dobj, rel);
    mp_unlock(mp);
    if (rc < 0)
    {
        handle_free(h);
        return rc;
    }
    return fd_of(h);
}

int vfs_readdir(vfs_fd_t fd, vfs_dirent_t *ent)
{
    vfs_handle_t *h = handle_get(fd, VFS_KIND_DIR);
    if (!h || !ent)
        return -EBADF;
    if (!h->fs->readdir)
        return -ENOTSUP;

    mp_lock(h->mp);
    int rc = h->fs->readdir(&h->dobj, ent);
    mp_unlock(h->mp);
    return rc;
}

int vfs_closedir(vfs_fd_t fd)
{
    vfs_handle_t *h = handle_get(fd, VFS_KIND_DIR);
    if (!h)
        return -EBADF;

    int rc = 0;
    if (h->fs && h->fs->closedir)
    {
        mp_lock(h->mp);
        rc = h->fs->closedir(&h->dobj);
        mp_unlock(h->mp);
    }
    handle_free(h);
    return rc;
}

#endif /* VFS_ENABLE_DIR */
