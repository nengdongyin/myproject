/**
 * @file file_io_flash.c
 * @brief Flash 直接 I/O 后端实现（基于 FAL — Flash Abstraction Layer）
 *
 * 不依赖文件系统，直接通过 FAL API 对 Flash 分区进行块级读写操作。
 * 使用分区末尾的 @ref flash_footer_t 存储元数据，支持数据完整性和大小还原。
 *
 * @section footer_design Footer 元数据设计
 * 每个 Flash 分区的最后 @ref FLASH_FOOTER_SIZE 字节（64 字节）保留为 footer：
 * - magic (0x46544F4F = "FOOT"): 魔数校验
 * - version (1): footer 结构版本号（精确匹配，不兼容未来版本）
 * - data_len: 实际写入的数据字节数
 * - reserved[13]: 对齐填充（共 64 字节）
 *
 * 写流程：open 擦除分区 → write 追加数据 → close 写 footer
 * 读流程：open → read → close
 * abort：擦除分区（丢弃所有数据）
 *
 * @section path_format 路径格式
 * 以 "分区名/文件名" 格式传递，通过第一个 '/' 前的子串匹配 FAL 分区。
 * 例如 "cfg_partition/settings.bin" → 查找 FAL 分区 "cfg_partition"
 */

#include "file_io_flash.h"
#include "fal.h"
#include <string.h>
#include <stdbool.h>
#include "port.h"

/**
 * @brief Footer 魔数（ASCII: "FOOT" 的小端表示）
 */
#define FLASH_FOOTER_MAGIC 0x46544F4FU

/**
 * @brief Footer 版本号（当前为 1）
 *
 * 用于区分不同版本的 footer 结构布局。
 * 版本不匹配时，get_size 返回分区原始大小而非 footer.data_len。
 */
#define FLASH_FOOTER_VERSION 1U

/**
 * @brief Footer 占用字节数（含 reserved 填充）
 *
 * 64 字节对齐到典型 Flash 的最小写入单元。
 */
#define FLASH_FOOTER_SIZE 64U

/**
 * @brief Flash 分区 Footer 结构体
 *
 * 存储在分区末尾的最后 64 字节中。
 * 写入路径：只有在 close 时才写入 footer（write 阶段仅追加数据）。
 * 读取路径：get_size 通过 footer 还原实际数据长度。
 */
typedef struct
{
    uint32_t magic;        /**< 魔数 0x46544F4F，用于校验 footer 有效性 */
    uint32_t version;      /**< Footer 版本号（当前 = 1） */
    uint32_t data_len;     /**< 实际写入数据的字节数（不含 footer） */
    uint32_t reserved[13]; /**< 保留字段（填零对齐到 64 字节） */
} flash_footer_t;

/**
 * @brief Flash 操作句柄
 *
 * 每次 open 时动态分配，close/abort 时释放。
 * 记录当前操作的 FAL 分区引用、偏移量和打开模式。
 */
typedef struct
{
    const struct fal_partition *part; /**< FAL 分区描述符指针 */
    uint32_t offset;                  /**< 当前写入偏移（字节，从 0 开始） */
    file_io_mode_t mode;              /**< 打开模式（读/写） */
} flash_handle_t;

/**
 * @brief 从路径中提取 FAL 分区名
 *
 * 路径格式为 "分区名/文件名"，通过查找第一个 '/' 字符提取前缀。
 * 若路径中无 '/'，则整串作为分区名。
 *
 * @param path     输入的完整路径字符串（如 "cfg_partition/settings.bin"）
 * @param out      输出的分区名字符串缓冲区
 * @param out_size 输出缓冲区大小（字节）
 * @return 0 成功，-1 表示分区名超出 out_size 导致截断
 *
 * @note 分区名截断后可能匹配到错误的 FAL 分区，故直接返回 -1 而非静默截断
 */
static int flash_extract_part_name(const char *path, char *out, size_t out_size)
{
    /* 查找第一个 '/'：之前的部分为分区名，之后为文件名 */
    const char *slash = strchr(path, '/');
    if (slash)
    {
        /* 计算分区名长度 */
        size_t len = (size_t)(slash - path);
        if (len >= out_size)
            return -1; /* 分区名超出缓冲区：拒绝截断 */

        /* 安全拷贝分区名并加尾零 */
        memcpy(out, path, len);
        out[len] = '\0';
    }
    else
    {
        /* 无 '/'：整串作为分区名 */
        size_t len = strlen(path);
        if (len >= out_size)
            return -1; /* 分区名超出缓冲区：拒绝截断 */

        memcpy(out, path, len + 1); /* +1 拷贝尾零 */
    }
    return 0;
}

/**
 * @brief Flash 后端 open 回调
 *
 * 解析路径提取 FAL 分区名 → 查找分区 → 分配 flash_handle_t。
 * 写模式额外执行全分区擦除（fal_partition_erase），确保 Flash 为空状态。
 * 擦除失败时释放已分配的句柄。
 *
 * @param io   文件 I/O 实例
 * @param path "分区名/文件名" 格式的路径
 * @param mode 读/写模式
 * @return 0 成功，-1 失败（路径无效、分区不存在、内存不足或擦除失败）
 */
