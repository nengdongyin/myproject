#include "test_common.h"
#include <string.h>

uint8_t g_blob_buf[TEST_BLOB_SIZE];
uint8_t g_blob_default[TEST_BLOB_SIZE];

mock_kv_t g_mock_kv[MOCK_STORAGE_MAX];
uint16_t g_mock_kv_count;
int g_mock_init_called;
int g_mock_deinit_called;
int g_mock_erase_called;
int g_mock_save_ret;
int g_mock_load_ret;
int g_mock_init_ret;

int g_apply_call_count;
int g_apply_last_ret;
uint32_t g_apply_last_id;
param_value_t g_apply_last_value;

int g_flush_call_count;
int g_flush_last_ret;
void *g_flush_last_ctx;

int g_init_call_count;
int g_init_last_ret;

int g_ip_read_call_count;
int g_ip_write_call_count;
int g_ip_write_last_ret;
param_value_t g_ip_write_last_value;

/* ================================================================
 *  Mock Storage
 * ================================================================ */

void mock_storage_reset(void)
{
    memset(g_mock_kv, 0, sizeof(g_mock_kv));
    g_mock_kv_count = 0;
    g_mock_init_called = 0;
    g_mock_deinit_called = 0;
    g_mock_erase_called = 0;
    g_mock_save_ret = 0;
    g_mock_load_ret = 0;
    g_mock_init_ret = 0;

    memset(g_blob_buf, 0, sizeof(g_blob_buf));
    memset(g_blob_default, 0, sizeof(g_blob_default));
}

void mock_storage_set_load_ret(int ret) { g_mock_load_ret = ret; }
void mock_storage_set_save_ret(int ret) { g_mock_save_ret = ret; }
void mock_storage_set_init_ret(int ret) { g_mock_init_ret = ret; }

void mock_storage_preset(uint32_t id, const uint8_t *data, uint16_t len)
{
    if (g_mock_kv_count >= MOCK_STORAGE_MAX)
        return;
    if (len > 64)
        return;

    mock_kv_t *kv = &g_mock_kv[g_mock_kv_count++];
    kv->id = id;
    kv->len = len;
    memcpy(kv->data, data, len);
}

static mock_kv_t *mock_storage_find(uint32_t id)
{
    for (uint16_t i = 0; i < g_mock_kv_count; i++)
    {
        if (g_mock_kv[i].id == id)
            return &g_mock_kv[i];
    }
    return NULL;
}

__attribute__((unused)) static int mock_storage_init(void *ctx)
{
    (void)ctx;
    g_mock_init_called++;
    return g_mock_init_ret;
}

__attribute__((unused)) static int mock_storage_load(void *ctx, uint32_t id, uint8_t *data, uint16_t len)
{
    (void)ctx;
    if (g_mock_load_ret != 0)
        return g_mock_load_ret;

    mock_kv_t *kv = mock_storage_find(id);
    if (!kv)
        return -1;

    uint16_t copy_len = (len < kv->len) ? len : kv->len;
    memcpy(data, kv->data, copy_len);
    if (copy_len < len)
        memset(data + copy_len, 0, len - copy_len);
    return copy_len;
}

static int mock_storage_save(void *ctx, uint32_t id, const uint8_t *data, uint16_t len)
{
    (void)ctx;
    if (g_mock_save_ret != 0)
        return g_mock_save_ret;

    mock_kv_t *kv = mock_storage_find(id);
    if (!kv)
    {
        if (g_mock_kv_count >= MOCK_STORAGE_MAX)
            return -1;
        kv = &g_mock_kv[g_mock_kv_count++];
        kv->id = id;
    }
    kv->len = (len > 64) ? 64 : len;
    memcpy(kv->data, data, kv->len);
    return 0;
}

static int mock_storage_erase_all(void *ctx)
{
    (void)ctx;
    g_mock_erase_called++;
    g_mock_kv_count = 0;
    return 0;
}

static int mock_storage_deinit(void *ctx)
{
    (void)ctx;
    g_mock_deinit_called++;
    return 0;
}

param_storage_drv_t g_mock_storage = {
    .ctx = NULL,
    .load = mock_storage_load,
    .save = mock_storage_save,
    .erase_all = mock_storage_erase_all,
    .deinit = mock_storage_deinit,
};

/* ================================================================
 *  Mock Storage 2 (独立的第二个后端, 用于多分区切换测试)
 * ================================================================ */

mock_kv_t g_mock_kv2[MOCK_STORAGE_MAX];
uint16_t g_mock_kv2_count;
int g_mock_init_called2;
int g_mock_save_ret2;

void mock_storage2_reset(void)
{
    memset(g_mock_kv2, 0, sizeof(g_mock_kv2));
    g_mock_kv2_count = 0;
    g_mock_init_called2 = 0;
    g_mock_save_ret2 = 0;
}

void mock_storage2_set_save_ret(int ret) { g_mock_save_ret2 = ret; }

static mock_kv_t *mock_storage2_find(uint32_t id)
{
    for (uint16_t i = 0; i < g_mock_kv2_count; i++)
    {
        if (g_mock_kv2[i].id == id)
            return &g_mock_kv2[i];
    }
    return NULL;
}

