#ifndef PARAM_EVENT_LWEVT_H
#define PARAM_EVENT_LWEVT_H

#include "param_manager.h"
#include "lwevt/lwevt.h"
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline param_value_t param_event_get_value(const lwevt_t *evt)
{
    param_value_t v;
    memcpy(&v, &evt->msg.param_changed.new_value, sizeof(v));
    return v;
}

#ifdef __cplusplus
}
#endif

#endif
