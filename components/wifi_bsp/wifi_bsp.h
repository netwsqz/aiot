#ifndef WIFI_BSP_H
#define WIFI_BSP_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize WiFi STA, connect using NVS-stored credentials
 * (falls back to Kconfig defaults). Non-blocking — spawns internal task.
 */
void WifiBsp_Init(void);

/** Returns true if WiFi is connected and has an IP. */
bool WifiBsp_IsConnected(void);

/**
 * Sync time via SNTP, then write to RTC.
 * Blocks until NTP response received (up to 10s).
 * Returns true on success.
 */
bool WifiBsp_SyncNtpTime(void);

#ifdef __cplusplus
}
#endif

#endif
