/**
 * @file file_processing.c
 * @brief 文件处理通用逻辑实现
 *
 * 实现跨后端（文件系统 / Flash）的通用文件操作。
 * 通过 @ref file_io_t 策略模式将具体 I/O 分派到后端实现，
 * 并在调用层统一进行状态检查、参数校验和错误处理。
 *
 * @section flow 调用流程
 * @code
 *   调用方 → fp_open_for_write → fp_do_open (校验+分派) → 后端 open
 *   调用方 → fp_write          → 校验 + 分派             → 后端 write
 *   调用方 → fp_close          → 分派                    → 后端 close
 * @endcode
 */

#include "file_processing.h"
#include <string.h>
#include <stdio.h>
#include "port.h"

/**
 * @brief 创建文件处理实例
 *
 * 分配 file_processing_t 所需内存并初始化各字段。
 * 接管传入的 file_io_t 指针所有权，fp_destroy 时自动释放。
 *
 * @param io  后端实例指针（接管所有权）
 * @param dir 工作目录路径
 * @return 成功返回实例指针，失败返回 NULL
 */
file_processing_t *fp_create(file_io_t *io, const char *dir)
{
    /* 参数有效性校验：io、ops 表、dir 三者缺一不可 */
    if (!io || !io->ops || !dir)
    {
        return NULL;
    }

    /* 目录路径长度检查：防止 memcpy 越界 */
    size_t dir_len = strlen(dir);
    if (dir_len >= FP_PATH_MAX)
    {
        return NULL;
    }

    /* 通过平台统一接口分配零初始化内存 */
    file_processing_t *fp = (file_processing_t *)system_malloc(sizeof(*fp));
    if (!fp)
    {
        return NULL;
    }
    memset(fp, 0, sizeof(*fp));

    /* 绑定后端实例（接管所有权，由 fp_destroy 负责释放） */
    fp->io = io;
    fp->io->ctx = NULL;

    /* 拷贝工作目录（使用 memcpy 保证尾零，因为 dir_len 已经过校验） */
    memcpy(fp->dir_path, dir, dir_len + 1);

    /* 初始状态：空闲，等待 open 调用 */
    fp->state = FP_STATE_IDLE;
    return fp;
}

/**
 * @brief 销毁文件处理实例
 *
 * 先确保所有后端资源已清理（通过 fp_abort），再释放实例内存。
 * 对 NULL 安全。
 *
 * @param fp  实例指针（可为 NULL）
 */
void fp_destroy(file_processing_t *fp)
{
    if (!fp)
        return;

    /* 先清理后端资源（关闭文件/擦除分区） */
    fp_abort(fp);

    /* 释放 I/O 后端实例 */
    system_free(fp->io);

    /* 释放实例自身内存 */
    system_free(fp);
}

/**
 * @brief 构建完整文件路径
 *
 * 将 dir_path + "/" + filename 拼接为绝对路径。
 * 使用 snprintf 的返回值检测截断：若返回值 >= buf_size，说明输出被截断。
 *
 * @param fp       实例指针
 * @param buf      用户提供的输出缓冲区
 * @param buf_size 缓冲区容量（字节）
 * @return 成功返回 buf，截断或参数错误返回 NULL
 */
const char *fp_full_path(file_processing_t *fp, char *buf, size_t buf_size)
{
    /* 空指针及零缓冲区校验 */
    if (!fp || !buf || buf_size == 0)
        return NULL;

    int written = snprintf(buf, buf_size, "%s/%s", fp->dir_path, fp->filename);

    /* snprintf 返回值 >= buf_size 表示输出被截断；
       < 0 表示编码错误（嵌入式环境极少发生） */
    if (written < 0 || (size_t)written >= buf_size)
        return NULL;

    return buf;
}

