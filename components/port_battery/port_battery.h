#ifndef PORT_BATTERY_H
#define PORT_BATTERY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void PortBattery_Init(void);
float PortBattery_GetVoltage(void);
uint8_t PortBattery_GetLevel(void);

#ifdef __cplusplus
}
#endif

#endif