static int flash_io_open(file_io_t *io, const char *path, file_io_mode_t mode)
{
    /* 参数校验 */
    if (!io || !path)
        return -1;

    /* 从路径中提取分区名（如 "cfg_partition/settings.bin" → "cfg_partition"） */
    char part_name[24];
    if (flash_extract_part_name(path, part_name, sizeof(part_name)) != 0)
        return -1;

    /* 通过 FAL API 查找分区 */
    const struct fal_partition *part = fal_partition_find(part_name);
    if (!part)
        return -1;

    /* 分配句柄并零初始化 */
    flash_handle_t *h = (flash_handle_t *)system_malloc(sizeof(*h));
    if (!h)
        return -1;
    memset(h, 0, sizeof(*h));
    h->part = part;
    h->offset = 0;
    h->mode = mode;

    /* 写模式：先全量擦除分区，确保 Flash 处于可编程状态 */
    if (mode == FILE_IO_MODE_WRITE)
    {
        int rc = fal_partition_erase(part, 0, part->len);
        if (rc < 0)
        {
            /* 擦除失败：释放句柄，不污染 io->ctx */
            system_free(h);
            return -1;
        }
    }

    /* 绑定句柄到 io->ctx */
    io->ctx = h;
    return 0;
}

/**
 * @brief Flash 后端 read 回调
 *
 * 从绝对 offset 开始读取 len 字节到 buf。
 * 自动处理越界读取：offset 超出分区时返回 0 字节，offset+len 超出时截断 len。
 *
 * @param io       文件 I/O 实例
 * @param offset   读取起始偏移（字节）
 * @param buf      输出缓冲区
 * @param len      期望读取字节数
 * @param out_read 实际读取字节数（输出参数）
 * @return 0 成功（含越界回 0），-1 失败
 */
static int flash_io_read(file_io_t *io, uint32_t offset,
                         uint8_t *buf, uint32_t len, uint32_t *out_read)
{
    flash_handle_t *h = (flash_handle_t *)io->ctx;
    if (!h || !buf || !out_read)
        return -1;

    /* 偏移超出分区范围：返回 0 字节（按接口约定不算错误） */
    if (offset >= h->part->len)
    {
        *out_read = 0;
        return 0;
    }

    /* 请求长度超出分区末尾：截断到分区块末尾 */
    if (offset + len > h->part->len)
    {
        len = h->part->len - offset;
    }

    /* 调用 FAL API 读取 */
    int rc = fal_partition_read(h->part, offset, buf, len);
    if (rc < 0)
    {
        *out_read = 0;
        return -1;
    }

    *out_read = len;
    return 0;
}

/**
 * @brief Flash 后端 write 回调
 *
 * 向当前 offset 位置追加写入 len 字节数据。
 * 写入前检查容量：确保 len 不会超出 footer 之前的可用空间，
 * 且使用了安全的减法检测（防止 uint32_t 加法回绕）。
 *
 * @param io   文件 I/O 实例
 * @param data 待写入数据
 * @param len  数据长度
 * @return 0 成功（全部写入），-1 参数无效/空间不足/写入失败
 */
static int flash_io_write(file_io_t *io, const uint8_t *data, uint32_t len)
{
    flash_handle_t *h = (flash_handle_t *)io->ctx;
    if (!h || !data || len == 0)
        return -1;

    /* 容量检查（双重条件，防止加法回绕）：
       - len 本身不能超过可用空间
       - offset + len 不能超过 footer 起始地址 */
    if (len > h->part->len - FLASH_FOOTER_SIZE || h->offset > h->part->len - FLASH_FOOTER_SIZE - len)
        return -1;

    /* 调用 FAL API 写入 */
    int rc = fal_partition_write(h->part, h->offset, data, len);
    if (rc < 0)
        return -1;

    /* 递增写入偏移 */
    h->offset += len;
    return 0;
}

/**
 * @brief Flash 后端 close 回调
 *
 * 正常关闭：若为写模式且有数据写入，则将 footer 元数据写入分区末尾。
 * 无论 footer 写入成功与否，都会释放句柄并清理 io->ctx。
 *
 * footer 写入失败时，返回值会向上传递，以便上层获知数据可能未完整落盘。
 * 但句柄仍然会被释放（避免内存泄漏）。
 *
 * @param io  文件 I/O 实例
 * @return 0 成功，< 0 表示 footer 写入失败
 */
static int flash_io_close(file_io_t *io)
{
    flash_handle_t *h = (flash_handle_t *)io->ctx;
    if (!h)
    {
        io->ctx = NULL;
        return 0;
    }

    int ret = 0;

    /* 仅写模式 + 有数据写入时需要写入 footer */
    if (h->mode == FILE_IO_MODE_WRITE && h->offset > 0)
    {
        /* 构造 footer：魔数 + 版本 + 数据长度 */
        flash_footer_t footer;
        memset(&footer, 0, sizeof(footer));
        footer.magic = FLASH_FOOTER_MAGIC;
        footer.version = FLASH_FOOTER_VERSION;
        footer.data_len = h->offset;

        /* 将 footer 写入分区末尾保留区域 */
        ret = fal_partition_write(h->part, h->part->len - FLASH_FOOTER_SIZE,
                                  (const uint8_t *)&footer, sizeof(footer));
        /* 若 ret < 0，footer 写入失败，但句柄仍需释放 */
    }

    /* 释放句柄并清理 ctx */
    system_free(h);
    io->ctx = NULL;
    return ret;
}

