#ifndef WIFI_AP_H
#define WIFI_AP_H

#include <stdbool.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_AP_SSID        "AIoT-TODO"
#define WIFI_AP_CHANNEL     1
#define WIFI_AP_MAX_CONN    4
#define WIFI_AP_TIMEOUT_MS  (15 * 60 * 1000)  // 15 分钟

typedef enum {
    WIFI_AP_STATE_IDLE,
    WIFI_AP_STATE_ACTIVE,
} wifi_ap_state_t;

typedef void (*wifi_ap_state_cb_t)(wifi_ap_state_t state);

esp_err_t WifiAp_Init(void);
esp_err_t WifiAp_Start(void);
esp_err_t WifiAp_Stop(void);
bool WifiAp_IsActive(void);
void WifiAp_ResetTimeout(void);
void WifiAp_SetStateCallback(wifi_ap_state_cb_t cb);

/* 当前连接到 AP 热点的站点数（基于 WIFI_EVENT_AP_STACONNECTED /
 * AP_STADISCONNECTED 计数，AP 未开启时返回 0）。 */
int WifiAp_GetStaCount(void);

#ifdef __cplusplus
}
#endif

#endif
