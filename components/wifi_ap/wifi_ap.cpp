#include "wifi_ap.h"
#include <cstring>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "wifi_ap";

static wifi_ap_state_t s_state = WIFI_AP_STATE_IDLE;
static wifi_ap_state_cb_t s_state_cb = NULL;
static esp_timer_handle_t s_timeout_timer = NULL;
static esp_netif_t *s_ap_netif = NULL;
static bool s_initialized = false;
/* 当前关联到 AP 热点的站点数（由 STACONNECTED/STADISCONNECTED 事件维护）。
 * 用原子化的 std::atomic 在事件回调（事件循环 task）与查询（data_task / UI）间安全共享。 */
#include <atomic>
static std::atomic<int> s_sta_count{0};

static void timeout_timer_cb(void *arg)
{
    ESP_LOGI(TAG, "AP timeout, stopping...");
    WifiAp_Stop();
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_AP_STACONNECTED: {
                wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
                ESP_LOGI(TAG, "Station connected, AID=%d", event->aid);
                s_sta_count.fetch_add(1, std::memory_order_relaxed);
                WifiAp_ResetTimeout();
                break;
            }
            case WIFI_EVENT_AP_STADISCONNECTED: {
                wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
                ESP_LOGI(TAG, "Station disconnected, AID=%d", event->aid);
                /* fetch_sub 后若为负说明事件乱序，钳到 0 防止显示负数 */
                int prev = s_sta_count.fetch_sub(1, std::memory_order_relaxed);
                if (prev <= 0) s_sta_count.store(0, std::memory_order_relaxed);
                break;
            }
            default:
                break;
        }
    }
}

esp_err_t WifiAp_Init(void)
{
    if (s_initialized) return ESP_OK;

    // 创建 AP netif
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (!s_ap_netif) {
        ESP_LOGE(TAG, "Failed to create AP netif");
        return ESP_FAIL;
    }

    // 注册事件
    esp_event_handler_instance_t instance_any_id;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id));

    // 创建超时定时器
    const esp_timer_create_args_t timer_args = {
        .callback = timeout_timer_cb,
        .name = "wifi_ap_timeout"
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_timeout_timer));

    s_initialized = true;
    ESP_LOGI(TAG, "Init done");
    return ESP_OK;
}

esp_err_t WifiAp_Start(void)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_state == WIFI_AP_STATE_ACTIVE) {
        ESP_LOGW(TAG, "Already active");
        return ESP_OK;
    }

    wifi_config_t wifi_config = {};
    strncpy((char *)wifi_config.ap.ssid, WIFI_AP_SSID, sizeof(wifi_config.ap.ssid) - 1);
    wifi_config.ap.ssid_len = strlen(WIFI_AP_SSID);
    wifi_config.ap.channel = WIFI_AP_CHANNEL;
    wifi_config.ap.max_connection = WIFI_AP_MAX_CONN;
    wifi_config.ap.authmode = WIFI_AUTH_OPEN;  // 无密码

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_state = WIFI_AP_STATE_ACTIVE;
    s_sta_count.store(0, std::memory_order_relaxed);  /* 全新 AP 会话，清零站点计数 */
    WifiAp_ResetTimeout();

    if (s_state_cb) s_state_cb(s_state);

    ESP_LOGI(TAG, "AP started: %s", WIFI_AP_SSID);
    return ESP_OK;
}

esp_err_t WifiAp_Stop(void)
{
    if (s_state == WIFI_AP_STATE_IDLE) return ESP_OK;

    if (s_timeout_timer) {
        esp_timer_stop(s_timeout_timer);
    }

    /* 不调 esp_wifi_stop() —— 那是全局停 WiFi 驱动，APSTA 模式下会同时断 STA。
     * 正确做法：切回 WIFI_MODE_STA，只关 AP 接口，STA 维持原连接不动。
     * esp_wifi_set_mode() 在驱动已运行时平滑切换，不触发 STA 断开/重连。 */
    esp_wifi_set_mode(WIFI_MODE_STA);

    s_state = WIFI_AP_STATE_IDLE;
    s_sta_count.store(0, std::memory_order_relaxed);  /* AP 关闭，清零站点计数 */

    if (s_state_cb) s_state_cb(s_state);

    ESP_LOGI(TAG, "AP stopped (STA kept)");
    return ESP_OK;
}

bool WifiAp_IsActive(void)
{
    return s_state == WIFI_AP_STATE_ACTIVE;
}

void WifiAp_ResetTimeout(void)
{
    if (s_timeout_timer && s_state == WIFI_AP_STATE_ACTIVE) {
        esp_timer_stop(s_timeout_timer);
        esp_timer_start_once(s_timeout_timer, WIFI_AP_TIMEOUT_MS * 1000);
    }
}

void WifiAp_SetStateCallback(wifi_ap_state_cb_t cb)
{
    s_state_cb = cb;
}

int WifiAp_GetStaCount(void)
{
    return s_sta_count.load(std::memory_order_relaxed);
}
