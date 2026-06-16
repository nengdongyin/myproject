/**
 * @file    decoder_vsc.h
 * @brief   Decoder IP 的 VSC 适配器。
 */

#ifndef DECODER_VSC_H
#define DECODER_VSC_H

#include "vsc_core_types.h"

extern const vsc_driver_t decoder_vsc_driver;
void decoder_vsc_reset(void);

#endif
