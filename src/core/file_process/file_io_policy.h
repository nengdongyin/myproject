/**
 * @file file_io_policy.h
 * @brief 文件 I/O 策略抽象层接口定义
 *
 * 定义文件 I/O 后端驱动的统一接口，采用 OOP-in-C 的 vtable 模式。
 * 各存储后端（文件系统、Flash 等）通过实现 @ref file_io_ops_t 来注册其操作，
 * 上层通用文件处理模块通过 @ref file_io_t 进行多态分派。
 *
 * @section design 设计原则
 * - @ref file_io_t 作为策略上下文，通过 ops 指针（虚函数表）和 ctx 指针
 *   （后端私有数据）实现接口与实现的完全解耦
 * - 所有 I/O 操作函数（open/read/write/close/remove/abort）返回 int 类型：
 *   0 表示成功，非 0 表示失败
 * - get_size 返回 bool 类型：true 表示成功获取，false 表示失败
 *
 * @section usage 使用示例
 * @code
 *   // 文件系统后端
 *   file_io_fs_init();
 *   file_io_t *io = file_io_fs_create();
 *   file_processing_t *fp = fp_create(io, "/data");
 *
 *   // Flash 后端
 *   file_io_t *io = file_io_flash_create();
 *   file_processing_t *fp = fp_create(io, "cfg_partition");
 * @endcode
 */

#ifndef FILE_IO_POLICY_H
#define FILE_IO_POLICY_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief 文件 I/O 上下文的前向声明
     *
     * 具体的结构体定义在下方，各后端通过 ctx 字段持有私有数据。
     */
    typedef struct file_io file_io_t;

    /**
     * @brief 文件打开模式枚举
     *
     * 用于指定文件的访问方式，影响后端的 open 行为：
     * - 读模式：文件必须已存在
     * - 写模式：FS 后端会自动创建文件，Flash 后端会先擦除分区
     */
    typedef enum
    {
        FILE_IO_MODE_READ = 0,  /**< 只读模式 — 文件必须存在 */
        FILE_IO_MODE_WRITE = 1, /**< 写入模式 — 不存在则创建（FS）/ 先擦除（Flash） */
    } file_io_mode_t;

    /**
     * @brief 打开文件回调函数类型
     *
     * 后端需根据 path 和 mode 打开目标文件/分区，并将会话上下文写入 io->ctx。
     *
     * @param io   文件 I/O 实例指针，成功时需设置 io->ctx
     * @param path 文件路径（FS 后端为绝对路径，Flash 后端为 "分区名/文件名" 格式）
     * @param mode 打开模式（读/写）
     * @return 0 表示成功，非 0 表示失败
     */
    typedef int (*file_io_open_fn)(file_io_t *io, const char *path, file_io_mode_t mode);

    /**
     * @brief 读取文件回调函数类型
     *
     * 从指定 offset 开始读取 len 字节到 buf，实际读取量写入 out_read。
     *
     * @param io       文件 I/O 实例指针
     * @param offset   读取起始偏移（字节）
     * @param buf      输出缓冲区
     * @param len      期望读取的字节数
     * @param out_read 实际读取的字节数（输出参数）
     * @return 0 表示成功，非 0 表示失败
     * @note   当 offset 超出文件大小时，out_read 应为 0 且返回成功
     */
    typedef int (*file_io_read_fn)(file_io_t *io, uint32_t offset,
                                   uint8_t *buf, uint32_t len, uint32_t *out_read);

    /**
     * @brief 写入文件回调函数类型
     *
     * 向文件末尾或当前位置追加写入 data 缓冲区中的 len 字节。
     *
     * @param io   文件 I/O 实例指针
     * @param data 待写入数据缓冲区
     * @param len  待写入字节数
     * @return 0 表示成功（全部写入），非 0 表示失败
     */
    typedef int (*file_io_write_fn)(file_io_t *io, const uint8_t *data, uint32_t len);

    /**
     * @brief 关闭文件回调函数类型
     *
     * 正常关闭文件，Flash 后端会在此写入 footer 元数据。
     * 关闭后应释放 io->ctx 持有的资源并置 NULL。
     *
     * @param io  文件 I/O 实例指针
     * @return 0 表示成功，非 0 表示失败
     */
    typedef int (*file_io_close_fn)(file_io_t *io);

    /**
     * @brief 获取文件大小回调函数类型
     *
     * 查询指定路径文件/分区的大小。Flash 后端会通过 footer 还原实际数据长度。
     *
     * @param io       文件 I/O 实例指针（部分实现可能不需要，用 (void)io 消除警告）
     * @param path     文件路径
     * @param out_size 输出文件大小（字节）
     * @return true 成功，false 失败（文件不存在或读取错误）
     */
    typedef bool (*file_io_get_size_fn)(file_io_t *io, const char *path, uint32_t *out_size);

    /**
     * @brief 删除文件回调函数类型
     *
     * 删除指定路径的文件（FS）或擦除整块分区（Flash）。
     *
     * @param io   文件 I/O 实例指针
     * @param path 文件路径
     * @return 0 表示成功，非 0 表示失败
     */
    typedef int (*file_io_remove_fn)(file_io_t *io, const char *path);

    /**
     * @brief 异常终止回调函数类型
     *
     * 强制终止当前操作，关闭文件并清理临时资源。
     * 与 close 的区别在于：Flash 后端不写 footer，而是直接擦除分区。
     *
     * @param io  文件 I/O 实例指针
     * @return 0 表示成功
     */
    typedef int (*file_io_abort_fn)(file_io_t *io);

    /**
     * @brief 文件 I/O 操作虚函数表
     *
     * 每个后端需提供此结构体的静态常量化实例（如 g_fs_ops / g_flash_ops），
     * 将所有函数指针绑定到对应的实现函数。上层通过此表进行多态分派。
     *
     * 所有字段均为必填（不可为 NULL），否则上层会返回 FP_ERR_IO。
     */
    typedef struct
    {
        file_io_open_fn open;         /**< 打开文件 */
        file_io_read_fn read;         /**< 读取文件 */
        file_io_write_fn write;       /**< 写入文件 */
        file_io_close_fn close;       /**< 关闭文件（正常关闭，可能写元数据） */
        file_io_get_size_fn get_size; /**< 获取文件大小 */
        file_io_remove_fn remove;     /**< 删除文件/擦除分区 */
        file_io_abort_fn abort;       /**< 异常终止（清理但不写元数据） */
    } file_io_ops_t;

    /**
     * @brief 文件 I/O 实例（策略上下文）
     *
     * 作为策略模式的核心上下文结构，封装了虚函数表指针和后端私有数据。
     * 上层 file_processing_t 持有此结构的一个成员，调用时通过 io.ops->xxx(&io, ...) 分派。
     */
    struct file_io
    {
        const file_io_ops_t *ops; /**< 指向后端实现的虚函数表（不可为 NULL） */
        void *ctx;                /**< 后端私有上下文指针（fs_fd_t* / flash_handle_t* 等） */
    };

#ifdef __cplusplus
}
#endif

#endif /* FILE_IO_POLICY_H */
