/**
 * @file file_processing.h
 * @brief 文件处理公共接口
 *
 * 提供跨存储后端（文件系统 / Flash）的通用文件操作 API。
 * 通过 @ref file_io_t 策略模式将通用状态机逻辑与具体存储 I/O 实现完全解耦，
 * 支持打开、读取、写入、关闭、删除、获取大小等操作。
 *
 * @section state_machine 状态机设计
 * 文件处理实例维护以下状态转换：
 * @verbatim
 *   IDLE ──open(w)──▶ OPEN_WRITE ──close/abort──▶ IDLE
 *   IDLE ──open(r)──▶ OPEN_READ  ──close/abort──▶ IDLE
 *   任意状态 ──错误──▶ ERROR     ──close/abort──▶ IDLE
 * @endverbatim
 * 在非 IDLE 状态下调用 open 返回 FP_ERR_ALREADY_OPEN，必须先关闭当前文件。
 *
 * @section error_handling 错误处理
 * - 所有操作在非法状态下返回对应错误码（如未打开时写入返回 FP_ERR_NOT_OPEN）
 * - 发生 I/O 错误后，状态机转入 FP_STATE_ERROR，last_error 记录最后一次错误
 * - 调用方可通过 fp_abort() 无条件回 IDLE，丢弃未写入的数据
 */

#ifndef FILE_PROCESSING_H
#define FILE_PROCESSING_H

#include <stdint.h>
#include <stdbool.h>
#include "file_io_policy.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** @brief 最大路径长度（目录 + "/" + 文件名 + '\\0'），单位：字节 */
#define FP_PATH_MAX 256

