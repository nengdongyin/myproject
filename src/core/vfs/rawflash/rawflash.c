/**
 * @file rawflash.c
 * @brief 通用块区存储核心实现（见 rawflash.h）
 *
 * 只依赖 vfs_bdev_t 及其字节/块访问，不依赖 VFS 核心。footer 双份 + CRC32
 * 保证元数据在掉电/位翻转下的存活；写意图以 close 为提交点。
 */

#include "rawflash.h"
#include <string.h>
#include <errno.h>
#include <stddef.h>

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
 * @details 双份冗余：主份写在块首，冗余份写在块中途，任一损坏可从另一份恢复。
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
 * @brief 校验数据区 [from, end) 是否全为 0xFF（可安全 prog）
 * @details 按 256 字节分块读，任一非 0xFF 或读失败即判脏。append 用它校验
 *          "不可擦的当前块尾部"是否干净——flash 对非 0xFF 区域 prog 只做
 *          位与（静默损坏），故必须写前确认。
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

/* ================================================================
 *  对外接口
 * ================================================================ */

size_t rawflash_footer_size(void)
{
    return sizeof(rawflash_footer_t);
}

void rawflash_ctx_init(rawflash_ctx_t *ctx, const vfs_bdev_t *bdev, uint32_t data_cap)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->bdev = bdev;
    ctx->data_cap = data_cap;
    ctx->meta_blk = (bdev && bdev->blk_count > 0) ? (bdev->blk_count - 1) : 0;
}

uint32_t rawflash_visible_len(const vfs_bdev_t *bdev, uint32_t data_cap)
{
    uint32_t len = 0;
    uint32_t meta_blk = bdev->blk_count - 1;
    (void)rf_footer_load(bdev, meta_blk, &len); /* 无效→len=0(空) */
    return (len > data_cap) ? data_cap : len;
}

int rawflash_erase(const vfs_bdev_t *bdev)
{
    if (!bdev)
        return -EINVAL;
    return vfs_bdev_erase(bdev, 0, bdev->blk_count);
}

int rawflash_open(rawflash_ctx_t *ctx, int mode, int append, int truncate)
{
    const vfs_bdev_t *bdev = ctx->bdev;
    ctx->cursor = 0;
    ctx->rdlen = 0;
    ctx->append = 0;

    if (append)
    {
        /* 尾部续写：读 footer 得 L → 校验不可擦段(A) → 擦可擦段(B) → 续写 */
        uint32_t len = rawflash_visible_len(bdev, ctx->data_cap);
        uint32_t b = len / bdev->blk_size;
        uint32_t a_end = (b + 1) * bdev->blk_size; /* L 所在块尾（不可擦段上界） */
        if (a_end > ctx->data_cap)
            a_end = ctx->data_cap;
        /* A 段必须全 0xFF：否则是上次掉电残留在当前块尾部，prog 会位与损坏 */
        if (len < a_end && !rf_range_all_ff(bdev, len, a_end))
            return -EIO;
        /* B 段（L 之后整块，无已提交数据）一次擦净；L 块对齐时含当前块 */
        uint32_t b_start = (len % bdev->blk_size == 0) ? b : b + 1;
        uint32_t nblk = (bdev->blk_count - 1) - b_start; /* 天然不含 meta 块 */
        if (nblk > 0)
        {
            int rc = vfs_bdev_erase(bdev, b_start, nblk);
            if (rc < 0)
                return rc;
        }
        ctx->mode = RAWFLASH_MODE_WRITE;
        ctx->append = 1;
        ctx->cursor = len;
        return 0;
    }

    if (mode != RAWFLASH_MODE_READ || truncate)
    {
        if (!truncate && mode == RAWFLASH_MODE_RDWR)
        {
            /* O_RDWR 无 O_TRUNC：读-改-写会话。打开不擦，先按只读装载旧内容；
             * 首次 write 才擦整区并转流式写，避免"打开即毁数据"。 */
            ctx->mode = RAWFLASH_MODE_RDWR;
            ctx->rdlen = rawflash_visible_len(bdev, ctx->data_cap);
        }
        else
        {
            /* 写意图：擦整分区（数据区 + 元数据块），进入写模式 */
            int rc = rawflash_erase(bdev);
            if (rc < 0)
                return rc;
            ctx->mode = RAWFLASH_MODE_WRITE;
        }
    }
    else
    {
        /* 只读：读 footer，绝不擦/写 */
        ctx->mode = RAWFLASH_MODE_READ;
        ctx->rdlen = rawflash_visible_len(bdev, ctx->data_cap);
    }
    return 0;
}

