#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "wifi_bsp.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_timer.h"

static const char *TAG = "wifi";

/* Event group bits */
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

static EventGroupHandle_t s_wifi_event_group;
/* s_connected is read from data_task / HTTP handlers / boot_key_task and
 * written from the wifi event-handler task. On the dual-core ESP32-S3 a
 * plain bool has no ordering/visibility guarantee; mark volatile so the
 * compiler doesn't cache it across the read sites. (wifi_ap.cpp uses
 * std::atomic<int> for the analogous sta_count; a bare volatile is the
 * C-friendly equivalent here and is sufficient for a single flag.) */
static volatile bool s_connected = false;
static int s_retry_count = 0;
#define MAX_RETRY 5
/* After MAX_RETRY exhausted retries, schedule a slow background reconnect
 * so the device recovers on its own when the AP comes back / signal
 * returns, instead of staying offline until the user presses Reconnect. */
static esp_timer_handle_t s_reconnect_timer = NULL;
#define AUTO_RECONNECT_DELAY_MS 30000

/* STA netif 句柄 — 由 esp_netif_create_default_wifi_sta() 创建，
 * 保存下来供 WifiBsp_GetIp() 直接查询 IP，不依赖默认 netif 假设。 */
static esp_netif_t *s_sta_netif = NULL;

/* Event handler instance handles — needed for proper unregister in Deinit */
static esp_event_handler_instance_t s_inst_any = NULL;
static esp_event_handler_instance_t s_inst_ip  = NULL;

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        if (s_retry_count < MAX_RETRY) {
            esp_wifi_connect();
            s_retry_count++;
            ESP_LOGW(TAG, "Retry connect (%d/%d)", s_retry_count, MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            ESP_LOGE(TAG, "Connect failed after %d retries", MAX_RETRY);
            /* Schedule a slow background reconnect so the device self-heals
             * when the AP comes back or signal returns, instead of staying
             * offline until the user opens the WiFi page and presses
             * Reconnect. The timer is one-shot; if it fires and the AP is
             * still down, the retries will run again and re-arm this. */
            if (s_reconnect_timer) {
                esp_timer_stop(s_reconnect_timer);
                esp_timer_start_once(s_reconnect_timer, AUTO_RECONNECT_DELAY_MS * 1000);
            }
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_connected = true;
        s_retry_count = 0;
        if (s_reconnect_timer) esp_timer_stop(s_reconnect_timer);
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* Background auto-reconnect: triggered by the timer after MAX_RETRY
 * consecutive failures. Resets the retry budget and kicks a fresh
 * esp_wifi_connect() — the normal event-driven retry path then runs
 * again, re-arming this timer if it fails once more. */
static void reconnect_timer_cb(void *arg)
{
    (void)arg;
    if (!s_connected) {
        s_retry_count = 0;
        ESP_LOGI(TAG, "Auto-reconnect attempt");
        esp_wifi_connect();
    }
}

void WifiBsp_Init(void)
{
    s_wifi_event_group = xEventGroupCreate();

    /* NVS init (may already be initialized) */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* esp_netif_init / event_loop_create_default are idempotent in ESP-IDF
     * v5.x (they return ESP_ERR_INVALID_STATE on a second call). We only
     * call Init once per cold boot, but guard anyway for clarity. */
    esp_netif_init();
    esp_event_loop_create_default();
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* One-shot auto-reconnect timer (created lazily here, deleted in
     * Deinit). Started from the DISCONNECTED handler after MAX_RETRY.
     * If creation fails we simply proceed without auto-reconnect — the
     * manual Reconnect button still works, and the event_handler guards
     * every use of s_reconnect_timer with a NULL check. */
    if (!s_reconnect_timer) {
        esp_timer_create_args_t tcfg = {};
        tcfg.callback = reconnect_timer_cb;
        tcfg.name = "wifi_reconn";
        esp_err_t terr = esp_timer_create(&tcfg, &s_reconnect_timer);
        if (terr != ESP_OK) {
            ESP_LOGW(TAG, "auto-reconnect timer create failed: %s", esp_err_to_name(terr));
            s_reconnect_timer = NULL;
        }
    }

    esp_event_handler_instance_t inst_any, inst_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &inst_any));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &inst_ip));
    s_inst_any = inst_any;
    s_inst_ip  = inst_ip;

    /* Read credentials from NVS, fallback to Kconfig defaults */
    char ssid[33] = {0};
    char password[65] = {0};
    nvs_handle_t nvs;
    size_t ssid_len = sizeof(ssid) - 1;
    size_t pass_len = sizeof(password) - 1;

    if (nvs_open("wifi", NVS_READONLY, &nvs) == ESP_OK) {
        if (nvs_get_str(nvs, "ssid", ssid, &ssid_len) != ESP_OK) {
            strncpy(ssid, CONFIG_WIFI_SSID, sizeof(ssid) - 1);
        }
        if (nvs_get_str(nvs, "password", password, &pass_len) != ESP_OK) {
            strncpy(password, CONFIG_WIFI_PASSWORD, sizeof(password) - 1);
        }
        nvs_close(nvs);
    } else {
        strncpy(ssid, CONFIG_WIFI_SSID, sizeof(ssid) - 1);
        strncpy(password, CONFIG_WIFI_PASSWORD, sizeof(password) - 1);
    }

    ESP_LOGI(TAG, "Connecting to: %s", ssid);

    wifi_config_t wifi_config = {};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Wait for connection result (non-blocking for the rest of the system) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected");
    } else {
        ESP_LOGW(TAG, "WiFi connection timeout");
    }
}