/**
 * @brief 内部通用的文件打开函数
 *
 * 执行打开文件的完整流程：
 *   1. 参数校验（fp、filename、ops）        → FP_ERR_PARAM / FP_ERR_IO
 *   2. 状态处理：非 IDLE 时自动关闭旧文件   → 归位到 IDLE
 *   3. 文件名长度校验                        → FP_ERR_PARAM
 *   4. 拼接完整路径（fp_full_path）          → FP_ERR_PARAM
 *   5. 分派到后端 open 回调                  → FP_ERR_OPEN / FP_OK
 *
 * 调用方通过 fp_open_for_write / fp_open_for_read 传入不同的 mode。
 *
 * @param fp       实例指针
 * @param filename 文件名（不含目录前缀）
 * @param mode     打开模式（读/写）
 * @return FP_OK 表示成功，其他为错误码
 */
static int fp_do_open(file_processing_t *fp, const char *filename, file_io_mode_t mode)
{
    /* ---- 第 1 步：基础参数校验 ---- */
    if (!fp || !filename || filename[0] == '\0')
        return FP_ERR_PARAM;
    if (!fp->io->ops || !fp->io->ops->open)
        return FP_ERR_IO;

    /* ---- 第 2 步：状态检查 — 已有文件打开则拒绝 ---- */
    if (fp->state != FP_STATE_IDLE)
    {
        return FP_ERR_ALREADY_OPEN;
    }

    /* ---- 第 3 步：文件名长度校验（+1 为 '\\0'） ---- */
    size_t name_len = strlen(filename);
    if (name_len >= FP_NAME_MAX)
        return FP_ERR_PARAM;
    memcpy(fp->filename, filename, name_len + 1);

    /* ---- 第 4 步：拼接完整路径并校验是否截断 ---- */
    char full[FP_PATH_MAX];
    if (!fp_full_path(fp, full, sizeof(full)))
        return FP_ERR_PARAM;

    /* ---- 第 5 步：分派到后端 ---- */
    int rc = fp->io->ops->open(fp->io, full, mode);
    if (rc != 0)
    {
        /* 后端返回错误：记录错误类型并转入 ERROR 状态 */
        fp->last_error = FP_ERR_OPEN;
        fp->state = FP_STATE_ERROR;
        return FP_ERR_OPEN;
    }

    /* 打开成功：重置写入位置计数器 */
    fp->position = 0;
    return FP_OK;
}

/**
 * @brief 打开文件用于写入
 *
 * 调用 fp_do_open(FILE_IO_MODE_WRITE)，成功后状态转为 FP_STATE_OPEN_WRITE。
 *
 * @param fp       实例指针
 * @param filename 文件名
 * @return FP_OK 成功，其他为错误码
 */
int fp_open_for_write(file_processing_t *fp, const char *filename)
{
    int rc = fp_do_open(fp, filename, FILE_IO_MODE_WRITE);

    /* 打开成功则将状态设为"写就绪" */
    if (rc == FP_OK)
    {
        fp->state = FP_STATE_OPEN_WRITE;
    }
    return rc;
}

/**
 * @brief 打开文件用于读取
 *
 * 调用 fp_do_open(FILE_IO_MODE_READ)，成功后状态转为 FP_STATE_OPEN_READ。
 *
 * @param fp       实例指针
 * @param filename 文件名
 * @return FP_OK 成功，其他为错误码
 */
int fp_open_for_read(file_processing_t *fp, const char *filename)
{
    int rc = fp_do_open(fp, filename, FILE_IO_MODE_READ);

    /* 打开成功则将状态设为"读就绪" */
    if (rc == FP_OK)
    {
        fp->state = FP_STATE_OPEN_READ;
    }
    return rc;
}

/**
 * @brief 追加写入数据
 *
 * 在四次校验通过后，将数据写入当前文件并递增 position 计数器。
 * 校验顺序：参数 → 状态 → ops 完整性 → 写入位置溢出检测。
 *
 * @param fp   实例指针（必须处于 FP_STATE_OPEN_WRITE）
 * @param data 待写入数据
 * @param len  数据长度
 * @return FP_OK 成功，其他为错误码
 */
