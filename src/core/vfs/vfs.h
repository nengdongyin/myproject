/**
 * @file vfs.h
 * @brief 嵌入式轻量级虚拟文件系统 —— 公共接口
 *
 * 设计要点：
 *  - **POSIX 子集**：open/close/read/write/lseek/... 语义对齐，返回负 errno。
 *  - **RTOS 无关**：仅依赖项目 port 层（system_malloc / sys_mutex），不绑定任何 RTOS。
 *  - **开闭原则**：核心只认 @ref vfs_fs_ops_t 虚函数表。新增文件系统 =
 *    新增一个后端 .c + 注册一行，核心零改动。
 *  - **挂载点命名空间**：路径 → 挂载点（最长前缀）→ 后端实例。分区归属由
 *    挂载绑定决定，不在打开时解析路径。
 *  - **句柄池**：静态池，零堆。
 *
 * @section errno_contract 返回码约定
 * 所有以 int 返回的 API：>= 0 成功（读/写返回字节数），< 0 为负 errno：
 *  - @c -ENOENT  文件/分区/挂载点不存在
 *  - @c -ENOTSUP 该文件系统不支持此操作，或功能被编译关闭
 *  - @c -ENOSPC  空间不足 / 挂载槽或锁池耗尽
 *  - @c -ENFILE  打开文件/目录数超过 VFS_MAX_OPEN_FILES / VFS_MAX_OPEN_DIRS
 *  - @c -EINVAL  参数非法
 *  - @c -EIO     硬件 I/O 错误
 *  - @c -EROFS   写只读挂载点
 *  - @c -EBUSY   同文件独占锁冲突
 *  - @c -EEXIST  O_EXCL 且文件已存在
 *  - @c -EBADF   对已关闭/非法句柄操作
 *
 * @section usage 使用示例
 * @code
 *   vfs_init();
 *   vfs_register(VFS_FSTYPE_LITTLEFS, &vfs_littlefs_ops);
 *   vfs_bdev_fal_init(&bd, "flash_app", "app");
 *   vfs_bdev_register("flash_app", &bd);
 *   static const vfs_mount_cfg_t mnt[] = {
 *       { "/app", VFS_FSTYPE_LITTLEFS, "flash_app", 0, NULL },
 *   };
 *   vfs_mount_table(mnt, 1);
 *
 *   vfs_fd_t fd = vfs_open("/app/foo.bin", VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
 *   vfs_write(fd, buf, len);
 *   vfs_close(fd);
 * @endcode
 */

#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h> /* off_t, ssize_t */
#include "vfs_config.h"
#include "vfs_bdev.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* ================================================================
 *  打开标志（与 newlib O_* 数值对齐，便于 stdio 适配）
 * ================================================================ */
#define VFS_O_RDONLY 0x0000 /**< 只读 */
#define VFS_O_WRONLY 0x0001 /**< 只写 */
#define VFS_O_RDWR   0x0002 /**< 读写 */
#define VFS_O_CREAT  0x0040 /**< 不存在则创建 */
#define VFS_O_EXCL   0x0080 /**< 与 O_CREAT 合用，存在则失败(-EEXIST) */
#define VFS_O_TRUNC  0x0200 /**< 打开即截断/擦除 */
#define VFS_O_APPEND 0x0400 /**< 追加写（rawflash 不支持） */

/* ================================================================
 *  定位原点
 * ================================================================ */
/** @brief 定位原点：从文件头开始 */
#define VFS_SEEK_SET 0
/** @brief 定位原点：从当前偏移开始 */
#define VFS_SEEK_CUR 1
/** @brief 定位原点：从文件尾开始 */
#define VFS_SEEK_END 2

    /** @brief 文件描述符类型（句柄池下标），失败为负 errno */
    typedef int32_t vfs_fd_t;

/** @brief 非法 fd 哨兵 */
#define VFS_FD_INVALID (-1)

    /** @brief 文件/目录类型 */
    typedef enum
    {
        VFS_TYPE_FILE = 0, /**< 普通文件 */
        VFS_TYPE_DIR = 1   /**< 目录 */
    } vfs_type_t;

    /** @brief stat 结果 */
    typedef struct vfs_stat
    {
        uint8_t type;   /**< VFS_TYPE_FILE / VFS_TYPE_DIR */
        uint32_t size;  /**< 文件字节数（目录为 0） */
    } vfs_stat_t;

    /** @brief 目录项 */
    typedef struct vfs_dirent
    {
        uint8_t type;                /**< VFS_TYPE_FILE / VFS_TYPE_DIR */
        char name[VFS_NAME_MAX + 1]; /**< 条目名 */
        uint32_t size;               /**< 文件字节数 */
    } vfs_dirent_t;

    /** @brief 卷容量统计 */
    typedef struct vfs_statvfs
    {
        uint32_t total_bytes;
        uint32_t free_bytes;
    } vfs_statvfs_t;

