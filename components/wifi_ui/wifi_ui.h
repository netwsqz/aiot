#ifndef WIFI_UI_H_
#define WIFI_UI_H_

#include <lvgl.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* WiFi 管理页面初始化（须在 LVGL 锁内调用）。创建 200×200 白底页面根对象。
 * 该页同时显示 AP 与 STA 两侧状态，并提供 AP 启停 / STA 重连 / 刷新天气入口。
 * 进入方式：长按 BOOT（≥2s，仅进入界面，不再切换 AP）。返回方式：左滑或点击"返回"按钮。 */
void WifiUi_Init(void);

/* 返回页面根对象（首次 Init 后有效）。 */
lv_obj_t *WifiUi_GetScreen(void);

/* 推送最新状态到 UI（须在 LVGL 锁内调用）。
 * sta_connected: STA 是否拿到 IP
 * ap_active:     AP 热点是否开启
 * sta_ssid:      当前 STA SSID 字符串（可为 NULL，无连接时）
 * sta_ip:        当前 STA IPv4 点分十进制字符串（可为 NULL，无连接时）
 * ap_ssid:       AP SSID（用于显示，可为 NULL 走默认 "AIoT-TODO"）
 * ap_stas:       当前连接到 AP 的站点数 */
void WifiUi_UpdateStatus(bool sta_connected, bool ap_active,
                         const char *sta_ssid, const char *sta_ip,
                         const char *ap_ssid, int ap_stas);

/* 注册"返回主界面"回调（左滑 / 返回按钮触发）。
 * 须在 Init 前调用。cb 不可为 NULL。 */
void WifiUi_SetBackCallback(void (*cb)(void));

/* 注册"AP 启停"回调（按钮触发；按钮文字随状态切换"开启 AP"/"关闭 AP"）。
 * 须在 Init 前调用。cb 不可为 NULL。 */
void WifiUi_SetToggleApCallback(void (*cb)(void));

/* 注册"STA 重连"回调（按钮触发）。
 * 须在 Init 前调用。cb 不可为 NULL。 */
void WifiUi_SetReconnectCallback(void (*cb)(void));

/* 注册"刷新天气"回调（按钮触发）。
 * 须在 Init 前调用。cb 不可为 NULL。 */
void WifiUi_SetRefreshWeatherCallback(void (*cb)(void));

#ifdef __cplusplus
}
#endif

#endif /* WIFI_UI_H_ */
