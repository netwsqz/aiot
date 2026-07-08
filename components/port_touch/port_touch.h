#ifndef PORT_TOUCH_H
#define PORT_TOUCH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void PortTouch_Init(void);
/* Remove the FT6336 device from the shared I2C bus. Called from
 * i2c_master_Deinit so the device handle doesn't leak across deep-sleep
 * cycles (i2c_del_master_bus with an attached device is undefined). */
void PortTouch_Deinit(void);
uint8_t PortTouch_GetPoint(uint16_t *x, uint16_t *y);

#ifdef __cplusplus
}
#endif

#endif