/* ================================================================
 *  文件系统类型 ID（注册用）
 * ================================================================ */
    typedef enum
    {
        VFS_FSTYPE_RAWFLASH = 0, /**< 裸 flash 静态注册伪 FS */
        VFS_FSTYPE_LITTLEFS,     /**< littlefs */
        VFS_FSTYPE_FATFS,        /**< fatfs */
        VFS_FSTYPE_EXTERNAL_BASE /**< 外部扩展起始（第三方 FS 从此往后） */
    } vfs_fstype_t;

/* ================================================================
 *  挂载标志
 * ================================================================ */
#define VFS_MNT_RO (1u << 0) /**< 只读挂载 */

/* ================================================================
 *  后端能力位（caps）：core 据此在调用前拦截不支持的操作
 * ================================================================ */
/** @brief 支持读操作（read） */
#define VFS_CAP_READ     (1u << 0)
/** @brief 支持写操作（write） */
#define VFS_CAP_WRITE    (1u << 1)
/** @brief 支持追加写（VFS_O_APPEND） */
#define VFS_CAP_APPEND   (1u << 2)
/** @brief 支持定位（lseek） */
#define VFS_CAP_SEEK     (1u << 3)
/** @brief 支持截断（truncate） */
#define VFS_CAP_TRUNCATE (1u << 4)
/** @brief 支持目录操作（opendir/readdir/closedir） */
#define VFS_CAP_DIR      (1u << 5)
/** @brief 支持重命名（rename） */
#define VFS_CAP_RENAME   (1u << 6)
/** @brief 支持创建目录（mkdir） */
#define VFS_CAP_MKDIR    (1u << 7)
/** @brief 支持卷容量查询（statvfs） */
#define VFS_CAP_STATVFS  (1u << 8)

    /* ---- 前向声明 ---- */
    struct vfs_mount;
    struct vfs_fs_ops;

    /**
     * @brief 文件会话对象（core 持有，传给 backend）
     *
     * backend 在 open 时填充 priv 指向自己的文件句柄。
     */
    typedef struct vfs_file
    {
        struct vfs_mount *mp; /**< 所属挂载点 */
        void *priv;           /**< backend 私有文件句柄 */
    } vfs_file_t;

    /** @brief 目录会话对象（结构同 vfs_file_t） */
    typedef struct vfs_dir
    {
        struct vfs_mount *mp;
        void *priv;
    } vfs_dir_t;

    /**
     * @brief 挂载点（backend 可见：读 fs_ctx / storage_dev / flags）
     *
     * @note fs_ctx 由 backend 在 mount() 中填充；core 只负责在 unmount() 后释放槽位。
     */
    typedef struct vfs_mount
    {
        char mnt_point[VFS_PATH_MAX]; /**< 挂载点，如 "/app" */
        size_t mnt_len;               /**< 挂载点长度（core 计算） */
        uint8_t fstype;               /**< vfs_fstype_t */
        const struct vfs_fs_ops *fs;  /**< 后端虚函数表（core 填入） */
        void *fs_ctx;                 /**< backend 私有：lfs_t* / FATFS* / 表 */
        const vfs_bdev_t *storage_dev;/**< 挂载源（rawflash 可空） */
        uint8_t flags;                /**< VFS_MNT_* */
        void *lock;                   /**< 每挂载点锁句柄（core 内部使用） */
        uint8_t used;                 /**< 槽位占用标记 */
    } vfs_mount_t;

    /**
     * @brief 文件系统后端虚函数表（唯一扩展点）
     *
     * 必需回调：mount/unmount（若文件系统需要初始化）、open/read/write/lseek/close。
     * 可选回调：置 NULL 表示不支持，core 分派前返回 -ENOTSUP。
     * 所有回调的路径参数 rel 均为**挂载点内的相对路径**（不含挂载前缀）。
     */
    typedef struct vfs_fs_ops
    {
        const char *name; /**< 文件系统名（诊断用） */
        uint32_t caps;    /**< VFS_CAP_* 位集合 */

        /* ---- 文件系统生命周期 ---- */
        int (*mount)(vfs_mount_t *mp);   /**< 建 mp->fs_ctx；可 NULL */
        int (*unmount)(vfs_mount_t *mp); /**< 释放 mp->fs_ctx；可 NULL */

        /* ---- 文件操作（open/read/write/lseek/close 必需） ---- */
        /**
         * @brief 打开/创建挂载点内的相对路径文件
         * @param f     会话对象，成功时须填充 f->priv 指向后端私有句柄
         * @param rel   挂载点内相对路径（不含挂载前缀）
         * @param flags VFS_O_* 组合（core 已做 caps/RO 前置校验）
         * @return 0 成功；< 0 负 errno（-ENOENT/-EROFS/-ENOSPC 等）
         */
        int (*open)(vfs_file_t *f, const char *rel, int flags);
        /**
         * @brief 从当前文件偏移读取数据
         * @return 实际读取字节数（0 表示 EOF）；< 0 负 errno
         */
        ssize_t (*read)(vfs_file_t *f, void *buf, size_t n);
        /**
         * @brief 从当前文件偏移写入数据
         * @return 实际写入字节数；< 0 负 errno（-ENOSPC 空间不足）
         */
        ssize_t (*write)(vfs_file_t *f, const void *buf, size_t n);
        /**
         * @brief 移动文件偏移
         * @param off    目标偏移（相对 whence）
         * @param whence VFS_SEEK_SET/CUR/END
         * @return 新的绝对偏移；< 0 负 errno
         */
        off_t (*lseek)(vfs_file_t *f, off_t off, int whence);
        /**
         * @brief 关闭文件，释放 f->priv 指向的后端私有句柄
         * @return 0 成功；< 0 负 errno
         */
        int (*close)(vfs_file_t *f);

        /* ---- 文件操作（可选） ---- */
        off_t (*tell)(vfs_file_t *f);            /**< 返回当前偏移；< 0 负 errno */
        int (*sync)(vfs_file_t *f);              /**< 刷缓存；无缓存后端可直接返回 0 */
        int (*truncate)(vfs_file_t *f, off_t len);/**< 截断/扩展到 len；< 0 负 errno */

        /* ---- 文件系统级（stat/unlink 必需） ---- */
        /**
         * @brief 查询文件/目录状态
         * @param rel 挂载点内相对路径
         * @param st  输出类型与大小（成功时填充）
         * @return 0 成功；< 0 负 errno
         */
        int (*stat)(vfs_mount_t *mp, const char *rel, vfs_stat_t *st);
        /**
         * @brief 删除文件（目录的删除约束由后端决定）
         * @return 0 成功；< 0 负 errno
         */
        int (*unlink)(vfs_mount_t *mp, const char *rel);

        /* ---- 文件系统级（可选） ---- */
        int (*rename)(vfs_mount_t *mp, const char *from, const char *to); /**< 重命名；< 0 负 errno */
        int (*mkdir)(vfs_mount_t *mp, const char *rel);                  /**< 创建目录；< 0 负 errno */
        int (*statvfs)(vfs_mount_t *mp, vfs_statvfs_t *st);              /**< 卷容量统计；< 0 负 errno */

        /* ---- 目录（可选，需 VFS_CAP_DIR） ---- */
        /**
         * @brief 打开目录，成功时填充 d->priv
         * @return 0 成功；< 0 负 errno
         */
        int (*opendir)(vfs_dir_t *d, const char *rel);
        /**
         * @brief 读取下一个目录项
         * @return 1 有项；0 结束；< 0 负 errno
         */
        int (*readdir)(vfs_dir_t *d, vfs_dirent_t *ent);
        /**
         * @brief 关闭目录，释放 d->priv
         * @return 0 成功；< 0 负 errno
         */
        int (*closedir)(vfs_dir_t *d);
    } vfs_fs_ops_t;

    /**
     * @brief 挂载配置（storage_config.c 中静态定义）
     */
    typedef struct vfs_mount_cfg
    {
        const char *mnt_point; /**< 挂载点，如 "/app" */
        uint8_t fstype;        /**< vfs_fstype_t */
        const char *bdev_name; /**< 绑定的块设备名（rawflash 可 NULL） */
        uint8_t flags;         /**< VFS_MNT_* */
        const void *fs_data;   /**< backend 私有初始化数据（littlefs cfg / rawflash 表） */
    } vfs_mount_cfg_t;

