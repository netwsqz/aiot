#ifndef PORT_RTC_H
#define PORT_RTC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint8_t week;   /* 0=Sun, 1=Mon ... 6=Sat */
} rtc_datetime_t;

void PortRtc_Init(void);
void PortRtc_SetTime(uint16_t year, uint8_t month, uint8_t day,
                     uint8_t hour, uint8_t minute, uint8_t second);
rtc_datetime_t PortRtc_GetTime(void);

#ifdef __cplusplus
}
#endif

#endif