ssize_t rawflash_read(rawflash_ctx_t *ctx, void *buf, size_t n)
{
    if (!ctx || (ctx->mode != RAWFLASH_MODE_READ && ctx->mode != RAWFLASH_MODE_RDWR))
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

ssize_t rawflash_write(rawflash_ctx_t *ctx, const void *buf, size_t n)
{
    if (!ctx || (ctx->mode != RAWFLASH_MODE_WRITE && ctx->mode != RAWFLASH_MODE_RDWR))
        return -EBADF;
    if (n == 0)
        return 0; /* 空写不改变状态：RDWR 下尤其不得因 0 字节触发整区擦除 */
    if (ctx->mode == RAWFLASH_MODE_RDWR)
    {
        /* 首次写入：先做容量检查再擦除——避免"写太大"先擦全区导致旧数据丢失 */
        if (n > (size_t)ctx->data_cap)
            return -ENOSPC;
        int rc = rawflash_erase(ctx->bdev);
        if (rc < 0)
            return rc;
        ctx->mode = RAWFLASH_MODE_WRITE;
        ctx->cursor = 0;
        ctx->rdlen = 0;
    }
    /* 空间检查（n 为 size_t，先比上界再做减法，防回绕） */
    if (n > (size_t)ctx->data_cap || ctx->cursor > ctx->data_cap - (uint32_t)n)
        return -ENOSPC;

    int rc = vfs_bdev_prog_bytes(ctx->bdev, ctx->cursor, buf, n);
    if (rc < 0)
        return rc;
    ctx->cursor += (uint32_t)n; /* 写游标累加：支持同一会话多次 write */
    return (ssize_t)n;
}

off_t rawflash_lseek(rawflash_ctx_t *ctx, off_t off, int whence)
{
    if (!ctx || (ctx->mode != RAWFLASH_MODE_READ && ctx->mode != RAWFLASH_MODE_RDWR))
        return -ENOTSUP; /* 写阶段不支持回改 */

    off_t base;
    switch (whence)
    {
    case RAWFLASH_SEEK_SET:
        base = 0;
        break;
    case RAWFLASH_SEEK_CUR:
        base = (off_t)ctx->cursor;
        break;
    case RAWFLASH_SEEK_END:
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

off_t rawflash_tell(const rawflash_ctx_t *ctx)
{
    return ctx ? (off_t)ctx->cursor : -EBADF;
}

int rawflash_sync(rawflash_ctx_t *ctx)
{
    (void)ctx; /* 直接写 flash，无缓存 */
    return 0;
}

int rawflash_truncate(rawflash_ctx_t *ctx)
{
    if (!ctx)
        return -EBADF;
    int rc = rawflash_erase(ctx->bdev);
    if (rc < 0)
        return rc;
    ctx->mode = RAWFLASH_MODE_WRITE;
    ctx->cursor = 0;
    ctx->rdlen = 0;
    return 0;
}

int rawflash_close(rawflash_ctx_t *ctx)
{
    int rc = 0;
    if (ctx->mode == RAWFLASH_MODE_WRITE)
    {
        if (ctx->append)
        {
            /* append：meta 块仍存旧 footer，prog 会位与损坏，先擦再写 */
            rc = vfs_bdev_erase(ctx->bdev, ctx->meta_blk, 1);
        }
        if (rc == 0)
            rc = rf_footer_store(ctx->bdev, ctx->meta_blk, ctx->cursor);
    }
    return rc;
}
