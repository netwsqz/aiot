#ifndef PORT_TOUCH_H
#define PORT_TOUCH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void PortTouch_Init(void);
uint8_t PortTouch_GetPoint(uint16_t *x, uint16_t *y);

#ifdef __cplusplus
}
#endif

#endif
