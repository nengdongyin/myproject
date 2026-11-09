#include <stdio.h>
#include <string.h>
#include "param_manager.h"
#include "param_manager_internal.h"
#include "ip_param_manager.h"
#include "param_manager_config.h"

#define TEST_MODULE_IP2 0xF3u

int main(void)
{
    static param_range_entry_t ip_arr[100];
    static param_entry_t *ip_table[100];
    for (int i = 0; i < 100; i++) {
        memset(&ip_arr[i], 0, sizeof(ip_arr[i]));
        ip_arr[i].base.param_id = MAKE_PARAM_ID(TEST_MODULE_IP2, i);
        ip_arr[i].base.vtable   = &ip_vtable;
        ip_arr[i].type          = PARAM_TYPE_UINT;
        ip_arr[i].has_range     = 0;
        ip_arr[i].cache.u32     = (uint32_t)i;
        ip_arr[i].default_val.u32 = (uint32_t)i;
        ip_table[i] = &ip_arr[i].base;
    }
    IP_DRIVER_DEFINE(big_ip, TEST_MODULE_IP2, "BigIP", NULL, NULL, NULL, NULL, NULL, NULL);

    param_init(NULL, NULL);
    int r = ip_driver_register(&big_ip_instance, ip_table, 100);
    printf("register=%d (%s)\n", r, param_strerror(r));

    param_value_t v = { .u32 = 12345 };
    r = param_write(MAKE_PARAM_ID(TEST_MODULE_IP2, 99), v);
    printf("write=%d (%s)\n", r, param_strerror(r));

    param_entry_t *e = param_entry_find(MAKE_PARAM_ID(TEST_MODULE_IP2, 99));
    if (e) {
        const param_entry_head_t *h = (const param_entry_head_t *)e;
        printf("found id=0x%08X type=%u flags=0x%X has_range=%u min=%u max=%u\n",
               (unsigned)e->param_id, (unsigned)h->type, (unsigned)h->flags,
               (unsigned)((param_range_entry_t *)e)->has_range,
               (unsigned)((param_range_entry_t *)e)->min.u32,
               (unsigned)((param_range_entry_t *)e)->max.u32);
    } else {
        printf("entry not found!\n");
    }
    return 0;
}
