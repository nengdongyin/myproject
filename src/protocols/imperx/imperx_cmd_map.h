#ifndef IMPERX_CMD_MAP_H
#define IMPERX_CMD_MAP_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t addr;
    uint32_t param_id;
} cmd_map_entry_t;

const cmd_map_entry_t *cmd_map_lookup(uint16_t addr);

#ifdef __cplusplus
}
#endif

#endif
