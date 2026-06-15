/**
 * @file param_storage_mkv.h
 * @brief 基于极简 KV (mkv) 的持久化存储后端
 *
 * 用法与 param_storage_flashdb.h 一致。
 */

#ifndef PARAM_STORAGE_MKV_H
#define PARAM_STORAGE_MKV_H

#include "param_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

const param_storage_drv_t *param_storage_mkv_create(void);
const param_storage_drv_t *param_storage_mkv_get_driver(const char *part_name);

#ifdef __cplusplus
}
#endif

#endif
