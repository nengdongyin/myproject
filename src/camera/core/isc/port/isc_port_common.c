#include <stdint.h>
#include "isc_port_common.h"
void isc_delay_ms(uint32_t ms)   
{ 
    (void)ms; 
}
int  isc_gpio_write(uint8_t pin, uint8_t level)
{ 
    (void)pin; 
    (void)level; 
    return 0; 
}