/**
 * @brief Flash 后端 abort 回调
 *
 * 强制终止操作：擦除整块分区（丢弃所有数据），然后释放句柄。
 * 与 close 的关键区别：不写 footer，数据全部丢失。
 *
 * @param io  文件 I/O 实例
 * @return 始终返回 0
 */
static int flash_io_abort(file_io_t *io)
{
    flash_handle_t *h = (flash_handle_t *)io->ctx;
    if (!h)
    {
        io->ctx = NULL;
        return 0;
    }

    /* 擦除整块分区：放弃所有未关闭的数据 */
    fal_partition_erase(h->part, 0, h->part->len);

    /* 释放句柄并清理 ctx */
    system_free(h);
    io->ctx = NULL;
    return 0;
}

/**
 * @brief Flash 后端 get_size 回调
 *
 * 读取分区末尾 footer，校验魔数和版本号：
 * - footer 有效：返回 footer.data_len（实际写入的数据长度）
 * - footer 无效：返回分区原始总大小（part->len）
 *
 * @param io       文件 I/O 实例（未使用）
 * @param path     "分区名/文件名" 格式的路径
 * @param out_size 输出文件大小（字节）
 * @return true 成功，false 分区不存在或路径无效
 */
static bool flash_io_get_size(file_io_t *io, const char *path, uint32_t *out_size)
{
    (void)io; /* io 未使用，由路径直接定位分区 */

    /* 参数校验 */
    if (!path || !out_size)
        return false;

    /* 提取分区名 */
    char part_name[24];
    if (flash_extract_part_name(path, part_name, sizeof(part_name)) != 0)
        return false;

    /* 查找分区 */
    const struct fal_partition *part = fal_partition_find(part_name);
    if (!part)
        return false;

    /* 读取分区末尾 footer */
    flash_footer_t footer;
    fal_partition_read(part, part->len - FLASH_FOOTER_SIZE,
                       (uint8_t *)&footer, sizeof(footer));

    /* 校验 footer：魔数匹配 + 版本精确匹配 */
    if (footer.magic == FLASH_FOOTER_MAGIC && footer.version == FLASH_FOOTER_VERSION)
    {
        /* footer 有效：返回实际数据长度 */
        *out_size = footer.data_len;
    }
    else
    {
        /* footer 无效（未写入或损坏）：返回分区原始大小 */
        *out_size = (uint32_t)part->len;
    }
    return true;
}

/**
 * @brief Flash 后端 remove 回调
 *
 * 擦除整块 FAL 分区（等价于删除 Flash 上的"文件"）。
 *
 * @param io   文件 I/O 实例（未使用）
 * @param path "分区名/文件名" 格式的路径
 * @return 0 成功，-1 失败（分区不存在或擦除失败）
 */
static int flash_io_remove(file_io_t *io, const char *path)
{
    (void)io; /* io 未使用，由路径直接定位分区 */

    if (!path)
        return -1;

    /* 提取分区名 */
    char part_name[24];
    if (flash_extract_part_name(path, part_name, sizeof(part_name)) != 0)
        return -1;

    /* 查找分区并全量擦除 */
    const struct fal_partition *part = fal_partition_find(part_name);
    if (!part)
        return -1;

    return fal_partition_erase(part, 0, part->len);
}

/**
 * @brief Flash 后端虚函数表（静态常量化实例）
 *
 * 将所有 7 个回调函数指针绑定到对应的 flash_io_xxx 实现。
 */
static const file_io_ops_t g_flash_ops = {
    .open = flash_io_open,
    .read = flash_io_read,
    .write = flash_io_write,
    .close = flash_io_close,
    .get_size = flash_io_get_size,
    .remove = flash_io_remove,
    .abort = flash_io_abort,
};

/**
 * @brief 创建 Flash I/O 后端实例
 *
 * 动态分配 file_io_t，将 ops 绑定到 g_flash_ops（FAL 实现）。
 * ctx 初始为 NULL，在 open 时由 flash_io_open 分配句柄。
 *
 * @return 成功返回独立分配的 file_io_t 指针，内存不足返回 NULL
 */
file_io_t *file_io_flash_create(void)
{
    file_io_t *io = (file_io_t *)system_malloc(sizeof(*io));
    if (!io)
        return NULL;

    io->ops = &g_flash_ops;
    io->ctx = NULL;
    return io;
}

/**
 * @brief 销毁 Flash I/O 后端实例
 *
 * 释放 file_io_t 所占内存。对 NULL 指针安全。
 *
 * @param io 由 file_io_flash_create() 创建的实例
 */
void file_io_flash_destroy(file_io_t *io)
{
    system_free(io);
}
