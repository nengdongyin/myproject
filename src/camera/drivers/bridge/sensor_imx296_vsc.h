#ifndef SENSOR_IMX296_VSC_H
#define SENSOR_IMX296_VSC_H

#include "vsc_core_types.h"

typedef struct isc_dev_t isc_dev_t;

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { isc_dev_t *isc_dev; } sensor_imx296_vsc_inst_t;

extern const vsc_driver_t sensor_imx296_vsc_driver;

#ifdef __cplusplus
}
#endif

#endif
