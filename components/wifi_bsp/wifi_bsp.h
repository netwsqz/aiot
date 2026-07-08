#ifndef WIFI_BSP_H
#define WIFI_BSP_H

#include <stdbool.h>
#include <esp_err.h>

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
void WifiBsp_Deinit(void);

/**
 * Save WiFi credentials to NVS. Takes effect on next WifiBsp_Init()
 * or after calling WifiBsp_Reconnect().
 */
esp_err_t WifiBsp_SetCredentials(const char *ssid, const char *password);

/**
 * Disconnect current STA, reconnect using NVS credentials.
 * Returns ESP_OK if reconnection initiated (check WifiBsp_IsConnected later).
 */
esp_err_t WifiBsp_Reconnect(void);

/** Get current SSID from NVS (or Kconfig default). Caller provides buffer. */
esp_err_t WifiBsp_GetSsid(char *buf, size_t buf_len);

/* 获取当前 STA 拿到的 IPv4 地址（点分十进制字符串，如 "192.168.1.10"）。
 * 未连接 / 未拿到 IP 时返回 ESP_ERR_INVALID_STATE，buf 仍以 '\0' 结尾。
 * buf_len 建议 >= 16。 */
esp_err_t WifiBsp_GetIp(char *buf, size_t buf_len);

#ifdef __cplusplus
}
#endif

#endif
