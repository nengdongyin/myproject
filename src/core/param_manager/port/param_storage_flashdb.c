#include "param_storage_flashdb.h"
#include <stdio.h>
#include <string.h>

#ifndef USE_FLASHDB

static int stub_load(void *ctx, uint32_t id, uint8_t *d, uint16_t l)
{
    (void)ctx;
    (void)id;
    (void)d;
    (void)l;
    return -1;
}
static int stub_save(void *ctx, uint32_t id, const uint8_t *d, uint16_t l)
{
    (void)ctx;
    (void)id;
    (void)d;
    (void)l;
    return -1;
}
static int stub_erase_all(void *ctx) { (void)ctx; return 0; }
static int stub_delete(void *ctx, uint32_t id) { (void)ctx; (void)id; return 0; }
static int stub_deinit(void *ctx)
{
    (void)ctx;
    return 0;
}

static param_storage_drv_t g_stub_drv = {
    .ctx = NULL,
    .load = stub_load,
    .save = stub_save,
    .delete = stub_delete,
    .erase_all = stub_erase_all,
    .deinit = stub_deinit,
};

const param_storage_drv_t *param_storage_flashdb_get_driver(const char *part_name)
{
    (void)part_name;
    return &g_stub_drv;
}

#else

#include "flashdb.h"

#define FDB_KVDB_NAME "param_db"
#define FDB_SECTOR_SIZE 4096
#define MAX_INSTANCES 6

typedef struct
{
    bool used;
    struct fdb_kvdb kvdb;
    bool kvdb_ready;
    char part_name[24];
    param_storage_drv_t drv;
} flashdb_ctx_t;

static flashdb_ctx_t g_ctx[MAX_INSTANCES];
static bool g_fal_inited = false;

static int flashdb_init_kvdb(flashdb_ctx_t *c)
{
    uint32_t sec_size = FDB_SECTOR_SIZE;
    fdb_kvdb_control(&c->kvdb, FDB_KVDB_CTRL_SET_SEC_SIZE, &sec_size);

    fdb_err_t result = fdb_kvdb_init(&c->kvdb, FDB_KVDB_NAME,
                                     c->part_name, NULL, NULL);
    if (result != FDB_NO_ERR)
        return -1;

    c->kvdb_ready = true;
    return 0;
}

static bool kvdb_is_empty(flashdb_ctx_t *c)
{
    if (!c || !c->kvdb_ready)
        return true;

    struct fdb_kv_iterator itr;
    fdb_kv_iterator_init(&c->kvdb, &itr);
    return !fdb_kv_iterate(&c->kvdb, &itr);
}

static int flashdb_get_active_partition(void *ctx, uint8_t *index)
{
    (void)ctx;
    if (!index)
        return -1;

    const struct fal_partition *part = fal_partition_find("param_boot");
    if (!part)
        return -1;

    uint8_t val = 0xFF;
    if (fal_partition_read(part, 0, &val, 1) < 0)
        return -1;

    *index = (val < 4) ? val : 4;
    return 0;
}

static int flashdb_set_active_partition(void *ctx, uint8_t index)
{
    (void)ctx;

    const struct fal_partition *part = fal_partition_find("param_boot");
    if (!part)
        return -1;

    if (fal_partition_erase(part, 0, part->len) < 0)
        return -1;

    if (fal_partition_write(part, 0, &index, 1) < 0)
        return -1;

    return 0;
}

static const param_storage_drv_t *flashdb_create_partition(void *ctx, uint8_t index)
{
    (void)ctx;

    static const char *names[] = {
        "param_user0",
        "param_user1",
        "param_user2",
        "param_user3",
        "param_factory",
    };
    const char *name = (index < 5) ? names[index] : "param_factory";
    const param_storage_drv_t *drv = param_storage_flashdb_get_driver(name);
    if (!drv)
        return NULL;
    if (kvdb_is_empty((flashdb_ctx_t *)drv->ctx))
    {
        drv->deinit(drv->ctx);
        return NULL;
    }
    return drv;
}

static int flashdb_load(void *ctx, uint32_t param_id,
                        uint8_t *data, uint16_t len)
{
    flashdb_ctx_t *c = (flashdb_ctx_t *)ctx;
    if (!c || !c->kvdb_ready || !data || len == 0)
        return -1;

    char key[16];
    snprintf(key, sizeof(key), "p%lu", (unsigned long)param_id);

    struct fdb_blob blob;
    size_t read_len = fdb_kv_get_blob(&c->kvdb, key,
                                      fdb_blob_make(&blob, data, len));
    return (read_len > 0) ? 0 : -1;
}

