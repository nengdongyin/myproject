/**
 * @file vfs_rawflash.c
 * @brief 裸 flash 文件系统后端实现（静态注册 + 尾块元数据 + 写游标）
 *
 * 设计见 vfs_rawflash.h。核心要点：
 *  - 文件 = 静态注册表里的一条，映射到一个 FAL 分区（vfs_bdev_t）。
 *  - 分区最后一个擦除块（blk_size）为元数据块，存 footer 主份 + 冗余份。
 *  - footer 记录 data_len + CRC；掉电/半写导致无效时，文件视为空。
 *  - 写游标 ctx->cursor 在 open(写) 置 0，每次 write 累加，close 提交。
 */

#include "vfs_rawflash.h"
#include <string.h>
#include <errno.h>
#include <stddef.h>
#include "port.h"

/* ================================================================
 *  Footer 定义
 * ================================================================ */

/** @brief Footer 魔数 ASCII "RFLH" */
#define RAWFLASH_MAGIC 0x52464C48u
/** @brief Footer 版本 */
#define RAWFLASH_VERSION 1u

/** @brief 元数据块内主份偏移 */
#define RAWFLASH_PRIMARY_OFF 0u
/** @brief 元数据块内冗余份偏移（块中途） */
#define RAWFLASH_REDUNDANT_DIV 2u

    typedef struct
    {
        uint32_t magic;    /**< RAWFLASH_MAGIC */
        uint16_t version;  /**< RAWFLASH_VERSION */
        uint16_t flags;    /**< 保留 */
        uint32_t data_len; /**< 数据区有效字节数 */
        uint32_t crc32;    /**< 覆盖前 sizeof-4 字节 */
    } rawflash_footer_t;

