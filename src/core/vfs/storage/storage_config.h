/**
 * @file storage_config.h
 * @brief 存储资源定义入口 —— 设备 / 分区 / 挂载点集中于此（需求7）
 *
 * 该模块只做"资源 + 装配"，不含文件操作逻辑：定义存储器、分区几何、以及
 * 文件系统类型到挂载点的映射。换板子/改分区只动 storage_config.c。
 */

#ifndef STORAGE_CONFIG_H
#define STORAGE_CONFIG_H

#ifdef __cplusplus
extern "C"
{
#endif

/** @brief 文件挂载点（littlefs，承载 ymodem 收发文件） */
#define STORAGE_MNT_FILES "/lfs"
/** @brief 裸 flash 挂载点（静态注册伪 FS） */
#define STORAGE_MNT_RAW "/raw"

/**
 * @brief 初始化并挂载所有存储卷
 * @return 0 成功，< 0 负 errno（挂载失败）
 */
int storage_config_init(void);

/** @brief 卸载所有卷（可选） */
void storage_config_deinit(void);

/**
 * @brief 按分区名格式化 littlefs 分区（破坏性，出厂/首次上电 provisioning 用）
 * @param bdev_name 分区名（s_parts 里的名字，如 "flash_lfs"）
 * @return 0 成功；-ENOENT 分区未注册；< 0 其他负 errno
 */
int storage_config_format(const char *bdev_name);

#ifdef __cplusplus
}
#endif

#endif /* STORAGE_CONFIG_H */