int fp_write(file_processing_t *fp, const uint8_t *data, uint32_t len)
{
    /* 校验 1：基础参数（fp、data、len 任一无效则拒绝） */
    if (!fp || !data || len == 0)
        return FP_ERR_PARAM;

    /* 校验 2：状态检查（非"写就绪"状态拒绝写入） */
    if (fp->state != FP_STATE_OPEN_WRITE)
        return FP_ERR_NOT_OPEN;

    /* 校验 3：后端 write 回调存在性检查 */
    if (!fp->io->ops || !fp->io->ops->write)
        return FP_ERR_IO;

    /* 校验 4：写入后 position 是否会溢出 UINT32_MAX */
    if (fp->position > UINT32_MAX - len)
        return FP_ERR_IO;

    /* 通过后端 write 回调执行实际写入 */
    int rc = fp->io->ops->write(fp->io, data, len);
    if (rc != 0)
    {
        /* 写入失败：记录错误并转入 ERROR 状态 */
        fp->last_error = FP_ERR_WRITE;
        fp->state = FP_STATE_ERROR;
        return FP_ERR_WRITE;
    }

    /* 写入成功：位置计数器递增 len 字节 */
    fp->position += len;
    return FP_OK;
}

/**
 * @brief 从指定偏移读取数据
 *
 * 在四次校验通过后，从文件的绝对偏移量 offset 开始读取 len 字节。
 * 实际读取量由后端写入 out_read。
 *
 * @param fp       实例指针（必须处于 FP_STATE_OPEN_READ）
 * @param offset   读取起始偏移（字节）
 * @param buf      输出缓冲区
 * @param len      期望读取字节数
 * @param out_read 实际读取字节数（输出参数）
 * @return FP_OK 成功，其他为错误码
 */
int fp_read(file_processing_t *fp, uint32_t offset,
            uint8_t *buf, uint32_t len, uint32_t *out_read)
{
    /* 校验 1：基础参数（fp、buf、len、out_read 任一无效则拒绝） */
    if (!fp || !buf || len == 0 || !out_read)
        return FP_ERR_PARAM;

    /* 校验 2：状态检查（非"读就绪"状态拒绝读取） */
    if (fp->state != FP_STATE_OPEN_READ)
        return FP_ERR_NOT_OPEN;

    /* 校验 3：后端 read 回调存在性检查 */
    if (!fp->io->ops || !fp->io->ops->read)
        return FP_ERR_IO;

    /* 通过后端 read 回调执行实际读取 */
    int rc = fp->io->ops->read(fp->io, offset, buf, len, out_read);
    if (rc != 0)
    {
        /* 读取失败：记录错误并转入 ERROR 状态 */
        fp->last_error = FP_ERR_READ;
        fp->state = FP_STATE_ERROR;
        return FP_ERR_READ;
    }

    return FP_OK;
}

/**
 * @brief 获取文件大小
 *
 * 查询指定文件的字节大小。此函数独立于当前打开的文件，
 * 不需要实例处于特定打开状态。
 *
 * 流程：
 *   1. 校验参数和 ops 表
 *   2. 校验文件名长度（防止截断）
 *   3. 拼接完整路径
 *   4. 分派到后端 get_size 回调
 *
 * @param fp       实例指针
 * @param filename 文件名
 * @param out_size 输出文件大小（字节）
 * @return FP_OK 成功，FP_ERR_PARAM 参数无效，FP_ERR_IO 后端查询失败
 */
int fp_get_size(file_processing_t *fp, const char *filename, uint32_t *out_size)
{
    /* 校验：参数和 ops 表 */
    if (!fp || !filename || !out_size)
        return FP_ERR_PARAM;
    if (!fp->io->ops || !fp->io->ops->get_size)
        return FP_ERR_IO;

    /* 文件名长度校验并安全拷贝到临时缓冲区 */
    char temp_name[FP_NAME_MAX];
    size_t temp_len = strlen(filename);
    if (temp_len >= FP_NAME_MAX)
        return FP_ERR_PARAM;
    memcpy(temp_name, filename, temp_len + 1);

    /* 拼接完整路径 */
    char full[FP_PATH_MAX];
    int written = snprintf(full, sizeof(full), "%s/%s", fp->dir_path, temp_name);
    if (written < 0 || (size_t)written >= sizeof(full))
        return FP_ERR_PARAM;

    /* 分派到后端：Flash 通过 footer 还原，FS 通过 fs_stat */
    if (!fp->io->ops->get_size(fp->io, full, out_size))
        return FP_ERR_IO;

    return FP_OK;
}