/** @brief 最大文件名长度（不含目录前缀和 '\\0'），单位：字节 */
#define FP_NAME_MAX 128

    /**
     * @brief 文件处理状态枚举
     *
     * 表示文件处理实例当前所处的生命周期阶段，由所有公开 API 按状态机规则检查和转换。
     */
    typedef enum
    {
        FP_STATE_IDLE = 0,       /**< 空闲状态 — 无文件打开，可执行 open 或 get_size/remove */
        FP_STATE_OPEN_READ = 1,  /**< 读就绪 — 文件已打开用于读取，可执行 fp_read */
        FP_STATE_OPEN_WRITE = 2, /**< 写就绪 — 文件已打开用于写入，可执行 fp_write */
        FP_STATE_ERROR = 3,      /**< 错误状态 — 上次操作出错，需 close/abort 后恢复 */
    } fp_state_t;

    /**
     * @brief 文件操作返回码枚举
     *
     * 所有以 int 返回的公开 API 使用此枚举值。
     * FP_OK (=0) 表示成功，其余正数表示对应的错误类型。
     */
    typedef enum
    {
        FP_OK = 0,           /**< 操作成功 */
        FP_ERR_OPEN,         /**< 打开文件失败（后端 open 返回错误或文件被锁定） */
        FP_ERR_READ,         /**< 读取文件失败（FS 读取错误 / Flash 读取出错） */
        FP_ERR_WRITE,        /**< 写入文件失败（空间不足 / 硬件错误） */
        FP_ERR_CLOSE,        /**< 关闭文件失败（flush 失败 / footer 写入失败） */
        FP_ERR_NOT_OPEN,     /**< 文件未打开 — 在非读写状态下调用了 read/write */
        FP_ERR_PARAM,        /**< 参数无效 — 空指针、零长度、路径超长等 */
        FP_ERR_IO,           /**< 通用 I/O 错误 — 后端 ops 表不完整或后端内部失败 */
        FP_ERR_ALREADY_OPEN, /**< 文件已打开 — 必须先关闭当前文件才能打开新文件 */
    } fp_result_t;

    /**
     * @brief 文件处理上下文结构体
     *
     * 封装了一次文件操作会话所需的全部状态信息：
     * - I/O 后端接口（策略模式多态分派）
     * - 当前工作目录与文件名
     * - 文件大小与位置跟踪
     * - 状态机状态与错误记录
     *
     * @note 此结构体由 fp_create() 动态分配，必须由 fp_destroy() 释放。
     *       不可在栈上创建或手动 memset 初始化的实例。
     */
    typedef struct file_processing
    {
        file_io_t *io;              /**< I/O 后端实例指针（策略模式核心：ops 虚函数表 + ctx 私有数据）
                                         由 fp_create() 接管所有权，fp_destroy() 时自动释放 */
        char dir_path[FP_PATH_MAX]; /**< 工作目录路径（如 "/data" 或 "cfg_partition"） */
        char filename[FP_NAME_MAX]; /**< 当前打开的文件名（不含目录前缀） */
        uint32_t file_size;         /**< 当前文件的总字节数（由 fp_get_size 或 open 填充） */
        uint32_t position;          /**< 当前写入位置偏移（追加写入时自动递增） */
        fp_state_t state;           /**< 状态机当前状态 */
        fp_result_t last_error;     /**< 最后一次失败操作的具体错误码 */
    } file_processing_t;

    /**
     * @brief 创建文件处理实例
     *
     * 分配并初始化 file_processing_t 实例。接管传入的 file_io_t 所有权：
     * fp_destroy() 时会自动释放该 io 指针。
     * 初始状态为 FP_STATE_IDLE。
     *
     * @param io I/O 后端实例指针（由 file_io_fs_create() / file_io_flash_create() 返回）
     *           不可为 NULL，且其 ops 成员也不可为 NULL
     * @param dir 工作目录路径字符串（FS 后端为绝对路径，Flash 后端为分区名）
     *            不可为 NULL，长度必须小于 FP_PATH_MAX
     * @return 成功返回已零初始化的实例指针
     * @retval NULL 参数无效、目录路径超长或内存分配失败
     *
     * @note 返回的实例必须通过 fp_destroy() 释放，不可直接用 free()
     */
    file_processing_t *fp_create(file_io_t *io, const char *dir);

    /**
     * @brief 销毁文件处理实例
     *
     * 先调用 fp_abort() 确保所有资源已清理，再释放 io 后端实例和自身内存。
     * 对 NULL 指针安全（直接返回）。
     *
     * @param fp 由 fp_create() 创建的实例指针，可为 NULL
     */
    void fp_destroy(file_processing_t *fp);

    /**
     * @brief 构建完整文件路径
     *
     * 将工作目录、斜杠分隔符、当前文件名拼接为完整路径，写入用户提供的缓冲区。
     *
     * @param fp       实例指针
     * @param buf      输出缓冲区（由调用方提供）
     * @param buf_size 缓冲区大小（字节）
     * @return 成功返回 buf 指针
     * @retval NULL fp 或 buf 为 NULL、buf_size 为 0、或拼接结果超出 buf_size
     */
    const char *fp_full_path(file_processing_t *fp, char *buf, size_t buf_size);

    /**
     * @brief 打开文件用于写入
     *
     * 内部调用 fp_do_open() 以 FILE_IO_MODE_WRITE 模式打开文件。
     * 若实例非 IDLE 状态，会先自动关闭旧文件。
     * 文件名超长（>= FP_NAME_MAX）或路径超长均返回 FP_ERR_PARAM。
     *
     * @param fp       实例指针
     * @param filename 文件名（不含目录前缀），长度必须 < FP_NAME_MAX
     * @return FP_OK 表示成功，其他为错误码
     */
    int fp_open_for_write(file_processing_t *fp, const char *filename);

    /**
     * @brief 打开文件用于读取
     *
     * 内部调用 fp_do_open() 以 FILE_IO_MODE_READ 模式打开文件。
     * 行为同 fp_open_for_write()。
     *
     * @param fp       实例指针
     * @param filename 文件名（不含目录前缀），长度必须 < FP_NAME_MAX
     * @return FP_OK 表示成功，其他为错误码
     */
    int fp_open_for_read(file_processing_t *fp, const char *filename);

    /**
     * @brief 写入数据（追加模式）
     *
     * 将 data 缓冲区中的 len 字节追加写入到当前文件。
     * 写入成功后 position 自动递增 len。
     *
     * @param fp   实例指针（必须处于 FP_STATE_OPEN_WRITE 状态）
     * @param data 待写入数据缓冲区（不可为 NULL）
     * @param len  待写入字节数（必须 > 0，且 position + len 不可溢出 UINT32_MAX）
     * @return FP_OK 表示成功，其他为错误码
     *
     * @note 写入失败后状态变为 FP_STATE_ERROR，需 close/abort 后重新 open
     * @note Flash 后端会检查剩余空间（扣除 FLASH_FOOTER_SIZE 的 footer 区域）
     */
    int fp_write(file_processing_t *fp, const uint8_t *data, uint32_t len);

    /**
     * @brief 从指定偏移读取数据
     *
     * 以绝对偏移量 offset 从文件读取 len 字节到 buf，
     * 实际读取的字节数通过 out_read 返回。
     *
     * @param fp       实例指针（必须处于 FP_STATE_OPEN_READ 状态）
     * @param offset   读取起始偏移（字节）
     * @param buf      输出缓冲区（不可为 NULL）
     * @param len      期望读取字节数（必须 > 0）
     * @param out_read 实际读取字节数（输出参数，不可为 NULL）
     * @return FP_OK 表示成功，其他为错误码
     */
    int fp_read(file_processing_t *fp, uint32_t offset,
                uint8_t *buf, uint32_t len, uint32_t *out_read);

    /**
     * @brief 获取文件大小
     *
     * 查询指定文件（通过文件名 + 工作目录拼接）的总字节数。
     * 此操作独立于当前打开的文件，不要求实例处于特定状态，
     * 但需要后端提供有效的 get_size 回调。
     *
     * @param fp       实例指针
     * @param filename 文件名（不含目录前缀），长度必须 < FP_NAME_MAX
     * @param out_size 输出文件大小（字节），不可为 NULL
     * @return FP_OK 表示成功，其他为错误码
     *
     * @note Flash 后端：通过分区末尾 footer 中的 data_len 还原实际数据长度；
     *       若 footer 无效（magic/version 不匹配），返回分区原始大小
     * @note FS 后端：通过 fs_stat() 查询文件系统中的元数据
     */
    int fp_get_size(file_processing_t *fp, const char *filename, uint32_t *out_size);

    /**
     * @brief 关闭当前打开的文件
     *
     * 调用后端 close 回调正常关闭文件。Flash 后端会在此写入 footer 元数据。
     * 关闭后状态归位到 FP_STATE_IDLE，position 清零。
     *
     * @param fp  实例指针
     * @return FP_OK 表示成功，其他为错误码
     * @note 当前状态为 IDLE 时直接返回 FP_OK（幂等操作）
     */
    int fp_close(file_processing_t *fp);

    /**
     * @brief 异常终止（关闭并清理，不写元数据）
     *
     * 无条件终止当前文件操作：调用后端 abort 回调，重置状态为 IDLE。
     * 与 fp_close() 的区别：
     * - abort 不保证数据完整落盘（Flash 后端会擦除分区）
     * - abort 对任何状态均有效（IDLE 状态直接返回）
     *
     * @param fp  实例指针
     */
    void fp_abort(file_processing_t *fp);

    /**
     * @brief 删除文件
     *
     * 通过后端 remove 回调删除指定文件（FS 后端）或擦除整块 Flash 分区。
     * 此操作独立于当前打开的文件，不要求实例处于特定状态。
     *
     * @param fp       实例指针
     * @param filename 文件名（不含目录前缀），长度必须 < FP_NAME_MAX
     * @return FP_OK 表示成功，FP_ERR_IO 表示失败
     */
    int fp_remove(file_processing_t *fp, const char *filename);

#ifdef __cplusplus
}
#endif

#endif /* FILE_PROCESSING_H */
