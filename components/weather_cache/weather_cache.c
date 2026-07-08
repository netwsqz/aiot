#include "weather_cache.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "weather_cache";
static nvs_handle_t s_nvs_handle;
static bool s_inited;

esp_err_t WeatherCache_Init(void)
{
    if (s_inited) return ESP_OK;

    /* nvs_flash_init() is idempotent — main.cpp may already have called it. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_open("weather", NVS_READWRITE, &s_nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open weather failed: %s", esp_err_to_name(err));
        return err;
    }

    s_inited = true;
    return ESP_OK;
}

void WeatherCache_Save(const weather_snapshot_t *snapshot)
{
    if (!s_inited || !snapshot) return;

    /* Ensure version stamp is current before persisting. */
    weather_snapshot_t copy = *snapshot;
    copy.version = WEATHER_CACHE_VERSION;

    nvs_set_blob(s_nvs_handle, "snapshot", &copy, sizeof(copy));
    nvs_set_u8(s_nvs_handle, "valid", 1);
    nvs_commit(s_nvs_handle);
    ESP_LOGI(TAG, "weather cache saved (%u bytes)", (unsigned)sizeof(copy));
}

bool WeatherCache_Load(weather_snapshot_t *snapshot)
{
    if (!s_inited || !snapshot) return false;

    uint8_t valid = 0;
    if (nvs_get_u8(s_nvs_handle, "valid", &valid) != ESP_OK || !valid) {
        return false;
    }

    size_t blob_size = 0;
    esp_err_t err = nvs_get_blob(s_nvs_handle, "snapshot", NULL, &blob_size);
    if (err != ESP_OK || blob_size != sizeof(weather_snapshot_t)) {
        ESP_LOGW(TAG, "cache blob missing or wrong size (%u vs %u expected)",
                 (unsigned)blob_size, (unsigned)sizeof(weather_snapshot_t));
        return false;
    }

    err = nvs_get_blob(s_nvs_handle, "snapshot", snapshot, &blob_size);
    if (err != ESP_OK) return false;

    if (snapshot->version != WEATHER_CACHE_VERSION) {
        ESP_LOGW(TAG, "cache version mismatch (%u vs %u)",
                 snapshot->version, (unsigned)WEATHER_CACHE_VERSION);
        return false;
    }

    return true;
}

void WeatherCache_Clear(void)
{
    if (!s_inited) return;
    /* Erase both keys; ignore "not found" which is fine on first clear. */
    nvs_erase_key(s_nvs_handle, "snapshot");
    nvs_set_u8(s_nvs_handle, "valid", 0);
    nvs_commit(s_nvs_handle);
    ESP_LOGI(TAG, "weather cache cleared");
}