/**
 * @brief 关闭当前文件
 *
 * 通过后端 close 回调正常关闭文件，将状态归位到 FP_STATE_IDLE。
 * 对于 Flash 后端，close 会写入 footer 元数据。
 *
 * 特殊处理：
 * - IDLE 状态时直接返回 FP_OK（幂等操作，多次关闭无害）
 * - 关闭失败后状态转入 FP_STATE_ERROR
 *
 * @param fp  实例指针
 * @return FP_OK 成功，其他为错误码
 */
int fp_close(file_processing_t *fp)
{
    if (!fp)
        return FP_ERR_PARAM;

    /* 若已在 IDLE 状态，无需操作（幂等保护） */
    if (fp->state == FP_STATE_IDLE)
        return FP_OK;

    /* 校验后端 close 回调存在性 */
    if (!fp->io->ops || !fp->io->ops->close)
        return FP_ERR_IO;

    /* 调用后端关闭文件 */
    int rc = fp->io->ops->close(fp->io);

    /* 通用层自行清理上下文指针（与后端 free 操作不同层） */
    fp->io->ctx = NULL;
    fp->state = FP_STATE_IDLE;
    fp->position = 0;

    /* 若后端关闭失败，记录错误码并转入 ERROR 状态 */
    if (rc != 0)
    {
        fp->last_error = FP_ERR_CLOSE;
        fp->state = FP_STATE_ERROR;
        return FP_ERR_CLOSE;
    }

    return FP_OK;
}

/**
 * @brief 异常终止文件操作
 *
 * 强制终止当前文件操作，调用后端 abort 回调。
 * 对于 Flash 后端，abort 会擦除分区（丢弃所有未保存数据）。
 * 对于 FS 后端，abort 会关闭文件句柄。
 *
 * 与 fp_close() 的关键区别：
 * - abort 不写 footer 元数据（Flash 后端）
 * - abort 总是返回 void（不报告错误）
 *
 * @param fp  实例指针
 */
void fp_abort(file_processing_t *fp)
{
    if (!fp)
        return;

    /* IDLE 状态无需操作 */
    if (fp->state == FP_STATE_IDLE)
        return;

    /* 若后端提供了 abort 回调，则调用之 */
    if (fp->io->ops && fp->io->ops->abort)
    {
        fp->io->ops->abort(fp->io);
    }

    /* 归位到 IDLE：清零 ctx 指针重置所有跟踪变量 */
    fp->io->ctx = NULL;
    fp->state = FP_STATE_IDLE;
    fp->position = 0;
}

/**
 * @brief 删除文件
 *
 * 通过后端 remove 回调删除指定文件（FS）或擦除整块 Flash 分区。
 * 不依赖当前打开状态，可随时调用。
 *
 * @param fp       实例指针
 * @param filename 文件名
 * @return FP_OK 成功，FP_ERR_PARAM 参数无效，FP_ERR_IO 删除失败
 */
int fp_remove(file_processing_t *fp, const char *filename)
{
    if (!fp || !filename)
        return FP_ERR_PARAM;
    if (!fp->io->ops || !fp->io->ops->remove)
        return FP_ERR_IO;

    /* 文件名长度校验并安全拷贝 */
    char temp_name[FP_NAME_MAX];
    size_t temp_len = strlen(filename);
    if (temp_len >= FP_NAME_MAX)
        return FP_ERR_PARAM;
    memcpy(temp_name, filename, temp_len + 1);

    /* 拼接完整路径 */
    char full[FP_PATH_MAX];
    int written = snprintf(full, sizeof(full), "%s/%s", fp->dir_path, temp_name);
    if (written < 0 || (size_t)written >= sizeof(full))
        return FP_ERR_PARAM;

    /* 分派到后端 remove 回调 */
    int rc = fp->io->ops->remove(fp->io, full);

    /* 后端约定：0 表示成功，非 0 为失败 */
    return (rc == 0) ? FP_OK : FP_ERR_IO;
}
