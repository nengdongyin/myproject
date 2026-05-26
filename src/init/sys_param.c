#include "param_manager.h"
#include "param_manager_internal.h"
#include "app_param_manager.h"
#include "module_ids.h"

PARAM_UINT(g_boot_index, MAKE_PARAM_ID(MODULE_SYS, 0x00), 0, 4, 0, 4);
PARAM_EXEC(g_cmd_switch, MAKE_PARAM_ID(MODULE_SYS, 0x01));

PARAM_TABLE(sys_params, &g_boot_index.base, &g_cmd_switch.base);

static int sys_apply_cb(uint32_t param_id, param_value_t value)
{
    if (param_id == MAKE_PARAM_ID(MODULE_SYS, 0x00))
        param_storage_set_active_partition((uint8_t)value.u32);
    return PARAM_OK;
}

static int sys_exec_cb(void *ctx, uint32_t param_id, param_value_t arg)
{
    (void)ctx;
    if (param_id == MAKE_PARAM_ID(MODULE_SYS, 0x01))
    {
        uint8_t idx = *(uint8_t *)arg.ptr;
        const param_storage_drv_t *drv = param_get_storage();
        const param_storage_drv_t *new_drv = drv->create_partition(drv->ctx, idx);
        if (!new_drv)
            return PARAM_OK;
        param_save_all();
        param_reload_storage(new_drv);
        return PARAM_OK;
    }
    return PARAM_ERR_NOT_FOUND;
}

static int sys_mod_init(void *ctx)
{
    (void)ctx;
    uint8_t idx = 4;
    param_storage_get_active_partition(&idx);
    param_entry_t *e = param_entry_find(MAKE_PARAM_ID(MODULE_SYS, 0x00));
    if (e)
    {
        param_value_t v = {.u32 = idx};
        *entry_cache_ptr(e) = v;
    }
    return PARAM_OK;
}

PARAM_MODULE_DEFINE(sys_mod, MODULE_SYS, "System", NULL, sys_mod_init, NULL, sys_apply_cb, sys_exec_cb, NULL);
PARAM_MODULE_INIT(sys_mod, sys_params);