bool WifiBsp_IsConnected(void)
{
    return s_connected;
}

bool WifiBsp_SyncNtpTime(void)
{
    if (!s_connected) {
        ESP_LOGW(TAG, "Not connected, skip NTP");
        return false;
    }

    ESP_LOGI(TAG, "Starting NTP sync (UTC+8)...");

    /* Set timezone to UTC+8 (China Standard Time) */
    setenv("TZ", "CST-8", 1);
    tzset();

    /* Initialize SNTP */
    esp_sntp_config_t sntp_config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sntp_config);

    /* Wait for time sync */
    int retry = 0;
    while (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(2000)) != ESP_OK && retry < 5) {
        ESP_LOGW(TAG, "Waiting for NTP sync... (%d)", ++retry);
    }

    esp_netif_sntp_deinit();

    if (retry >= 5) {
        ESP_LOGE(TAG, "NTP sync failed");
        return false;
    }

    /* Get synced time */
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    ESP_LOGI(TAG, "NTP synced: %04d-%02d-%02d %02d:%02d:%02d",
             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);

    return true;
}

void WifiBsp_Deinit(void)
{
    /* 1. Unregister event handlers FIRST — once they're gone, the
     *    DISCONNECTED handler can no longer touch s_reconnect_timer, which
     *    closes the race between event_handler's "if non-NULL then start"
     *    and the timer teardown below. */
    if (s_inst_any) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_inst_any);
        s_inst_any = NULL;
    }
    if (s_inst_ip) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_inst_ip);
        s_inst_ip = NULL;
    }

    /* 2. Stop + delete the auto-reconnect timer BEFORE esp_wifi_deinit,
     *    otherwise the timer callback could fire mid-teardown and call
     *    esp_wifi_connect() against a half-deinitialized driver. NULL the
     *    handle after delete so any stray reader sees NULL. */
    if (s_reconnect_timer) {
        esp_timer_handle_t t = s_reconnect_timer;
        s_reconnect_timer = NULL;   /* visible to any reader before delete */
        esp_timer_stop(t);
        esp_timer_delete(t);
    }

    /* 3. Disconnect + stop + deinit WiFi driver */
    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_deinit();

    /* 4. Destroy the STA netif so a later Init recreates it cleanly
     *    (otherwise the old esp_netif object leaks across Deinit/Init
     *    cycles). */
    if (s_sta_netif) {
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = NULL;
    }

    /* 5. Free event group */
    if (s_wifi_event_group) {
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
    }

    s_connected = false;
    s_retry_count = 0;
    ESP_LOGI(TAG, "WiFi deinit done");
}

