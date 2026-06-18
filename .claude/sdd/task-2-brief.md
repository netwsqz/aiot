## Task 2: wifi_ap 热点管理

**目标:** AP 热点开关 + 15 分钟超时，可独立测试

**Files:**
- Create: `components/wifi_ap/CMakeLists.txt`
- Create: `components/wifi_ap/wifi_ap.h`
- Create: `components/wifi_ap/wifi_ap.cpp`

**Interfaces:**
- Produces: `WifiAp_Init()`, `WifiAp_Start()`, `WifiAp_Stop()`, `WifiAp_IsActive()`, `WifiAp_ResetTimeout()`, `WifiAp_SetStateCallback()`

---

### Step 1: 创建 CMakeLists.txt

```cmake
idf_component_register(
    SRCS "wifi_ap.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_wifi esp_netif esp_event nvs_flash)
```

---

### Step 2: 创建 wifi_ap.h

```c
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

#ifdef __cplusplus
}
#endif

#endif
```

---

### Step 3: 创建 wifi_ap.cpp 实现

```cpp
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
                WifiAp_ResetTimeout();
                break;
            }
            case WIFI_EVENT_AP_STADISCONNECTED: {
                wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
                ESP_LOGI(TAG, "Station disconnected, AID=%d", event->aid);
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

    esp_wifi_stop();
    s_state = WIFI_AP_STATE_IDLE;

    if (s_state_cb) s_state_cb(s_state);

    ESP_LOGI(TAG, "AP stopped");
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
```

---

### Step 4: 验证编译

```bash
cd E:/aiot
idf.py build
```

**预期:** 编译通过

---

### Step 5: 提交

```bash
git add components/wifi_ap/
git commit -m "feat: add wifi_ap component for AP hotspot management"
```