/* ================================================================
 *  生命周期
 * ================================================================ */

    /**
     * @brief 初始化 VFS（清空所有池 + 建核心锁）。可重复调用
     * @return 0 成功；-ENOSPC 核心锁创建失败（仅 VFS_ENABLE_MP_LOCK=1 时）
     */
    int vfs_init(void);

    /**
     * @brief 反初始化 VFS（关闭所有打开句柄、卸载所有挂载点）
     * @details 先关闭所有打开的文件/目录句柄并释放文件锁，再逐个调用后端
     *          unmount 释放 fs_ctx，最后释放每挂载点锁与核心锁并复位所有池。
     *          调用后如需再次使用，须重新执行 vfs_init()。
     */
    void vfs_deinit(void);

    /**
     * @brief 注册文件系统类型
     * @param fstype VFS_FSTYPE_*
     * @param ops    后端虚函数表（须静态常量化）
     * @return 0 成功，-EINVAL 参数错，-EALREADY 已注册，-ENOSPC 注册表满
     */
    int vfs_register(uint8_t fstype, const vfs_fs_ops_t *ops);

    /**
     * @brief 注册块设备（分区切片）
     * @param name bdev 的全局唯一名字（不能为 NULL）
     * @param bdev 待注册的分区切片指针（不能为 NULL）
     * @return 0 成功，-EINVAL 参数错，-EEXIST 名字已注册，-ENOSPC 表满
     */
    int vfs_bdev_register(const char *name, const vfs_bdev_t *bdev);

    /**
     * @brief 批量挂载静态表（在 vfs_init 后、单线程阶段调用）
     * @param tbl 挂载配置数组（不能为 NULL）
     * @param n   数组元素个数
     * @return 0 全部成功，否则最后一个失败的错误码
     */
    int vfs_mount_table(const vfs_mount_cfg_t *tbl, size_t n);

    /**
     * @brief 动态挂载单个卷
     * @param cfg 挂载配置（不能为 NULL）
     * @return 0 成功；< 0 负 errno
     * @note 未开 VFS_ENABLE_DYNAMIC_MOUNT 时返回 -ENOTSUP（静态表仍可用）
     */
    int vfs_mount(const vfs_mount_cfg_t *cfg);

    /**
     * @brief 动态卸载指定挂载点
     * @param mnt_point 挂载点绝对路径，如 "/app"
     * @return 0 成功；-ENOENT 不存在，-EBUSY 仍有打开句柄，< 0 其他负 errno
     * @note 未开 VFS_ENABLE_DYNAMIC_MOUNT 时返回 -ENOTSUP
     */
    int vfs_unmount(const char *mnt_point);