static int flashdb_save(void *ctx, uint32_t param_id,
                        const uint8_t *data, uint16_t len)
{
    flashdb_ctx_t *c = (flashdb_ctx_t *)ctx;
    if (!c || !c->kvdb_ready || !data || len == 0)
        return -1;

    char key[16];
    snprintf(key, sizeof(key), "p%lu", (unsigned long)param_id);

    struct fdb_blob blob;
    fdb_err_t result = fdb_kv_set_blob(&c->kvdb, key,
                                       fdb_blob_make(&blob, data, len));
    return (result == FDB_NO_ERR) ? 0 : -1;
}

static int flashdb_delete(void *ctx, uint32_t param_id)
{
    flashdb_ctx_t *c = (flashdb_ctx_t *)ctx;
    if (!c || !c->kvdb_ready)
        return -1;

    char key[16];
    snprintf(key, sizeof(key), "p%lu", (unsigned long)param_id);
    fdb_err_t result = fdb_kv_del(&c->kvdb, key);
    return (result == FDB_NO_ERR) ? 0 : -1;
}

static int flashdb_erase_all(void *ctx)
{
    flashdb_ctx_t *c = (flashdb_ctx_t *)ctx;
    if (!c || !c->kvdb_ready)
        return -1;

    fdb_kvdb_deinit(&c->kvdb);
    c->kvdb_ready = false;

    return flashdb_init_kvdb(c);
}

static int flashdb_deinit(void *ctx)
{
    flashdb_ctx_t *c = (flashdb_ctx_t *)ctx;
    if (!c)
        return -1;
    if (c->kvdb_ready)
    {
        fdb_kvdb_deinit(&c->kvdb);
        c->kvdb_ready = false;
    }
    return 0;
}

const param_storage_drv_t *param_storage_flashdb_create(void)
{
    if (!g_fal_inited)
    {
        fal_init();
        g_fal_inited = true;
    }

    uint8_t boot_index = 0xFF;
    const struct fal_partition *boot = fal_partition_find("param_boot");
    if (boot)
        fal_partition_read(boot, 0, &boot_index, 1);

    static const char *user_parts[] = {
        "param_user0",
        "param_user1",
        "param_user2",
        "param_user3",
    };
    const char *target;
    if (boot_index < 4)
        target = user_parts[boot_index];
    else
        target = "param_factory";

    const param_storage_drv_t *drv = param_storage_flashdb_get_driver(target);
    if (!drv)
    {
        drv = param_storage_flashdb_get_driver("param_factory");
        return drv;
    }

    if (boot_index < 4 && kvdb_is_empty((flashdb_ctx_t *)drv->ctx))
    {
        drv->deinit(drv->ctx);
        drv = param_storage_flashdb_get_driver("param_factory");
        flashdb_set_active_partition(NULL, 4);
    }

    return drv;
}

const param_storage_drv_t *param_storage_flashdb_get_driver(const char *part_name)
{
    if (!part_name)
        return NULL;

    for (int i = 0; i < MAX_INSTANCES; i++)
    {
        if (g_ctx[i].used &&
            strncmp(g_ctx[i].part_name, part_name, sizeof(g_ctx[i].part_name)) == 0)
        {
            if (!g_ctx[i].kvdb_ready)
                flashdb_init_kvdb(&g_ctx[i]);
            return &g_ctx[i].drv;
        }
    }

    for (int i = 0; i < MAX_INSTANCES; i++)
    {
        if (!g_ctx[i].used)
        {
            flashdb_ctx_t *c = &g_ctx[i];
            memset(c, 0, sizeof(*c));
            strncpy(c->part_name, part_name, sizeof(c->part_name) - 1);
            c->part_name[sizeof(c->part_name) - 1] = '\0';
            c->drv.ctx = c;
            c->drv.load = flashdb_load;
            c->drv.save = flashdb_save;
            c->drv.delete = flashdb_delete;
            c->drv.erase_all = flashdb_erase_all;
            c->drv.deinit = flashdb_deinit;
            c->drv.get_active_partition = flashdb_get_active_partition;
            c->drv.set_active_partition = flashdb_set_active_partition;
            c->drv.create_partition = flashdb_create_partition;
            c->used = true;

            if (!g_fal_inited)
            {
                fal_init();
                g_fal_inited = true;
            }

            flashdb_init_kvdb(c);
            return &c->drv;
        }
    }

    return NULL;
}

#endif
