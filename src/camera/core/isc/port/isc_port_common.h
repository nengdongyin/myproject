#ifndef ISC_PORT_COMMON_H
#define ISC_PORT_COMMON_H

#include <stdint.h>

void isc_delay_ms(uint32_t ms);
int isc_gpio_write(uint8_t pin, uint8_t level);


#endif /* ISC_PORT_COMMON_H */