/** @brief CRC32（多项式 0xEDB88320，逐位实现，无需查表） */
static uint32_t rf_crc32(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
    {
        crc ^= p[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }
    return ~crc;
}

/* ================================================================
 *  每句柄上下文 + 静态池
 * ================================================================ */

#define RAW_MODE_READ  0
#define RAW_MODE_WRITE 1
#define RAW_MODE_RDWR  2 /**< O_RDWR 无 O_TRUNC：先可读，首次 write 才整区擦除重写 */

typedef struct
{
    uint8_t used;
    uint8_t mode; /* RAW_MODE_* */
    const vfs_rawflash_entry_t *entry;
    const vfs_bdev_t *bdev;
    uint32_t meta_blk;  /**< 元数据块号（= blk_count-1） */
    uint32_t data_cap;  /**< 数据容量（= entry->size） */
    uint32_t cursor;    /**< 写游标 / 读位置 */
    uint32_t rdlen;     /**< 读模式下的有效长度（来自 footer） */
    uint8_t append;     /**< 1=追加会话（close 需先擦 meta 块再写 footer） */
} raw_ctx_t;

typedef struct
{
    uint8_t used;                      /**< 槽位占用标记 */
    const vfs_rawflash_vol_t *vol;     /**< 所属卷（静态注册表） */
    size_t idx;                        /**< 下一次 readdir 要返回的条目下标 */
} raw_dir_t;

/** @brief rawflash 文件句柄池（静态池，零堆） */
static raw_ctx_t g_raw_files[VFS_MAX_OPEN_FILES];
#if VFS_ENABLE_DIR
/** @brief rawflash 目录句柄池（静态池，零堆） */
static raw_dir_t g_raw_dirs[VFS_MAX_OPEN_DIRS];
#endif

/**
 * @brief 分配 rawflash 文件句柄槽，并在写意图下做写-写互斥
 * @details 用后端池锁 SYS_LOCK_FILE 保护整个"查重 + 分配"临界区：
 *          want_write 时若同 entry 已有写档（WRITE / RDWR）则拒绝，
 *          保证同一文件不会有两个写会话交错（append 与整擦写一并保护）。
 * @param e          目标条目（并发检测键；可为 NULL）
 * @param want_write 1=写意图（含 append），需独占；0=只读
 * @param err_out    失败时输出负 errno（-EBUSY 写冲突，-ENFILE 池满）
 * @return 空闲槽指针；失败返回 NULL 并写 *err_out
 */
static raw_ctx_t *raw_ctx_alloc_guarded(const vfs_rawflash_entry_t *e, int want_write, int *err_out)
{
    raw_ctx_t *slot = NULL;
    int err = 0;
    system_lock(SYS_LOCK_FILE);
    if (want_write && e)
    {
        for (int i = 0; i < VFS_MAX_OPEN_FILES; i++)
        {
            if (g_raw_files[i].used && g_raw_files[i].entry == e &&
                g_raw_files[i].mode != RAW_MODE_READ) /* WRITE / RDWR 均视为写档 */
            {
                err = -EBUSY;
                goto out;
            }
        }
    }
    for (int i = 0; i < VFS_MAX_OPEN_FILES; i++)
    {
        if (!g_raw_files[i].used)
        {
            memset(&g_raw_files[i], 0, sizeof(g_raw_files[i]));
            g_raw_files[i].used = 1;
            g_raw_files[i].entry = e;
            g_raw_files[i].mode = want_write ? RAW_MODE_WRITE : RAW_MODE_READ;
            slot = &g_raw_files[i];
            break;
        }
    }
    if (!slot)
        err = -ENFILE;
out:
    system_unlock(SYS_LOCK_FILE);
    if (err_out)
        *err_out = err;
    return slot;
}

/** @brief 释放 rawflash 文件句柄槽（清零后归还池） */
static void raw_ctx_free(raw_ctx_t *c)
{
    if (!c)
        return;
    system_lock(SYS_LOCK_FILE);
    memset(c, 0, sizeof(*c));
    system_unlock(SYS_LOCK_FILE);
}

/* ================================================================
 *  静态注册表查找
 * ================================================================ */

/** @brief 按相对路径在静态注册表中查找条目 */
static const vfs_rawflash_entry_t *raw_entry_find(const vfs_rawflash_vol_t *vol, const char *rel)
{
    if (!vol || !rel)
        return NULL;
    for (size_t i = 0; i < vol->count; i++)
    {
        if (vol->entries[i].name && strcmp(vol->entries[i].name, rel) == 0)
            return &vol->entries[i];
    }
    return NULL;
}

/* ================================================================
 *  Footer 读写
 * ================================================================ */

/** @brief 校验一份 footer 是否有效 */
static int rf_footer_valid(const rawflash_footer_t *f)
{
    if (f->magic != RAWFLASH_MAGIC || f->version != RAWFLASH_VERSION)
        return 0;
    uint32_t crc = rf_crc32(f, offsetof(rawflash_footer_t, crc32));
    return (crc == f->crc32) ? 1 : 0;
}

/**
 * @brief 从元数据块读回有效 data_len
 * @details 依次读主份与冗余份，先通过 CRC 校验者生效；两份都无效时把
 *          *out_len 置 0 并返回 -ENOENT（掉电半写 → 视为空文件）。
 * @param bdev     分区切片
 * @param meta_blk 元数据块号（= blk_count-1）
 * @param out_len  输出有效数据长度
 * @return 0 成功（*out_len 有效），-ENOENT footer 无效（视为空）
 */
static int rf_footer_load(const vfs_bdev_t *bdev, uint32_t meta_blk, uint32_t *out_len)
{
    uint32_t meta_byte = meta_blk * bdev->blk_size;
    uint32_t cpy_off[2] = {RAWFLASH_PRIMARY_OFF, bdev->blk_size / RAWFLASH_REDUNDANT_DIV};

    for (int i = 0; i < 2; i++)
    {
        rawflash_footer_t f;
        if (vfs_bdev_read_bytes(bdev, meta_byte + cpy_off[i], &f, sizeof(f)) == 0 &&
            rf_footer_valid(&f))
        {
            *out_len = f.data_len;
            return 0;
        }
    }
    *out_len = 0;
    return -ENOENT;
}

/**
 * @brief 写 footer 主份 + 冗余份（要求元数据块已擦除）
 * @details 双份冗余策略：主份写在块首，冗余份写在块中途，任一损坏可从另
 *          一份恢复，提高掉电/位翻转下的元数据存活率。
 * @param bdev     分区切片
 * @param meta_blk 元数据块号
 * @param data_len 要记录的数据区有效长度
 * @return 0 成功；< 0 负 errno（底层编程失败）
 */
static int rf_footer_store(const vfs_bdev_t *bdev, uint32_t meta_blk, uint32_t data_len)
{
    rawflash_footer_t f;
    memset(&f, 0, sizeof(f));
    f.magic = RAWFLASH_MAGIC;
    f.version = RAWFLASH_VERSION;
    f.data_len = data_len;
    f.crc32 = rf_crc32(&f, offsetof(rawflash_footer_t, crc32));

    uint32_t meta_byte = meta_blk * bdev->blk_size;
    int rc = vfs_bdev_prog_bytes(bdev, meta_byte + RAWFLASH_PRIMARY_OFF, &f, sizeof(f));
    if (rc < 0)
        return rc;
    return vfs_bdev_prog_bytes(bdev, meta_byte + bdev->blk_size / RAWFLASH_REDUNDANT_DIV,
                               &f, sizeof(f));
}

/**
 * @brief 读回某条目“可见文件长度”（footer 有效数据长度，夹取到注册容量 size）
 * @details 统一“footer → 可见长度”规则，供读路径（open 只读/RDWR 装载）、
 *          stat、readdir 共用，避免多处各自夹取导致规则漂移。夹取上界用
 *          entry->size（逻辑容量），与写路径 raw_write 的上界一致；因挂载时
 *          已校验 size ≤ 数据区容量，结果恒落在数据区内、不触及元数据块。
 * @param e 静态注册条目（不能为 NULL）
 * @return 可见长度（字节）；footer 无效时返回 0
 */
static uint32_t raw_visible_len(const vfs_rawflash_entry_t *e)
{
    uint32_t len = 0;
    uint32_t meta_blk = e->bdev->blk_count - 1;
    (void)rf_footer_load(e->bdev, meta_blk, &len); /* 无效→len=0(空) */
    return (len > e->size) ? e->size : len;
}

#if VFS_RAWFLASH_ENABLE_APPEND
/**
 * @brief 校验数据区 [from, end) 是否全为 0xFF（可安全 prog）
 * @details 按 256 字节分块读，任一非 0xFF 或读失败即判脏。append 用它校验
 *          "不可擦的当前块尾部"是否干净——flash 对非 0xFF 区域 prog 只做
 *          位与（静默损坏），故必须写前确认。
 * @return 1 全 0xFF（干净）；0 含非 0xFF 或读失败（脏）
 */
static int rf_range_all_ff(const vfs_bdev_t *bdev, uint32_t from, uint32_t end)
{
    uint8_t buf[256];
    for (uint32_t off = from; off < end;)
    {
        size_t chunk = (size_t)(end - off);
        if (chunk > sizeof(buf))
            chunk = sizeof(buf);
        if (vfs_bdev_read_bytes(bdev, off, buf, chunk) < 0)
            return 0;
        for (size_t i = 0; i < chunk; i++)
        {
            if (buf[i] != 0xFF)
                return 0;
        }
        off += (uint32_t)chunk;
    }
    return 1;
}
#endif /* VFS_RAWFLASH_ENABLE_APPEND */

/* ================================================================
 *  backend 回调
 * ================================================================ */

/**
 * @brief 挂载 rawflash 卷：校验静态注册表与各分区几何一致性
 * @return 0 成功；-EINVAL 注册表为空或某分区容量/块大小不足
 */
static int raw_mount(vfs_mount_t *mp)
{
    const vfs_rawflash_vol_t *vol = (const vfs_rawflash_vol_t *)mp->fs_ctx;
    if (!vol || !vol->entries || vol->count == 0)
        return -EINVAL;

    /* 校验每个分区的容量一致性 */
    for (size_t i = 0; i < vol->count; i++)
    {
        const vfs_rawflash_entry_t *e = &vol->entries[i];
        if (!e->name || !e->bdev || e->bdev->blk_count < 2 ||
            e->bdev->blk_size < 2 * sizeof(rawflash_footer_t))
            return -EINVAL;
        uint32_t cap = (e->bdev->blk_count - 1) * e->bdev->blk_size;
        if (e->size > cap)
            return -EINVAL; /* 注册容量超过数据区 → 配置错误 */
    }
    return 0;
}

/** @brief 卸载 rawflash 卷（无持久状态需释放，直接返回 0） */
static int raw_unmount(vfs_mount_t *mp)
{
    (void)mp;
    return 0;
}

/**
 * @brief 打开一个静态注册的"文件"
 * @details 设计要点：文件名必须在注册表中，未注册直接 -ENOENT。写意图
 *          （O_WRONLY/O_RDWR/O_TRUNC）会**擦除整个分区**（数据区+元数据块）
 *          并把写游标置 0，随后同一会话多次 write 顺序追加；只读模式绝不
 *          擦/写，仅从 footer 读回有效长度。掉电半写时 footer 无效 → 空文件。
 * @param f     会话对象（成功时填充 f->priv）
 * @param rel   挂载点内相对路径（= 注册名）
 * @param flags VFS_O_* 组合
 * @return 0 成功；-ENOENT 未注册，-EROFS 只读条目被写，-ENFILE 句柄池满，
 *         其余负 errno
 */
static int raw_open(vfs_file_t *f, const char *rel, int flags)
{
    const vfs_rawflash_vol_t *vol = (const vfs_rawflash_vol_t *)f->mp->fs_ctx;
    const vfs_rawflash_entry_t *e = raw_entry_find(vol, rel);
    if (!e)
        return -ENOENT; /* 未注册文件 → 打开失败（需求4） */

    int acc = flags & 0x3;
    int is_write = (acc == VFS_O_WRONLY) || (acc == VFS_O_RDWR);
    int trunc = (flags & VFS_O_TRUNC) ? 1 : 0;
#if VFS_RAWFLASH_ENABLE_APPEND
    int append = (flags & VFS_O_APPEND) && !trunc; /* 截断优先，走整擦路径 */
#else
    int append = 0;
#endif

    if ((e->flags & VFS_RAWRO) && (is_write || trunc || append || (flags & VFS_O_CREAT)))
        return -EROFS;

    int want_write = is_write || trunc || append;
    int aerr = 0;
    raw_ctx_t *ctx = raw_ctx_alloc_guarded(e, want_write, &aerr);
    if (!ctx)
        return aerr;

    ctx->bdev = e->bdev;
    ctx->meta_blk = e->bdev->blk_count - 1;
    ctx->data_cap = e->size;
    ctx->cursor = 0;

#if VFS_RAWFLASH_ENABLE_APPEND
    if (append)
    {
        /* 尾部续写：读 footer 得 L → 校验不可擦段(A) → 擦可擦段(B) → 续写 */
        uint32_t len = raw_visible_len(e); /* footer 有效长度，夹取到注册容量 */
        uint32_t b = len / e->bdev->blk_size;
        uint32_t a_end = (b + 1) * e->bdev->blk_size; /* L 所在块尾（不可擦段上界） */
        if (a_end > e->size)
            a_end = e->size;
        /* A 段必须全 0xFF：否则是上次掉电残留在当前块尾部，prog 会位与损坏 */
        if (len < a_end && !rf_range_all_ff(e->bdev, len, a_end))
        {
            raw_ctx_free(ctx);
            return -EIO;
        }
        /* B 段（L 之后整块，无已提交数据）一次擦净；L 块对齐时含当前块 */
        uint32_t b_start = (len % e->bdev->blk_size == 0) ? b : b + 1;
        uint32_t nblk = (e->bdev->blk_count - 1) - b_start; /* 天然不含 meta 块 */
        if (nblk > 0)
        {
            int rc = vfs_bdev_erase(e->bdev, b_start, nblk);
            if (rc < 0)
            {
                raw_ctx_free(ctx);
                return rc;
            }
        }
        ctx->mode = RAW_MODE_WRITE;
        ctx->append = 1;
        ctx->cursor = len;
        ctx->rdlen = 0;
        f->priv = ctx;
        return 0;
    }
#endif /* VFS_RAWFLASH_ENABLE_APPEND */

    if (is_write || trunc)
    {
        if (!trunc && acc == VFS_O_RDWR)
        {
            /* O_RDWR 无 O_TRUNC：读-改-写会话。打开时不擦除，先按只读装载
             * （可读旧内容）；首次 write 时才擦整区并转流式写，避免"打开即毁数据"。 */
            ctx->mode = RAW_MODE_RDWR;
            ctx->rdlen = raw_visible_len(e);
        }
        else
        {
            /* 写意图（O_WRONLY / 任一 O_TRUNC）：擦整分区（数据区 + 元数据块），进入写模式 */
            int rc = vfs_bdev_erase(e->bdev, 0, e->bdev->blk_count);
            if (rc < 0)
            {
                raw_ctx_free(ctx);
                return rc;
            }
            ctx->mode = RAW_MODE_WRITE;
            ctx->rdlen = 0;
        }
    }
    else
    {
        /* 只读：读 footer，绝不擦/写 */
        ctx->mode = RAW_MODE_READ;
        ctx->rdlen = raw_visible_len(e);
    }

    f->priv = ctx;
    return 0;
}

/**
 * @brief 只读模式读取数据（不越过 rdlen，游标递增）
 * @return >= 0 读取字节数（0 表示 EOF）；< 0 负 errno
 */
static ssize_t raw_read(vfs_file_t *f, void *buf, size_t n)
{
    raw_ctx_t *ctx = (raw_ctx_t *)f->priv;
    if (!ctx || (ctx->mode != RAW_MODE_READ && ctx->mode != RAW_MODE_RDWR))
        return -EBADF;
    if (n == 0 || ctx->cursor >= ctx->rdlen)
        return 0;

    size_t avail = (size_t)(ctx->rdlen - ctx->cursor);
    if (n > avail)
        n = avail;

    int rc = vfs_bdev_read_bytes(ctx->bdev, ctx->cursor, buf, n);
    if (rc < 0)
        return rc;
    ctx->cursor += (uint32_t)n;
    return (ssize_t)n;
}

/**
 * @brief 写模式顺序写数据
 * @details 先做容量上界检查（防 size_t 回绕），再按当前写游标编程，最后
 *          累加游标以支持同一 open 会话内多次 write。
 * @return >= 0 写入字节数；-EBADF 非写模式，-ENOSPC 超出注册容量
 */
static ssize_t raw_write(vfs_file_t *f, const void *buf, size_t n)
{
    raw_ctx_t *ctx = (raw_ctx_t *)f->priv;
    if (!ctx || (ctx->mode != RAW_MODE_WRITE && ctx->mode != RAW_MODE_RDWR))
        return -EBADF;
    if (n == 0)
        return 0; /* 空写不改变状态：RDWR 下尤其不得因 0 字节触发整区擦除 */
    if (ctx->mode == RAW_MODE_RDWR)
    {
        /* 首次写入：先做容量检查再擦除——避免"写太大"先擦全区导致旧数据丢失。
         * RDWR 写从 0 整区重写，此处只需判 n 是否超过注册容量。 */
        if (n > (size_t)ctx->data_cap)
            return -ENOSPC;
        /* 此刻才擦除整区并转流式写，从 0 整区重写（读阶段结束） */
        int rc = vfs_bdev_erase(ctx->bdev, 0, ctx->bdev->blk_count);
        if (rc < 0)
            return rc;
        ctx->mode = RAW_MODE_WRITE;
        ctx->cursor = 0;
        ctx->rdlen = 0;
    }
    /* 空间检查（n 为 size_t，先比上界再做减法，防回绕） */
    if (n > (size_t)ctx->data_cap || ctx->cursor > ctx->data_cap - (uint32_t)n)
        return -ENOSPC;

    int rc = vfs_bdev_prog_bytes(ctx->bdev, ctx->cursor, buf, n);
    if (rc < 0)
        return rc;
    ctx->cursor += (uint32_t)n; /* ★写游标累加：支持同一会话多次 write */
    return (ssize_t)n;
}

/**
 * @brief 只读模式下移动读游标
 * @return >= 0 新偏移；-ENOTSUP 写模式，-EINVAL 参数/越界
 */
static off_t raw_lseek(vfs_file_t *f, off_t off, int whence)
{
    raw_ctx_t *ctx = (raw_ctx_t *)f->priv;
    if (!ctx || (ctx->mode != RAW_MODE_READ && ctx->mode != RAW_MODE_RDWR))
        return -ENOTSUP; /* 写阶段不支持回改 */

    off_t base;
    switch (whence)
    {
    case VFS_SEEK_SET:
        base = 0;
        break;
    case VFS_SEEK_CUR:
        base = (off_t)ctx->cursor;
        break;
    case VFS_SEEK_END:
        base = (off_t)ctx->rdlen;
        break;
    default:
        return -EINVAL;
    }

    off_t np = base + off;
    if (np < 0 || np > (off_t)ctx->rdlen)
        return -EINVAL;
    ctx->cursor = (uint32_t)np;
    return np;
}

/** @brief 返回当前游标位置；句柄无效返回 -EBADF */
static off_t raw_tell(vfs_file_t *f)
{
    raw_ctx_t *ctx = (raw_ctx_t *)f->priv;
    return ctx ? (off_t)ctx->cursor : -EBADF;
}

/** @brief 刷缓存：直接写 flash，无缓存，恒返回 0 */
static int raw_sync(vfs_file_t *f)
{
    (void)f; /* 直接写 flash，无缓存 */
    return 0;
}

/**
 * @brief 截断：仅支持截断到 0（= 擦除整个分区并进入写模式）
 * @return 0 成功；-EBADF 句柄无效，-ENOTSUP len 非 0，< 0 底层擦除失败
 */
static int raw_truncate(vfs_file_t *f, off_t len)
{
    raw_ctx_t *ctx = (raw_ctx_t *)f->priv;
    if (!ctx)
        return -EBADF;
    if (len != 0)
        return -ENOTSUP; /* 仅支持截断到 0（= 擦除重写） */

    int rc = vfs_bdev_erase(ctx->bdev, 0, ctx->bdev->blk_count);
    if (rc < 0)
        return rc;
    ctx->mode = RAW_MODE_WRITE;
    ctx->cursor = 0;
    ctx->rdlen = 0;
    return 0;
}

/**
 * @brief 关闭文件：写模式提交 footer，随后释放句柄
 * @details 写意图的语义是"close 才落 footer"，close 之前数据虽已写 flash
 *          但不可见（footer 仍无效），保证掉电不会读到半截数据。
 * @return 0 成功；写模式 footer 写入失败返回负 errno
 */
static int raw_close(vfs_file_t *f)
{
    raw_ctx_t *ctx = (raw_ctx_t *)f->priv;
    if (!ctx)
        return 0;

    int rc = 0;
    if (ctx->mode == RAW_MODE_WRITE)
    {
#if VFS_RAWFLASH_ENABLE_APPEND
        if (ctx->append)
        {
            /* append：meta 块仍存旧 footer，prog 会位与损坏，先擦再写 */
            rc = vfs_bdev_erase(ctx->bdev, ctx->meta_blk, 1);
        }
#endif
        if (rc == 0)
            rc = rf_footer_store(ctx->bdev, ctx->meta_blk, ctx->cursor);
    }

    raw_ctx_free(ctx);
    f->priv = NULL;
    return rc;
}

/**
 * @brief 查询文件状态：文件长度来自 footer，footer 无效时为 0
 * @return 0 成功；-ENOENT 未注册
 */
static int raw_stat(vfs_mount_t *mp, const char *rel, vfs_stat_t *st)
{
    const vfs_rawflash_vol_t *vol = (const vfs_rawflash_vol_t *)mp->fs_ctx;
    const vfs_rawflash_entry_t *e = raw_entry_find(vol, rel);
    if (!e)
        return -ENOENT;

    st->type = VFS_TYPE_FILE;
    st->size = raw_visible_len(e);
    return 0;
}

/**
 * @brief 删除文件：擦除整个分区（含元数据块）即视为删除
 * @return 0 成功；-ENOENT 未注册，-EROFS 只读条目
 */
static int raw_unlink(vfs_mount_t *mp, const char *rel)
{
    const vfs_rawflash_vol_t *vol = (const vfs_rawflash_vol_t *)mp->fs_ctx;
    const vfs_rawflash_entry_t *e = raw_entry_find(vol, rel);
    if (!e)
        return -ENOENT;
    if (e->flags & VFS_RAWRO)
        return -EROFS;
    /* 擦除整个分区（含元数据块）即删除 */
    return vfs_bdev_erase(e->bdev, 0, e->bdev->blk_count);
}

#if VFS_ENABLE_DIR
/**
 * @brief 打开目录：仅支持卷根目录（扁平命名空间）
 * @return 0 成功；-ENOTSUP 非根目录，-ENFILE 目录池满
 */
static int raw_opendir(vfs_dir_t *d, const char *rel)
{
    /* 仅支持卷根目录（rawflash 为扁平命名空间） */
    if (rel[0] != '\0')
        return -ENOTSUP;

    system_lock(SYS_LOCK_FILE);
    for (int i = 0; i < VFS_MAX_OPEN_DIRS; i++)
    {
        if (!g_raw_dirs[i].used)
        {
            g_raw_dirs[i].used = 1;
            g_raw_dirs[i].vol = (const vfs_rawflash_vol_t *)d->mp->fs_ctx;
            g_raw_dirs[i].idx = 0;
            system_unlock(SYS_LOCK_FILE);
            d->priv = &g_raw_dirs[i];
            return 0;
        }
    }
    system_unlock(SYS_LOCK_FILE);
    return -ENFILE;
}

/**
 * @brief 读取下一个静态注册条目
 * @return 1 有项；0 结束；-EBADF 句柄无效
 */
static int raw_readdir(vfs_dir_t *d, vfs_dirent_t *ent)
{
    raw_dir_t *it = (raw_dir_t *)d->priv;
    if (!it || !it->vol)
        return -EBADF;

    const vfs_rawflash_vol_t *vol = it->vol;
    if (it->idx >= vol->count)
        return 0; /* 结束 */

    const vfs_rawflash_entry_t *e = &vol->entries[it->idx++];
    memset(ent, 0, sizeof(*ent));
    ent->type = VFS_TYPE_FILE;
    ent->size = raw_visible_len(e);
    strncpy(ent->name, e->name, VFS_NAME_MAX);
    ent->name[VFS_NAME_MAX] = '\0';
    return 1; /* 有项 */
}

/** @brief 关闭目录：清零目录句柄槽并置 d->priv = NULL */
static int raw_closedir(vfs_dir_t *d)
{
    raw_dir_t *it = (raw_dir_t *)d->priv;
    if (it)
    {
        system_lock(SYS_LOCK_FILE);
        memset(it, 0, sizeof(*it));
        system_unlock(SYS_LOCK_FILE);
    }
    d->priv = NULL;
    return 0;
}
#endif /* VFS_ENABLE_DIR */

/* ================================================================
 *  虚函数表
 * ================================================================ */

const vfs_fs_ops_t vfs_rawflash_ops = {
    .name = "rawflash",
    .caps = VFS_CAP_READ | VFS_CAP_WRITE | VFS_CAP_TRUNCATE | VFS_CAP_SEEK | VFS_CAP_DIR
#if VFS_RAWFLASH_ENABLE_APPEND
            | VFS_CAP_APPEND
#endif
    ,
    .mount = raw_mount,
    .unmount = raw_unmount,
    .open = raw_open,
    .read = raw_read,
    .write = raw_write,
    .lseek = raw_lseek,
    .close = raw_close,
    .tell = raw_tell,
    .sync = raw_sync,
    .truncate = raw_truncate,
    .stat = raw_stat,
    .unlink = raw_unlink,
    .rename = NULL,
    .mkdir = NULL,
    .statvfs = NULL,
#if VFS_ENABLE_DIR
    .opendir = raw_opendir,
    .readdir = raw_readdir,
    .closedir = raw_closedir,
#else
    .opendir = NULL,
    .readdir = NULL,
    .closedir = NULL,
#endif
};