__attribute__((unused)) static int mock_storage2_init(void *ctx)
{
    (void)ctx;
    g_mock_init_called2++;
    return 0;
}

static int mock_storage2_load(void *ctx, uint32_t id, uint8_t *data, uint16_t len)
{
    (void)ctx;
    mock_kv_t *kv = mock_storage2_find(id);
    if (!kv)
        return -1;
    uint16_t copy_len = (len < kv->len) ? len : kv->len;
    memcpy(data, kv->data, copy_len);
    if (copy_len < len)
        memset(data + copy_len, 0, len - copy_len);
    return copy_len;
}

static int mock_storage2_save(void *ctx, uint32_t id, const uint8_t *data, uint16_t len)
{
    (void)ctx;
    if (g_mock_save_ret2 != 0)
        return g_mock_save_ret2;

    mock_kv_t *kv = mock_storage2_find(id);
    if (!kv)
    {
        if (g_mock_kv2_count >= MOCK_STORAGE_MAX)
            return -1;
        kv = &g_mock_kv2[g_mock_kv2_count++];
        kv->id = id;
    }
    kv->len = (len > 64) ? 64 : len;
    memcpy(kv->data, data, kv->len);
    return 0;
}

static int mock_storage2_erase_all(void *ctx)
{
    (void)ctx;
    return 0;
}
static int mock_storage2_deinit(void *ctx)
{
    (void)ctx;
    return 0;
}

param_storage_drv_t g_mock_storage2 = {
    .ctx = NULL,
    .load = mock_storage2_load,
    .save = mock_storage2_save,
    .erase_all = mock_storage2_erase_all,
    .deinit = mock_storage2_deinit,
};

/* ================================================================
 *  Mock Callbacks
 * ================================================================ */

void mock_apply_reset(void)
{
    g_apply_call_count = 0;
    g_apply_last_ret = PARAM_OK;
    g_apply_last_id = 0;
    memset(&g_apply_last_value, 0, sizeof(g_apply_last_value));
}

void mock_flush_reset(void)
{
    g_flush_call_count = 0;
    g_flush_last_ret = PARAM_OK;
    g_flush_last_ctx = NULL;
}

void mock_init_reset(void)
{
    g_init_call_count = 0;
    g_init_last_ret = PARAM_OK;
}

void mock_callbacks_reset(void)
{
    mock_apply_reset();
    mock_flush_reset();
    mock_init_reset();
    mock_ip_reset();
}

int mock_apply_ok(void *ctx, uint32_t param_id, param_value_t value)
{
    (void)ctx;
    g_apply_call_count++;
    g_apply_last_id = param_id;
    g_apply_last_value = value;
    g_apply_last_ret = PARAM_OK;
    return PARAM_OK;
}

int mock_apply_fail(void *ctx, uint32_t param_id, param_value_t value)
{
    (void)ctx;
    g_apply_call_count++;
    g_apply_last_id = param_id;
    g_apply_last_value = value;
    g_apply_last_ret = PARAM_ERR_INVALID_ID;
    return PARAM_ERR_INVALID_ID;
}

int mock_flush_ok(void *ctx)
{
    g_flush_call_count++;
    g_flush_last_ctx = ctx;
    g_flush_last_ret = PARAM_OK;
    return PARAM_OK;
}

int mock_flush_fail(void *ctx)
{
    g_flush_call_count++;
    g_flush_last_ctx = ctx;
    g_flush_last_ret = PARAM_ERR_FLASH_FAIL;
    return PARAM_ERR_FLASH_FAIL;
}

int mock_init_ok(void *ctx)
{
    g_init_call_count++;
    g_init_last_ret = PARAM_OK;
    return PARAM_OK;
}

/* ================================================================
 *  Mock IP Driver
 * ================================================================ */

void mock_ip_reset(void)
{
    g_ip_read_call_count = 0;
    g_ip_write_call_count = 0;
    g_ip_write_last_ret = PARAM_OK;
    memset(&g_ip_write_last_value, 0, sizeof(g_ip_write_last_value));
}

int mock_ip_read(void *drv, uint32_t param_id, param_value_t *value)
{
    (void)drv;
    (void)param_id;
    g_ip_read_call_count++;
    memset(value, 0, sizeof(*value));
    return PARAM_OK;
}

int mock_ip_write_ok(void *drv, uint32_t param_id, param_value_t value)
{
    (void)drv;
    (void)param_id;
    g_ip_write_call_count++;
    g_ip_write_last_value = value;
    g_ip_write_last_ret = PARAM_OK;
    return PARAM_OK;
}

int mock_ip_write_fail(void *drv, uint32_t param_id, param_value_t value)
{
    (void)drv;
    (void)param_id;
    g_ip_write_call_count++;
    g_ip_write_last_value = value;
    g_ip_write_last_ret = PARAM_ERR_NOT_FOUND;
    return PARAM_ERR_NOT_FOUND;
}

int mock_ip_init_ok(void *drv)
{
    (void)drv;
    g_init_call_count++;
    return PARAM_OK;
}

void test_reset_entry(param_entry_t *e)
{
    if (!e)
        return;
    *entry_cache_ptr(e) = *entry_default(e);
    entry_set_dirty(e, 0);
}