/* ================================================================
 *  文件操作
 * ================================================================ */

    /**
     * @brief 打开/创建文件
     * @details 流程：最长前缀解析挂载点 → 校验访问模式与 O_TRUNC 合法性 →
     *          只读挂载点写意图拦截(-EROFS) → caps 前置校验(-ENOTSUP) →
     *          分配文件句柄槽 → （可选）登记同文件独占锁 → 加挂载点锁并调用
     *          后端 open。成功后返回由句柄池下标编码的 vfs_fd_t。
     * @param path  绝对路径（含挂载前缀），如 "/app/foo.bin"
     * @param flags VFS_O_* 组合
     * @retval >= 0 文件描述符
     * @retval -EINVAL 非法访问模式 / O_RDONLY|O_TRUNC
     * @retval -EROFS  只读挂载点上的写意图
     * @retval -ENOTSUP 后端不支持 APPEND/TRUNC
     * @retval -ENFILE 文件句柄池耗尽
     * @retval -EBUSY  同文件独占锁冲突（VFS_ENABLE_FILE_LOCK）
     */
    vfs_fd_t vfs_open(const char *path, int flags);

    /**
     * @brief 关闭文件句柄
     * @param fd vfs_open 返回的文件描述符
     * @return 0 成功；-EBADF 非法 fd，< 0 其他负 errno
     */
    int vfs_close(vfs_fd_t fd);

    /**
     * @brief 读取：返回读取字节数，< 0 为错误
     * @param fd  文件描述符
     * @param buf 输出缓冲区（不能为 NULL）
     * @param n   期望读取字节数
     * @return >= 0 实际读取字节数（0 表示 EOF）；< 0 负 errno
     */
    ssize_t vfs_read(vfs_fd_t fd, void *buf, size_t n);

    /**
     * @brief 写入：返回写入字节数，< 0 为错误
     * @param fd  文件描述符
     * @param buf 数据缓冲区（不能为 NULL）
     * @param n   期望写入字节数
     * @return >= 0 实际写入字节数；< 0 负 errno
     */
    ssize_t vfs_write(vfs_fd_t fd, const void *buf, size_t n);

    /**
     * @brief 移动文件偏移
     * @param fd     文件描述符
     * @param off    目标偏移（相对 whence）
     * @param whence VFS_SEEK_SET/CUR/END
     * @return >= 0 新的绝对偏移；< 0 负 errno
     */
    off_t vfs_lseek(vfs_fd_t fd, off_t off, int whence);

    /**
     * @brief 获取当前文件偏移
     * @param fd 文件描述符
     * @return >= 0 当前偏移；< 0 负 errno（后端无 tell 时 -ENOTSUP）
     */
    off_t vfs_tell(vfs_fd_t fd);

    /**
     * @brief 刷缓存（把未落盘数据写回介质）
     * @param fd 文件描述符
     * @return 0 成功；后端无 sync 时也返回 0；< 0 负 errno
     */
    int vfs_sync(vfs_fd_t fd);

    /**
     * @brief 定位读（等价 lseek(fd,off,SEEK_SET)+read）
     * @note 与 POSIX pread 语义不同：**会改变文件当前偏移**。连续按偏移读时请递增 off。
     */
    ssize_t vfs_pread(vfs_fd_t fd, void *buf, size_t n, off_t off);