esp_err_t WifiBsp_SetCredentials(const char *ssid, const char *password)
{
    if (!ssid || strlen(ssid) == 0) return ESP_ERR_INVALID_ARG;
    /* Bound the inputs so the values we persist into NVS actually fit the
     * wifi_config_t fields that load them later. sta.ssid is 32 bytes
     * total (incl. NUL) → 31 usable chars; .password is 64 total → 63.
     * Without this, oversized values are stored and then silently fail to
     * load back into the smaller stack buffers in Reconnect(). */
    if (strlen(ssid) > 31) return ESP_ERR_INVALID_ARG;
    if (password && strlen(password) > 63) return ESP_ERR_INVALID_ARG;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open("wifi", NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    err = nvs_set_str(nvs, "ssid", ssid);
    if (err != ESP_OK) { nvs_close(nvs); return err; }

    /* password can be empty (open network) */
    const char *pass = (password) ? password : "";
    err = nvs_set_str(nvs, "password", pass);
    nvs_commit(nvs);
    nvs_close(nvs);

    ESP_LOGI(TAG, "Credentials saved: SSID=%s", ssid);
    return err;
}

esp_err_t WifiBsp_Reconnect(void)
{
    /* Read credentials from NVS */
    char ssid[33] = {0};
    char password[65] = {0};
    nvs_handle_t nvs;
    size_t ssid_len = sizeof(ssid) - 1;
    size_t pass_len = sizeof(password) - 1;

    if (nvs_open("wifi", NVS_READONLY, &nvs) == ESP_OK) {
        if (nvs_get_str(nvs, "ssid", ssid, &ssid_len) != ESP_OK) {
            strncpy(ssid, CONFIG_WIFI_SSID, sizeof(ssid) - 1);
        }
        if (nvs_get_str(nvs, "password", password, &pass_len) != ESP_OK) {
            strncpy(password, CONFIG_WIFI_PASSWORD, sizeof(password) - 1);
        }
        nvs_close(nvs);
    } else {
        strncpy(ssid, CONFIG_WIFI_SSID, sizeof(ssid) - 1);
        strncpy(password, CONFIG_WIFI_PASSWORD, sizeof(password) - 1);
    }

    /* Disconnect and reconnect */
    s_connected = false;
    s_retry_count = 0;

    esp_wifi_disconnect();

    wifi_config_t wifi_config = {};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    esp_wifi_connect();

    ESP_LOGI(TAG, "Reconnecting to: %s", ssid);
    return ESP_OK;
}

esp_err_t WifiBsp_GetSsid(char *buf, size_t buf_len)
{
    if (!buf || buf_len == 0) return ESP_ERR_INVALID_ARG;

    nvs_handle_t nvs;
    size_t len = buf_len - 1;

    if (nvs_open("wifi", NVS_READONLY, &nvs) == ESP_OK) {
        esp_err_t err = nvs_get_str(nvs, "ssid", buf, &len);
        nvs_close(nvs);
        if (err == ESP_OK) return ESP_OK;
    }

    /* Fallback to Kconfig default */
    strncpy(buf, CONFIG_WIFI_SSID, buf_len - 1);
    buf[buf_len - 1] = '\0';
    return ESP_OK;
}

esp_err_t WifiBsp_GetIp(char *buf, size_t buf_len)
{
    if (!buf || buf_len == 0) return ESP_ERR_INVALID_ARG;
    buf[0] = '\0';

    /* 未连接直接返回 INVALID_STATE，避免 esp_netif_get_ip_info 返回全 0 地址
     * 被格式化成 "0.0.0.0"。 */
    if (!s_connected) return ESP_ERR_INVALID_STATE;

    /* 用 esp_netif_get_default netif 拿 STA 的 IP_INFO。
     * STA netif 在 WifiBsp_Init 里由 esp_netif_create_default_wifi_sta() 创建，
     * 是默认 netif，esp_netif_get_ip_info_by_index 也行，这里走通用接口。 */
    esp_netif_t *netif = s_sta_netif;
    if (!netif) return ESP_FAIL;

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
        return ESP_FAIL;
    }

    /* ip4_addr_get_u32 拿到主机序的 32 位地址；逐字节拆解成点分十进制。
     * 不用 IP2STR/IPSTR 是因为它们面向 ip4_addr_t 结构且宏展开复杂，
     * 这里直接按字节格式化更直观、可移植。 */
    uint32_t ip = ip_info.ip.addr;
    snprintf(buf, buf_len, "%u.%u.%u.%u",
             (unsigned)(ip & 0xFF),
             (unsigned)((ip >> 8) & 0xFF),
             (unsigned)((ip >> 16) & 0xFF),
             (unsigned)((ip >> 24) & 0xFF));
    return ESP_OK;
}