/* ================================================================
 *  文件系统级操作
 * ================================================================ */

    /**
     * @brief 查询文件/目录状态
     * @param path 绝对路径（含挂载前缀）；为挂载点根时返回目录
     * @param st   输出状态（不能为 NULL）
     * @return 0 成功；< 0 负 errno
     */
    int vfs_stat(const char *path, vfs_stat_t *st);

    /**
     * @brief 删除文件
     * @param path 绝对路径（不能为挂载点根）
     * @return 0 成功；< 0 负 errno（-EROFS/-ENOTSUP/-EINVAL 等）
     */
    int vfs_unlink(const char *path);

    /**
     * @brief 创建目录（后端不支持则 -ENOTSUP）
     * @param path 绝对路径（不能为挂载点根）
     * @return 0 成功；< 0 负 errno
     */
    int vfs_mkdir(const char *path);

#if VFS_ENABLE_RENAME
    /**
     * @brief 重命名文件/目录
     * @param from 源绝对路径（不能为 NULL）
     * @param to   目标绝对路径（不能为 NULL，须与 from 在同一挂载点）
     * @return 0 成功；< 0 负 errno（跨挂载点 -EINVAL）
     */
    int vfs_rename(const char *from, const char *to);
#endif

#if VFS_ENABLE_STATVFS
    /**
     * @brief 查询卷容量
     * @param path 绝对路径（用于解析挂载点）
     * @param st   输出容量统计（不能为 NULL）
     * @return 0 成功；< 0 负 errno
     */
    int vfs_statvfs(const char *path, vfs_statvfs_t *st);
#endif

/* ================================================================
 *  目录操作（VFS_ENABLE_DIR）
 * ================================================================ */
#if VFS_ENABLE_DIR
    /**
     * @brief 打开目录，返回目录描述符
     * @param path 绝对路径（含挂载前缀）
     * @return >= 0 目录描述符；< 0 负 errno
     */
    vfs_fd_t vfs_opendir(const char *path);

    /**
     * @brief 读下一个目录项
     * @param fd  目录描述符
     * @param ent 输出目录项（不能为 NULL）
     * @return 1 有项，0 结束，< 0 负 errno
     */
    int vfs_readdir(vfs_fd_t fd, vfs_dirent_t *ent);

    /**
     * @brief 关闭目录
     * @param fd 目录描述符
     * @return 0 成功；-EBADF 非法 fd，< 0 其他负 errno
     */
    int vfs_closedir(vfs_fd_t fd);
#endif

    /**
     * @brief 按类型 ID 查后端（诊断/测试用）
     * @param fstype VFS_FSTYPE_*
     * @return 命中返回后端虚函数表指针，未注册返回 NULL
     */
    const vfs_fs_ops_t *vfs_fstype_lookup(uint8_t fstype);

#ifdef __cplusplus
}
#endif

#endif /* VFS_H */
