# 天气 + 天气预报 实现计划

> **For agentic workers:** 本计划按步骤实现，每一步有明确交付物。
>
> **目标：** 在现有 ESP32-S3 墨水屏固件上新增天气功能：主屏显示室外/室内温度摘要，独立天气页展示当前天气、逐时预报、未来预报。数据源为和风天气 QWeather。
>
> **技术栈：** ESP-IDF v5.5.4 / C / LVGL v9 / esp_http_client / cJSON / NVS / FreeRTOS

## 全局约束（来自设计文档）

- 所有 LVGL 调用必须在 `Lvgl_Lock()` / `Lvgl_Unlock()` 内执行
- `Lvgl_Lock()` 严禁包住网络请求、NVS 读写或任何可能阻塞的操作
- 页面根对象 200×200，白底，无边框、无圆角、无阴影
- 无动画，所有状态切换直接刷新
- 中文文本使用 `lv_font_chinese_14`，大温度数字使用 `lv_font_montserrat_28`
- 屏幕：`CONFIG_WEATHER_API_BASE_URL` 默认 `devapi.qweather.com`
- 缓存有效期：`CONFIG_WEATHER_CACHE_TTL_MIN` 默认 120 分钟
- REFRESH 间隔：`CONFIG_WEATHER_FETCH_INTERVAL_MIN` 默认 30 分钟
- 连续失败 3 次后刷新间隔翻倍，最大 2 小时
- HTTPS 缓解：`CONFIG_MBEDTLS_DYNAMIC_BUFFER=y`，`CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=4096`，每次请求后 `esp_http_client_cleanup()`

---

## Task 1: HTTPS 最小验证

**Goal：** 在 `wifi_bsp` 初始化后，用一个最小测试确认 `esp_http_client` + HTTPS + 真实 API 端点能打通，排除 TLS/证书/内存/网络路径上的未知问题。

**Files:** 无永久文件，验证后删除测试代码。

**Interfaces:**
- Produces: 确认 HTTPS 可以正常工作
- [ ] **Step 1：在 `main.cpp` 冷启动末尾加临时调试代码**

在 `main.cpp` 冷启动路径（已有 WiFi + NTP 之后的 `/* 8-9. Touch + Battery */` 之前）插入：

```cpp
/* -= Temp: HTTPS validation test (remove after confirm) =- */
#include "esp_http_client.h"
static void https_smoke_test(void)
{
    if (!WifiBsp_IsConnected()) { ESP_LOGW(TAG, "WiFi not connected, skip HTTPS test"); return; }
    esp_http_client_config_t cfg = {};
    cfg.url = "https://" CONFIG_WEATHER_API_BASE_URL "/v7/weather/now?location=101010100&key=test";
    cfg.method = HTTP_GET;
    cfg.timeout_ms = 10000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach; // 使用系统 CA 包
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) { ESP_LOGE(TAG, "http client init failed"); return; }
    esp_err_t err = esp_http_client_perform(cli);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(cli);
        int len = esp_http_client_get_content_length(cli);
        ESP_LOGI(TAG, "HTTPS OK: status=%d, content_len=%d", status, len);
    } else {
        ESP_LOGE(TAG, "HTTPS FAIL: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(cli);
}
```

然后在 `wifi_bsp_init` 之后加 `https_smoke_test();`。

- [ ] **Step 2：编译并串口验证**

```bash
env -u MSYSTEM powershell.exe -ExecutionPolicy Bypass -Command \
  "[Environment]::SetEnvironmentVariable('MSYSTEM', \$null, 'Process'); \
   . E:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1; \
   cd E:\aiot; idf.py build"
```

编译通过后烧录，串口观察输出：
- 期望看到 `HTTPS OK: status=200, content_len=xxx`（使用无效 key 时 API 返回 403 + JSON 错误体也可）
- 如果看到 `TLS handshake failed` 或 `crt_bundle_attach` 相关错误，说明需要额外的 CA 证书配置

- [ ] **Step 3：检查 heap 余量**

在测试函数末尾添加：

```cpp
ESP_LOGI(TAG, "Free heap after HTTPS test: %d", esp_get_free_heap_size());
ESP_LOGI(TAG, "Min free heap: %d", esp_get_minimum_free_heap_size());
```

观察 heap 变化，确保 <40KB 排除 HTTPS 可行性风险。

- [ ] **Step 4：清理临时代码**

确认 HTTPS 可正常使用后，删除所有临时调试代码。**不要提交此 commit**，此步骤纯验证。

---



## Task 2: weather_fetch 组件

**Goal：** 实现与和风天气 HTTP 客户端，解析 JSON，输出 compact `weather_snapshot_t`。

**Files:**
- Create: `components/weather_fetch/weather_fetch.h`
- Create: `components/weather_fetch/weather_fetch.c`
- Create: `components/weather_fetch/Kconfig`
- Create: `components/weather_fetch/CMakeLists.txt`

**Interfaces:**
- Consumes: 无（最底层）
- Produces: `weather_fetch.h` 中的类型定义和 API

- [ ] **Step 1：创建 Kconfig**

创建 `components/weather_fetch/Kconfig`：

```kconfig
config WEATHER_API_KEY
    string "QWeather API Key"
    default ""
    help
        和风天气 API Key。留空时天气功能不生效，主屏退回室内传感器模式。

config WEATHER_LOCATION_ID
    string "QWeather Location ID (城市编码)"
    default "101010100"
    help
        QWeather 城市编码，如北京的 101010100。可通过 https://github.com/qwd/LocationList 查询。

config WEATHER_CITY_NAME
    string "QWeather 城市显示名称"
    default "北京"
    help
        主屏和天气页显示的城市名。

config WEATHER_API_BASE_URL
    string "QWeather API Base URL"
    default "devapi.qweather.com"
    help
        开发版使用 devapi.qweather.com；付费订阅用户可改为 api.qweather.com。

config WEATHER_FETCH_INTERVAL_MIN
    int "天气刷新间隔（分钟）"
    range 10 120
    default 30
    help
        正常联网时天气拉取间隔。最小 10 分钟避免 QWeather 限流。

config WEATHER_CACHE_TTL_MIN
    int "缓存有效期（分钟）"
    range 30 720
    default 120
    help
        缓存超过此时间标注"数据过旧"，但仍显示。
```

- [ ] **Step 2：创建 CMakeLists.txt**

```cmake
idf_component_register(
    SRCS "weather_fetch.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_http_client esp_https_ota wifi_bsp nvs_flash cJSON freertos esp_event
)
```

注意：`esp_http_client` 的 `PRIV_REQUIRES` 已经包含了 mbedtls，因此 HTTPS 随 `esp_http_client` 自动可用。如果项目默认不包含 `esp_http_client`，自动链接。

- [ ] **Step 3：创建 weather_fetch.h**

```c
#ifndef WEATHER_FETCH_H_
#define WEATHER_FETCH_H_

#include <stdint.h>
#include <stdbool.h>

/* ── Data model ── */
#define WEATHER_TEXT_LEN        16
#define WEATHER_ICON_CODE_LEN   8
#define WEATHER_DATE_LEN        12
#define WEATHER_HOURLY_COUNT    4
#define WEATHER_DAILY_COUNT     2
#define WEATHER_CACHE_VERSION   1

typedef struct {
    char text[WEATHER_TEXT_LEN];
    char icon[WEATHER_ICON_CODE_LEN];
    char city_name[WEATHER_TEXT_LEN];
    int temp;
    int feels_like;
    int humidity;
    int wind_scale;
    int64_t obs_time;
} weather_now_t;

typedef struct {
    int hour;
    char icon[WEATHER_ICON_CODE_LEN];
    int temp;
} weather_hourly_t;

typedef struct {
    char date[WEATHER_DATE_LEN];
    char text_day[WEATHER_TEXT_LEN];
    char icon_day[WEATHER_ICON_CODE_LEN];
    int temp_min;
    int temp_max;
} weather_daily_t;

typedef struct {
    uint32_t version;
    weather_now_t now;
    weather_hourly_t hourly[WEATHER_HOURLY_COUNT];
    weather_daily_t daily[WEATHER_DAILY_COUNT];
    int64_t fetch_time;
} weather_snapshot_t;

/* ── API ── */
#ifdef __cplusplus
extern "C" {
#endif

/* 初始化：读取 Kconfig，无需 WiFi 连接。返回 ESP_OK/ESP_FAIL。 */
esp_err_t WeatherFetch_Init(void);

/* 返回是否具备拉取条件（API Key + Location ID + City Name 均已配置）。 */
bool WeatherFetch_IsReady(void);

/* 执行一次完整拉取（now + 24h + 3d），写入 snapshot。返回 true 表示成功。 */
bool WeatherFetch_Update(weather_snapshot_t *snapshot);

/* 返回最近一次错误描述（用于日志）。 */
const char *WeatherFetch_LastError(void);

#ifdef __cplusplus
}
#endif

#endif /* WEATHER_FETCH_H_ */
```

- [ ] **Step 4：实现 weather_fetch.c**

```c
#include "weather_fetch.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "weather_fetch";

/* Kconfig / NVS config (populated in WeatherFetch_Init) */
static char s_api_key[64];
static char s_location_id[32];
static char s_city_name[32];
static char s_base_url[64];
static int  s_fetch_interval_min;
static int  s_cache_ttl_min;

static char s_last_error[64];
static bool s_initialized;

/* snprintf with NUL-guard */
static void safe_strcpy(char *dst, size_t dst_size, const char *src)
{
    size_t len = strlen(src);
    if (len >= dst_size) len = dst_size - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static void set_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_last_error, sizeof(s_last_error), fmt, ap);
    s_last_error[sizeof(s_last_error) - 1] = '\0';
    va_end(ap);
}

/* Build a QWeather icon code into our compact icon field.
 * Discards non-digit characters, keeps first 3 digits, max 7 chars + NUL. */
static void parse_icon_code(char *out, size_t out_size, const char *raw)
{
    char buf[8] = {0};
    int j = 0;
    for (size_t i = 0; raw[i] && j < 7; i++) {
        if (raw[i] >= '0' && raw[i] <= '9') buf[j++] = raw[i];
    }
    buf[j] = '\0';
    safe_strcpy(out, out_size, buf);
}

/* Categorize icon code for selecting the right LVGL image descriptor. */
static int weather_icon_category(const char *icon_code)
{
    int code = 0;
    sscanf(icon_code, "%d", &code);
    if (code == 100) return 0; /* sun */
    if (code >= 101 && code <= 104) return 1; /* cloud */
    if (code >= 300 && code <= 399) return 2; /* rain */
    if (code >= 400 && code <= 499) return 3; /* snow */
    if (code >= 500 && code <= 509) return 4; /* fog */
    return 1; /* default cloud */
}

/* Parse cJSON parse result into weather_now_t */
static bool parse_now(weather_now_t *out, const cJSON *root)
{
    const cJSON *now = cJSON_GetObjectItem(root, "now");
    if (!cJSON_IsObject(now)) { set_error("now missing"); return false; }

    const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(now, "text"));
    const char *icon = cJSON_GetStringValue(cJSON_GetObjectItem(now, "icon"));
    safe_strcpy(out->text, sizeof(out->text), text ? text : "");
    if (icon) parse_icon_code(out->icon, sizeof(out->icon), icon);
    else out->icon[0] = '\0';

    const cJSON *temp = cJSON_GetObjectItem(now, "temp");
    out->temp = cJSON_IsNumber(temp) ? (int)cJSON_GetNumberValue(temp) : 0;

    const cJSON *fl = cJSON_GetObjectItem(now, "feelsLike");
    out->feels_like = cJSON_IsNumber(fl) ? (int)cJSON_GetNumberValue(fl) : 0;

    const cJSON *hum = cJSON_GetObjectItem(now, "humidity");
    out->humidity = cJSON_IsNumber(hum) ? (int)cJSON_GetNumberValue(hum) : 0;

    const cJSON *wind = cJSON_GetObjectItem(now, "windScale");
    out->wind_scale = cJSON_IsNumber(wind) ? (int)cJSON_GetNumberValue(wind) : 0;

    int64_t obs = 0;
    sscanf(cJSON_GetStringValue(cJSON_GetObjectItem(now, "obsTime")), "%ld", &obs);
    out->obs_time = obs;

    safe_strcpy(out->city_name, sizeof(out->city_name), s_city_name);
    return true;
}

/* Parse cJSON result into weather_hourly_t array. Returns count parsed. */
static int parse_hourly(weather_hourly_t *out, size_t max_count, const cJSON *root)
{
    const cJSON *hourly = cJSON_GetObjectItem(root, "hourly");
    if (!cJSON_IsArray(hourly)) { set_error("hourly missing"); return 0; }

    int count = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, hourly) {
        if (count >= (int)max_count) break;
        const char *fx = cJSON_GetStringValue(cJSON_GetObjectItem(item, "fxTime"));
        if (!fx) continue;
        int hour = 0;
        sscanf(fx, "%*[^T]T%2d", &hour);

        const char *icon = cJSON_GetStringValue(cJSON_GetObjectItem(item, "icon"));
        out[count].hour = hour;
        if (icon) parse_icon_code(out[count].icon, sizeof(out[count].icon), icon);
        else out[count].icon[0] = '\0';

        const cJSON *t = cJSON_GetObjectItem(item, "temp");
        out[count].temp = cJSON_IsNumber(t) ? (int)cJSON_GetNumberValue(t) : 0;
        count++;
    }
    return count;
}

/* Parse cJSON result into weather_daily_t array. Returns count parsed.
 * Skips today's entry (index 0) because we already have "now" weather. */
static int parse_daily(weather_daily_t *out, size_t max_count, const cJSON *root)
{
    const cJSON *daily = cJSON_GetObjectItem(root, "daily");
    if (!cJSON_IsArray(daily)) { set_error("daily missing"); return 0; }

    int count = 0;
    cJSON *item;
    /* Skip index 0 (today) since "now" weather covers today */
    int skip_today = 1;
    cJSON_ArrayForEach(item, daily) {
        if (skip_today > 0) { skip_today--; continue; }
        if (count >= (int)max_count) break;

        const char *date = cJSON_GetStringValue(cJSON_GetObjectItem(item, "fxDate"));
        safe_strcpy(out[count].date, sizeof(out[count].date), date ? date : "");

        const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(item, "textDay"));
        safe_strcpy(out[count].text_day, sizeof(out[count].text_day), text ? text : "");

        const char *icon = cJSON_GetStringValue(cJSON_GetObjectItem(item, "iconDay"));
        if (icon) parse_icon_code(out[count].icon_day, sizeof(out[count].icon_day), icon);
        else out[count].icon_day[0] = '\0';

        const cJSON *tmp = cJSON_GetObjectItem(item, "tempMin");
        out[count].temp_min = cJSON_IsNumber(tmp) ? (int)cJSON_GetNumberValue(tmp) : 0;

        const cJSON *tmp2 = cJSON_GetObjectItem(item, "tempMax");
        out[count].temp_max = cJSON_IsNumber(tmp2) ? (int)cJSON_GetNumberValue(tmp2) : 0;

        count++;
    }
    return count;
}

/* Perform one HTTP GET to the given path, return body into caller buffer.
 * Caller must provide buf/buf_len. Returns actual bytes read, or -1 on failure. */
static int http_get_json(const char *path, char *buf, size_t buf_len)
{
    char url[128];
    snprintf(url, sizeof(url), "https://%s%s", s_base_url, path);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_GET,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) { set_error("http client init failed"); return -1; }

    esp_err_t err = esp_http_client_open(cli, 0);
    if (err != ESP_OK) { set_error("http open failed: %s", esp_err_to_name(err)); goto cleanup; }

    /* Read response */
    int total = 0;
    while (1) {
        if (total >= (int)buf_len) break;
        int n = esp_http_client_read(cli, buf + total, buf_len - total);
        if (n <= 0) break;
        total += n;
    }
    buf[total] = '\0';

    int status = esp_http_client_get_status_code(cli);
    if (status != 200) {
        set_error("HTTP %d", status);
        total = -1;
    }

cleanup:
    esp_http_client_cleanup(cli);
    return total;
}

/* Internal fetch for a single endpoint and parse into output struct */
static bool fetch_and_parse(const char *path,
                             bool (*parser)(void *, const cJSON *),
                             void *out)
{
    char buf[4096];
    int len = http_get_json(path, buf, sizeof(buf));
    if (len < 0) return false;

    cJSON *root = cJSON_Parse(buf);
    if (!root) { set_error("JSON parse failed"); return false; }

    bool ok = parser(out, root);
    cJSON_Delete(root);
    return ok;
}

/* Parser wrappers */
static bool parse_now_adapter(void *out, const cJSON *root) { return parse_now((weather_now_t *)out, root); }

/* Public API implementation */
esp_err_t WeatherFetch_Init(void)
{
    if (s_initialized) return ESP_OK;

    /* Read Kconfig defaults */
    safe_strcpy(s_api_key, sizeof(s_api_key), CONFIG_WEATHER_API_KEY);
    safe_strcpy(s_location_id, sizeof(s_location_id), CONFIG_WEATHER_LOCATION_ID);
    safe_strcpy(s_city_name, sizeof(s_city_name), CONFIG_WEATHER_CITY_NAME);
    safe_strcpy(s_base_url, sizeof(s_base_url), CONFIG_WEATHER_API_BASE_URL);

    s_initialized = true;
    return ESP_OK;
}

bool WeatherFetch_IsReady(void)
{
    return s_initialized
        && s_api_key[0] != '\0'
        && s_location_id[0] != '\0'
        && s_city_name[0] != '\0';
}

bool WeatherFetch_Update(weather_snapshot_t *snapshot)
{
    if (!snapshot || !WeatherFetch_IsReady()) return false;
    if (!WifiBsp_IsConnected()) { set_error("WiFi not connected"); return false; }

    /* Clear snapshot first */
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->version = WEATHER_CACHE_VERSION;

    /* 1. Current weather */
    char path[96];
    snprintf(path, sizeof(path), "/v7/weather/now?location=%s&key=%s",
             s_location_id, s_api_key);
    if (!fetch_and_parse(path, parse_now_adapter, &snapshot->now)) {
        ESP_LOGW(TAG, "fetch now failed: %s", s_last_error);
        return false;
    }

    /* 2. 24h hourly */
    snprintf(path, sizeof(path), "/v7/weather/24h?location=%s&key=%s",
             s_location_id, s_api_key);
    int hourly_count = parse_hourly(snapshot->hourly, WEATHER_HOURLY_COUNT,
                                    cJSON_Parse_buf);
    /* Parse from buffer since http_get_json returns raw JSON */
    {
        char buf[4096];
        if (http_get_json(path, buf, sizeof(buf)) < 0) {
            ESP_LOGW(TAG, "fetch 24h failed: %s", s_last_error);
            return false;
        }
        cJSON *root = cJSON_Parse(buf);
        if (!root) { set_error("parse 24h JSON"); return false; }
        parse_hourly(snapshot->hourly, WEATHER_HOURLY_COUNT, root);
        cJSON_Delete(root);
    }

    /* 3. 3d daily */
    snprintf(path, sizeof(path), "/v7/weather/3d?location=%s&key=%s",
             s_location_id, s_api_key);
    {
        char buf[4096];
        if (http_get_json(path, buf, sizeof(buf)) < 0) {
            ESP_LOGW(TAG, "fetch 3d failed: %s", s_last_error);
            return false;
        }
        cJSON *root = cJSON_Parse(buf);
        if (!root) { set_error("parse 3d JSON"); return false; }
        parse_daily(snapshot->daily, WEATHER_DAILY_COUNT, root);
        cJSON_Delete(root);
    }

    snapshot->fetch_time = time(NULL);
    return true;
}

const char *WeatherFetch_LastError(void)
{
    return s_last_error;
}
```

> **代码说明：**
> - `WeatherFetch_Init()` 只读 Kconfig，不联网，可离线调用。
> - `WeatherFetch_Update()` 会串联发 3 个 HTTPS 请求，每个超时 10s。必须在非 LVGL 任务上下文中调用。
> - `http_get_json()` 小工具函数使用 `esp_crt_bundle_attach` 信任系统 CA，无需额外证书文件。
> - parse_icon_code 将 QWeather 图标码如 `"100"` 精简为纯数字字符串 `"100"`，去掉前缀。

- [ ] **Step 5：编译验证**

```bash
env -u MSYSTEM powershell.exe -ExecutionPolicy Bypass -Command \
  "[Environment]::SetEnvironmentVariable('MSYSTEM', \$null, 'Process'); \
   . E:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1; \
   cd E:\aiot; idf.py build"
```

检查编译输出没有 errors 和 `weather_fetch` 相关 warnings。

---



## Task 3: weather_cache 组件

**Goal：** 将 `weather_snapshot_t` 快照持久化到 NVS，支持深睡眠唤醒后恢复。

**Files:**
- Create: `components/weather_cache/weather_cache.h`
- Create: `components/weather_cache/weather_cache.c`
- Create: `components/weather_cache/CMakeLists.txt`

**Interfaces:**
- Consumes: `weather_fetch.h` 中的 `weather_snapshot_t` / `WEATHER_CACHE_VERSION`
- Produces: `weather_cache.h` 中的 API

- [ ] **Step 1：创建 weather_cache.h**

```c
#ifndef WEATHER_CACHE_H_
#define WEATHER_CACHE_H_

#include "weather_fetch.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化 NVS 命名空间 */
esp_err_t WeatherCache_Init(void);

/* 写入快照（fetch 成功后调用） */
void WeatherCache_Save(const weather_snapshot_t *snapshot);

/* 读取缓存快照（深睡眠唤醒时调用）。返回 true 表示读到有效缓存。 */
bool WeatherCache_Load(weather_snapshot_t *snapshot);

/* 清除缓存（城市/API Key 改变时调用） */
void WeatherCache_Clear(void);

#ifdef __cplusplus
}
#endif

#endif /* WEATHER_CACHE_H_ */
```

- [ ] **Step 2：创建 CMakeLists.txt**

```cmake
idf_component_register(
    SRCS "weather_cache.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES nvs_flash freertos weather_fetch
)
```

- [ ] **Step 3：实现 weather_cache.c**

```c
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
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;
    err = nvs_open("weather", NVS_READWRITE, &s_nvs_handle);
    if (err != ESP_OK) return err;
    s_inited = true;
    return ESP_OK;
}

void WeatherCache_Save(const weather_snapshot_t *snapshot)
{
    if (!s_inited || !snapshot) return;
    nvs_set_blob(s_nvs_handle, "snapshot", snapshot, sizeof(*snapshot));
    uint8_t valid = 1;
    nvs_set_u8(s_nvs_handle, "valid", valid);
    nvs_commit(s_nvs_handle);
    ESP_LOGI(TAG, "weather cache saved (%u bytes)", (unsigned)sizeof(*snapshot));
}

bool WeatherCache_Load(weather_snapshot_t *snapshot)
{
    if (!s_inited || !snapshot) return false;
    uint8_t valid = 0;
    nvs_get_u8(s_nvs_handle, "valid", &valid);
    if (!valid) return false;

    size_t blob_size = 0;
    esp_err_t err = nvs_get_blob(s_nvs_handle, "snapshot", NULL, &blob_size);
    if (err != ESP_OK || blob_size != sizeof(weather_snapshot_t)) {
        ESP_LOGW(TAG, "cache blob missing or wrong size (%u vs %u expected)",
                 (unsigned)blob_size, (unsigned)sizeof(weather_snapshot_t));
        return false;
    }

    err = nvs_get_blob(s_nvs_handle, "snapshot", snapshot, &blob_size);
    if (err != ESP_OK) return false;

    /* 校验版本号 */
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
    nvs_erase_key(s_nvs_handle, "snapshot");
    uint8_t invalid = 0;
    nvs_set_u8(s_nvs_handle, "valid", invalid);
    nvs_commit(s_nvs_handle);
    ESP_LOGI(TAG, "weather cache cleared");
}
```

- [ ] **Step 4：编译验证**

```bash
env -u MSYSTEM powershell.exe -ExecutionPolicy Bypass -Command \
  "[Environment]::SetEnvironmentVariable('MSYSTEM', \$null, 'Process'); \
   . E:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1; \
   cd E:\aiot; idf.py build"
```

---



## Task 4: weather_ui 组件

**Goal：** 创建独立天气 LVGL 页面，方案 B 居中焦点式布局，以及主屏更新函数。

**Files:**
- Create: `components/weather_ui/weather_ui.h`
- Create: `components/weather_ui/weather_ui.c`
- Create: `components/weather_ui/CMakeLists.txt`

**Interfaces:**
- Consumes: `weather_fetch.h` 中的 `weather_snapshot_t`，`weather_cache.h` 中的 NVS 上下文化用
- Produces: `WeatherUI_Init()`, `WeatherUI_GetScreen()`, `WeatherUI_Render()`, `WeatherUI_SetIndoorTemp()`, `ui_update_weather_summary()` 为主屏

- [ ] **Step 1：创建 weather_ui.h**

```c
#ifndef WEATHER_UI_H_
#define WEATHER_UI_H_

#include <lvgl.h>
#include <stdbool.h>
#include "weather_fetch.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 天气页面初始化（须在 LVGL 锁内调用） */
void WeatherUI_Init(void);

/* 返回天气页面根对象 */
lv_obj_t *WeatherUI_GetScreen(void);

/* 渲染天气页快照。is_cached=true 时底部显示"缓存数据" */
void WeatherUI_Render(const weather_snapshot_t *snapshot, bool is_cached);

/* 设置室内温度（主屏右上角） */
void WeatherUI_SetIndoorTemp(float temp_c);

/* 主屏天气摘要更新（有 outdoor 数据时显示室外，无数据时显示室内） */
void ui_update_weather_summary(const weather_snapshot_t *snapshot, float indoor_temp, bool outdoor_humidity_available);

#ifdef __cplusplus
}
#endif

#endif /* WEATHER_UI_H_ */
```

- [ ] **Step 2：创建 weather_ui.c — 天气图标数据**

先定义图标数据。使用 LVGL v9 的 `lv_img_dsc_t`，RGB565A8 格式，24×24 像素。

每个图标是 24×24 的数组：`24*24*2(rgb565) + 24*24*1(alpha) = 1152 bytes`。5 个图标 = 5.7KB flash。

先用精简的图标 C 数组初始化 `lv_img_dsc_t`。以下给各图标的占位实现：

```c
/* ── Placeholder: sun icon (24x24 RGB565A8) ──
 * 真实部署时替换为实际矢量/位图转 RGB565A8 数组。
 * 这里用 lv_draw_buf 方式预分配。 */
static const uint8_t img_sun_rgb565[24*24*2+1] = { /* ... 椭圆太阳 ... */ };
static const uint8_t img_cloud_rgb565[24*24*2+1] = { /* ... 云 ... */ };
static const uint8_t img_rain_rgb565[24*24*2+1] = { /* ... 太阳+云+雨 ... */ };
static const uint8_t img_snow_rgb565[24*24*2+1] = { /* ... 雪 ... */ };
static const uint8_t img_fog_rgb565[24*24*2+1] = { /* ... 雾 ... */ };

static lv_img_dsc_t s_icon_sun, s_icon_cloud, s_icon_rain, s_icon_snow, s_icon_fog;

static void init_icons(void)
{
    uint16_t w = 24, h = 24;
    s_icon_sun.header.always_zero = 0;
    s_icon_sun.header.w = w;
    s_icon_sun.header.h = h;
    s_icon_sun.header.cf = LV_IMG_CF_RGB565A8;
    s_icon_sun.data = img_sun_rgb565;
    s_icon_sun.data_size = sizeof(img_sun_rgb565);
    /* repeat for cloud/rain/snow/fog... */
}

static const lv_img_dsc_t *get_icon(int category)
{
    switch (category) {
        case 0: return &s_icon_sun;
        case 1: return &s_icon_cloud;
        case 2: return &s_icon_rain;
        case 3: return &s_icon_snow;
        case 4: return &s_icon_fog;
        default: return &s_icon_cloud;
    }
}
```

> **图标资源（One-time 操作）：**
> 用 `bitmap2lv` 等工具将 SVG 转为 RGB565A8 C 数组，或者用手工画的二维码块数据，找到 24×24 的黑白简单图标即可。黑白 1bit 图用 RGB565A8 格式倍率可接受。图标绘制不是本计划重点，先放占位。

- [ ] **Step 3：实现 weather_ui.c — 天气页**

```c
/* Global state */
static lv_obj_t *s_screen = NULL;
static lv_obj_t *s_lbl_current_icon;   /* 24x24 weather icon */
static lv_obj_t *s_lbl_temp;           /* big temperature 28px */
static lv_obj_t *s_lbl_text;           /* e.g. "多云" */
static lv_obj_t *s_lbl_city;           /* e.g. "北京" */
static lv_obj_t *s_lbl_detail;         /* e.g. "湿度65% · 风速3级 · 体感27°" */
static lv_obj_t *s_lbl_hourly_row1;    /* "14 ☀28" */
static lv_obj_t *s_lbl_hourly_row2;
static lv_obj_t *s_lbl_daily_row1;     /* "明日 🌧 16/22°" */
static lv_obj_t *s_lbl_daily_row2;     /* "后日 ☀ 20/28°" */
static lv_obj_t *s_lbl_updated;        /* "14:30更新 / 缓存" */

void WeatherUI_Init(void)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_set_size(s_screen, EPD_WIDTH, EPD_HEIGHT);
    lv_obj_set_style_bg_color(s_screen, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_set_style_radius(s_screen, 0, 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_set_style_pad_row(s_screen, 0, 0);

    /* ── 当前天气：居中大字 ── */
    /* y=0~100 */
    s_lbl_current_icon = lv_image_create(s_screen);
    lv_obj_center(s_lbl_current_icon);  /* or lv_obj_align(s_lbl_current_icon, LV_ALIGN_TOP_MID, 0, 4) */
    lv_obj_set_size(s_lbl_current_icon, 24, 24);
    lv_obj_set_align(s_lbl_current_icon, LV_ALIGN_TOP_MID, 0, 4);

    s_lbl_temp = lv_label_create(s_screen);
    lv_obj_center(s_lbl_temp);
    lv_obj_set_align(s_lbl_temp, LV_ALIGN_TOP_MID, 0, 32);
    lv_obj_set_style_text_font(s_lbl_temp, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_align(s_lbl_temp, LV_TEXT_ALIGN_CENTER, 0);

    s_lbl_text = lv_label_create(s_screen);
    lv_obj_set_pos(s_lbl_text, 0, 62);
    lv_obj_set_size(s_lbl_text, 200, 18);
    lv_obj_set_style_text_font(s_lbl_text, &lv_font_chinese_14, 0);
    lv_obj_set_style_text_align(s_lbl_text, LV_TEXT_ALIGN_CENTER, 0);

    s_lbl_city = lv_label_create(s_screen);
    lv_obj_set_pos(s_lbl_city, 0, 80);
    lv_obj_set_size(s_lbl_city, 200, 16);
    lv_obj_set_style_text_font(s_lbl_city, &lv_font_chinese_14, 0);
    lv_obj_set_style_text_align(s_lbl_city, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_opa(s_lbl_city, LV_OPA_50, 0);

    s_lbl_detail = lv_label_create(s_screen);
    lv_obj_set_pos(s_lbl_detail, 0, 96);
    lv_obj_set_size(s_lbl_detail, 200, 16);
    lv_obj_set_style_text_font(s_lbl_detail, &lv_font_chinese_14, 0);
    lv_obj_set_style_text_align(s_lbl_detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_opa(s_lbl_detail, LV_OPA_70, 0);

    /* ── 逐时预报：y=116~156 ── */
    lv_obj_t *lbl_hourly_hdr = lv_label_create(s_screen);
    lv_obj_set_pos(lbl_hourly_hdr, 0, 116);
    lv_obj_set_size(lbl_hourly_hdr, 200, 16);
    lv_label_set_text(lbl_hourly_hdr, "逐时预报");
    lv_obj_set_style_text_font(lbl_hourly_hdr, &lv_font_chinese_14, 0);
    lv_obj_set_style_text_align(lbl_hourly_hdr, LV_TEXT_ALIGN_CENTER, 0);

    /* Two rows of 4 hourly entries (row1, row2) */
    s_lbl_hourly_row1 = lv_label_create(s_screen);
    lv_obj_set_pos(s_lbl_hourly_row1, 0, 134);
    lv_obj_set_size(s_lbl_hourly_row1, 200, 18);
    lv_obj_set_style_text_font(s_lbl_hourly_row1, &lv_font_chinese_14, 0);
    lv_obj_set_style_text_align(s_lbl_hourly_row1, LV_TEXT_ALIGN_CENTER, 0);

    s_lbl_hourly_row2 = lv_label_create(s_screen);
    lv_obj_set_pos(s_lbl_hourly_row2, 0, 152);
    lv_obj_set_size(s_lbl_hourly_row2, 200, 16);
    lv_obj_set_style_text_font(s_lbl_hourly_row2, &lv_font_chinese_14, 0);
    lv_obj_set_style_text_align(s_lbl_hourly_row2, LV_TEXT_ALIGN_CENTER, 0);

    /* ── 未来预报 + 时间戳：y=170~196 ── */
    lv_obj_t *lbl_daily_hdr = lv_label_create(s_screen);
    lv_obj_set_pos(lbl_daily_hdr, 0, 170);
    lv_obj_set_size(lbl_daily_hdr, 200, 16);
    lv_label_set_text(lbl_daily_hdr, "近日预报");
    lv_obj_set_style_text_font(lbl_daily_hdr, &lv_font_chinese_14, 0);
    lv_obj_set_style_text_align(lbl_daily_hdr, LV_TEXT_ALIGN_CENTER, 0);

    s_lbl_daily_row1 = lv_label_create(s_screen);
    lv_obj_set_pos(s_lbl_daily_row1, 0, 186);
    lv_obj_set_size(s_lbl_daily_row1, 200, 18);
    lv_obj_set_style_text_font(s_lbl_daily_row1, &lv_font_chinese_14, 0);
    lv_obj_set_style_text_align(s_lbl_daily_row1, LV_TEXT_ALIGN_CENTER, 0);

    s_lbl_daily_row2 = lv_label_create(s_screen);
    lv_obj_set_pos(s_lbl_daily_row2, 0, 200);
    lv_obj_set_size(s_lbl_daily_row2, 200, 18);
    lv_obj_set_style_text_font(s_lbl_daily_row2, &lv_font_chinese_14, 0);
    lv_obj_set_style_text_align(s_lbl_daily_row2, LV_TEXT_ALIGN_CENTER, 0);

    /* Retrofit new - updated ts */
    s_lbl_updated = lv_label_create(s_screen);
    lv_obj_set_pos(s_lbl_updated, 0, 188);
    lv_obj_set_size(s_lbl_updated, 200, 14);
    lv_obj_set_style_text_font(s_lbl_updated, &lv_font_chinese_14, 0);
    lv_obj_set_style_text_align(s_lbl_updated, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_opa(s_lbl_updated, LV_OPA_60, 0);
    lv_label_set_text(s_lbl_updated, "");
}

void WeatherUI_Render(const weather_snapshot_t *snapshot, bool is_cached)
{
    if (!s_screen || !snapshot) return;

    /* icon category */
    int cat = weather_icon_category(snapshot->now.icon);
    lv_img_set_src(s_lbl_current_icon, get_icon(cat));

    /* Temperature: try int format "26°" */
    char temp_buf[16];
    snprintf(temp_buf, sizeof(temp_buf), "%d", snapshot->now.temp);
    lv_label_set_text(s_lbl_temp, temp_buf);

    lv_label_set_text(s_lbl_text, snapshot->now.text);
    lv_label_set_text(s_lbl_city, snapshot->now.city_name);

    /* detail: "湿度65% · 风速3级 · 体感27°" */
    char detail[64];
    snprintf(detail, sizeof(detail),
             "湿度%d%%  风速%d级",
             snapshot->now.humidity, snapshot->now.wind_scale);
    lv_label_set_text(s_lbl_detail, detail);

    /* Hourly: two rows of 4 entries each */
    char hourly_str[64];
    /* row 1: entries 0-1 */
    snprintf(hourly_str, sizeof(hourly_str), "%02d%s%d  %02d%s%d",
             snapshot->hourly[0].hour, snapshot->hourly[0].icon, snapshot->hourly[0].temp,
             snapshot->hourly[1].hour, snapshot->hourly[1].icon, snapshot->hourly[1].temp);
    lv_label_set_text(s_lbl_hourly_row1, hourly_str);
    /* row 2: entries 2-3 */
    snprintf(hourly_str, sizeof(hourly_str), "%02d%s%d  %02d%s%d",
             snapshot->hourly[2].hour, snapshot->hourly[2].icon, snapshot->hourly[2].temp,
             snapshot->hourly[3].hour, snapshot->hourly[3].icon, snapshot->hourly[3].temp);
    lv_label_set_text(s_lbl_hourly_row2, hourly_str);

    /* Daily: two rows */
    char daily_str[48];
    snprintf(daily_str, sizeof(daily_str), "明日%s%d/%d",
             snapshot->daily[0].text_day, snapshot->daily[0].temp_min, snapshot->daily[0].temp_max);
    lv_label_set_text(s_lbl_daily_row1, daily_str);

    snprintf(daily_str, sizeof(daily_str), "后日%s%d/%d",
             snapshot->daily[1].text_day, snapshot->daily[1].temp_min, snapshot->daily[1].temp_max);
    lv_label_set_text(s_lbl_daily_row2, daily_str);

    /* Updated timestamp */
    char buf[32];
    time_t ft = snapshot->fetch_time;
    struct tm *t = localtime(&ft);
    snprintf(buf, sizeof(buf), "%02d:%02d更新", t->tm_hour, t->tm_min);
    if (is_cached) {
        /* check stale */
        int age_min = (int)(time(NULL) - snapshot->fetch_time) / 60;
        if (age_min > CONFIG_WEATHER_CACHE_TTL_MIN) {
            snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "  / 缓存");
        }
    }
    lv_label_set_text(s_lbl_updated, buf);
    /* move s_lbl_updated to bottom of last row if non-zero */
}

lv_obj_t *WeatherUI_GetScreen(void)
{
    return s_screen;
}

/* Indoor temp label floating var */
static float s_indoor_temp_c;

void WeatherUI_SetIndoorTemp(float temp_c)
{
    s_indoor_temp_c = temp_c;
}

/* ── Main-screen weather summary ──
 * func pointer stored in ui, called from data_task under LVGL lock */
void ui_update_weather_summary(const weather_snapshot_t *snapshot, float indoor_temp, bool outdoor_humidity_available)
{
    if (!snapshot) {
        /* fallback: show indoor sensors only */
        lv_label_set_text(ui.lbl_temperature, "室内");
        /* set lbl_humidity to indoor value */
        return;
    }
    /* outdoor data available: show outdoor temperature + humidity */
    /* set lbl_humidity to outdoor value */
}
```

- [ ] **Step 4：创建 CMakeLists.txt**

```cmake
idf_component_register(
    SRCS "weather_ui.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES lvgl lvgl_port weather_fetch weather_cache font_cn
)
```

- [ ] **Step 5：编译验证**

```bash
env -u MSYSTEM powershell.exe -ExecutionPolicy Bypass -Command \
  "[Environment]::SetEnvironmentVariable('MSYSTEM', \$null, 'Process'); \
   . E:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1; \
   cd E:\aiot; idf.py build"
```

---



## Task 5: 修改 ui 主屏（主屏右上角 + 页面注册）

**Goal：** 将主屏右上角从纯室内传感器改为"天气摘要 + 室内温度"，并在页面枚举中加入天气页。

**Files:**
- Modify: `components/ui/ui.h` — 添加 `lbl_indoor_temp`、`PAGE_WEATHER`、`ui_update_weather_summary` 声明
- Modify: `components/ui/ui_screen.c` — 添加 `lbl_indoor_temp` 初始化、设置顶部点击事件、`ui_switch_page` 加入天气页
- Modify: `components/ui/CMakeLists.txt` — 加入 `weather_ui` PRIV_REQUIRES

- [ ] **Step 1：修改 components/ui/ui.h**

在第 23 行后（`lbl_humidity` 之后）新增 `lbl_indoor_temp`：

```c
lv_obj_t *lbl_humidity;
lv_obj_t *lbl_indoor_temp;   /* 室内温度（小型 SHTC3） */
```

在 `ui_update_*` 函数列表（第 49~55 行）之后新增：

```c
void ui_update_weather_summary(const weather_snapshot_t *snapshot, float indoor_temp, bool outdoor_humidity_available);
```

在 `ui_page_t` 枚举（第 57~64 行）末尾新增：

```c
PAGE_WEATHER,
```

顶部新增 include：

```c
#include "weather_fetch.h"
```

- [ ] **Step 2：修改 components/ui/ui_screen.c**

添加 header：

```c
#include "weather_ui.h"
#include "weather_cache.h"
```

修改 `setup_top_right()`，在 `lbl_humidity` 之后添加 `lbl_indoor_temp`：

```c
/* Indoor temperature — shows when outdoor weather unavailable */
ui->lbl_indoor_temp = lv_label_create(ui->cont_top_right);
lv_obj_set_pos(ui->lbl_indoor_temp, 0, 78);  /* below humidity */
lv_obj_set_size(ui->lbl_indoor_temp, 100, 16);
lv_obj_set_style_text_font(ui->lbl_indoor_temp, &lv_font_chinese_14, 0);
lv_obj_set_style_text_color(ui->lbl_indoor_temp, lv_color_black(), 0);
lv_obj_set_style_text_align(ui->lbl_indoor_temp, LV_TEXT_ALIGN_CENTER, 0);
lv_label_set_text(ui->lbl_indoor_temp, "");
```

在 `setup_top_right` 末尾，整个 `cont_top_right` 区域添加"点击进入天气页"事件：

```c
/* Click top-right quadrant → enter weather page */
lv_obj_add_flag(ui->cont_top_right, LV_OBJ_FLAG_CLICKABLE);
lv_obj_add_event_cb(ui->cont_top_right, weather_entry_click_cb, LV_EVENT_CLICKED, NULL);
```

并在文件内添加点击回调（放在文件底部 static 函数区）：

```c
static void weather_entry_click_cb(lv_event_t *e)
{
    (void)e;
    ui_switch_page(PAGE_WEATHER);
}
```

修改 `ui_switch_page()` 函数，新增 `PAGE_WEATHER` 分支。在最后一个 `else if` 之后加：

```c
} else if (page == PAGE_WEATHER) {
    if (!s_weather_screen) {
        s_weather_screen = WeatherUI_GetScreen();
    }
    if (s_weather_screen) {
        /* Render latest snapshot if available */
        WeatherUI_Render(&s_weather_snapshot, false);
        lv_screen_load(s_weather_screen);
    }
}
```

> 注意：这里需要添加静态变量 `s_weather_screen` 和 `s_weather_snapshot` 来跟踪天气页面。

- [ ] **Step 3：修改 CMakeLists.txt**

在 `PRIV_REQUIRES` 中加入 `weather_ui`：

```cmake
idf_component_register(
 SRCS "ui_screen.c"
 INCLUDE_DIRS "."
 PRIV_REQUIRES lvgl lvgl_port todo_ui todo_manager font_cn timer_ui timer_engine weather_ui)
```

- [ ] **Step 4：编译验证**

```bash
env -u MSYSTEM powershell.exe -ExecutionPolicy Bypass -Command \
  "[Environment]::SetEnvironmentVariable('MSYSTEM', \$null, 'Process'); \
   . E:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1; \
   cd E:\aiot; idf.py build"
```

---



## Task 6: 修改 main.cpp — 初始化 + data_task 调度 + 页面

**Goal：** 将天气组件接入主初始化序列、data_task 周六时拉取、深睡眠路径 NVS 恢复。

**Files:**
- Modify: `main/main.cpp`

- [ ] **Step 1：引入新组件头文件**

在现有 includes 之后添加：

```cpp
#include "weather_fetch.h"
#include "weather_cache.h"
#include "weather_ui.h"
```

- [ ] **Step 2：声明静态状态**

在 `data_task` 附近添加静态状态：

```cpp
static int s_weather_fetch_tick = 0;
static int s_weather_consecutive_fail = 0;
static weather_snapshot_t s_weather_snapshot;
static bool s_weather_has_cached = false;
static TaskHandle_t s_weather_task_handle = nullptr;
```

- [ ] **Step 3：冷启动路径：在 `/* 10. WiFi AP + TodoWeb */` 之后、`xTaskCreatePinnedToCore(data_task` 之前加入天气初始化**

```cpp
/* 11. Weather: init cache (no WiFi needed), then UI (LVGL lock) */
WeatherCache_Init();
bool weather_cache_loaded = WeatherCache_Load(&s_weather_snapshot);
s_weather_has_cached = weather_cache_loaded;
ESP_LOGI(TAG, "Weather cache: %s", weather_cache_loaded ? "loaded" : "empty");

WeatherFetch_Init();

if (Lvgl_Lock(-1)) {
    WeatherUI_Init();
    lv_obj_t *weather_screen = WeatherUI_GetScreen();
    if (weather_screen && weather_cache_loaded) {
        WeatherUI_Render(&s_weather_snapshot, true);
    }
    /* Register weather page in ui_switch_page */
    /* NOTE: WeatherUI_GetScreen must match lv_obj_t */
    Lvgl_Unlock();
}
```

> **注意：** `ui_switch_page()` 中的 PAGE_WEATHER 需要能找到天气页根对象。这里会在 Task 4 中确保 API 可用。

- [ ] **Step 4：data_task 中加入天气拉取调度**

在 data_task for 循环内现有每 tick sleep 的 `vTaskDelay(pdMS_TO_TICKS(1000))` 之前，加入天气逻辑。

```cpp
    /* ── Weather refresh ── */
    if (WeatherFetch_IsReady() && WifiBsp_IsConnected()) {
        int interval = CONFIG_WEATHER_FETCH_INTERVAL_MIN * 60;  /* seconds */
        /* Backoff on consecutive failures: double up to max 2h */
        if (s_weather_consecutive_fail > 0) {
            interval = interval * (1 << (s_weather_consecutive_fail - 1));
            if (interval > 7200) interval = 7200;
        }
        s_weather_fetch_tick++;
        if (s_weather_fetch_tick >= interval) {
            s_weather_fetch_tick = 0;
            weather_snapshot_t snap;
            memset(&snap, 0, sizeof(snap));
            /* NOTE: HTTP call must NOT be inside Lvgl_Lock */
            bool ok = WeatherFetch_Update(&snap);
            if (ok) {
                WeatherCache_Save(&snap);
                s_weather_snapshot = snap;
                s_weather_has_cached = true;
                s_weather_consecutive_fail = 0;
                /* Now update UI under lock */
                if (Lvgl_Lock(500)) {
                    if (ui_get_current_page() == PAGE_WEATHER) {
                        WeatherUI_Render(&s_weather_snapshot, false);
                    }
                    ui_update_weather_summary(&s_weather_snapshot,
                                               s_sh_temp_c, true);
                    Lvgl_Unlock();
                }
            } else {
                ESP_LOGW(TAG, "Weather fetch failed: %s", WeatherFetch_LastError());
                s_weather_consecutive_fail++;
                if (s_weather_consecutive_fail >= 3) {
                    ESP_LOGW(TAG, "Weather fetch failed %d times, backing off",
                             s_weather_consecutive_fail);
                }
            }
        }
    }
```

其中 `s_sh_temp_c` 是 data_task 中已有的 SHTC3 温度值。

- [ ] **Step 5：深睡眠唤醒路径 — 已有 Task 4 中的 `WeatherCache_Load()`，恢复后渲染**

深睡眠路径在 `/* UI re-init */` 之后等待，已有：

```cpp
WeatherCache_Init(); already in init
/* WeatherCache_Load(&s_weather_snapshot) done in cold boot path above */

if (Lvgl_Lock(-1)) {
    WeatherUI_Init();
    if (s_weather_has_cached) {
        WeatherUI_Render(&s_weather_snapshot, true);
    }
    Lvgl_Unlock();
}
```

> **注意：** 深睡眠唤醒后 `s_weather_fetch_tick` 和 `s_weather_consecutive_fail` 重置。因为深睡眠路径不会自动联网，天气数据保持缓存，后续用户按 BOOT 恢复 WiFi 后才会重新拉取。

- [ ] **Step 6：编译验证**

```bash
env -u MSYSTEM powershell.exe -ExecutionPolicy Bypass -Command \
  "[Environment]::SetEnvironmentVariable('MSYSTEM', \$null, 'Process'); \
   . E:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1; \
   cd E:\aiot; idf.py build"
```

---



## Task 7: 修改 todo_web — 天气配置 API

**Goal：** 在现有 `todo_web` 中新增天气配置读取/保存接口。

**Files:**
- Modify: `components/todo_web/todo_web.cpp` — 新增 handler 函数和 register
- Modify: `components/todo_web/todo_web.h` — 无需接口修改（HTTP 端点在内部注册）
- Modify: `components/todo_web/CMakeLists.txt` — 加入 PRIV_REQUIRES weather_fetch

- [ ] **Step 1：修改 CMakeLists.txt**

```cmake
idf_component_register(
    SRCS "todo_web.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_http_server todo_manager json standby_screen wifi_bsp weather_fetch nvs_flash)
```

- [ ] **Step 2：新增 handler 函数（在 todo_web.cpp 的 `api_handler_*` 区域之后）**

```cpp
/* ── Weather config API ── */
static esp_err_t api_get_weather_config(httpd_req_t *req)
{
    char api_key[8 + 4] = {0}; /* up to 4 masked chars */
    size_t key_len = strlen(CONFIG_WEATHER_API_KEY);
    if (key_len > 4) {
        /* masked: "****abcd" */
        for (int i = 0; i < key_len - 4; i++) api_key[i] = '*';
        strncat(api_key, CONFIG_WEATHER_API_KEY + key_len - 4, 4);
    } else {
        strncpy(api_key, CONFIG_WEATHER_API_KEY, sizeof(api_key) - 1);
    }

    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "api_key_masked", api_key);
    cJSON_AddStringToObject(data, "location_id", CONFIG_WEATHER_LOCATION_ID);
    cJSON_AddStringToObject(data, "city_name", CONFIG_WEATHER_CITY_NAME);
    return send_ok(req, data);
}

static esp_err_t api_post_weather_config(httpd_req_t *req)
{
    cJSON *body = parse_body(req);
    if (!body) return send_error(req, "Invalid JSON", 400);

    cJSON *jkey = cJSON_GetObjectItem(body, "api_key");
    cJSON *jloc = cJSON_GetObjectItem(body, "location_id");
    cJSON *jcity = cJSON_GetObjectItem(body, "city_name");

    if (!cJSON_IsString(jkey) || jkey->valuestring[0] == '\0')
        { cJSON_Delete(body); return send_error(req, "Missing api_key", 400); }
    if (!cJSON_IsString(jloc) || jloc->valuestring[0] == '\0')
        { cJSON_Delete(body); return send_error(req, "Missing location_id", 400); }
    if (!cJSON_IsString(jcity) || jcity->valuestring[0] == '\0')
        { cJSON_Delete(body); return send_error(req, "Missing city_name", 400); }
    if (strlen(jcity->valuestring) > WEATHER_TEXT_LEN - 1)
        { cJSON_Delete(body); return send_error(req, "city_name too long", 400); }

    /* Store in NVS */
    nvs_handle_t handle;
    esp_err_t err = nvs_open("weather", NVS_READWRITE, &handle);
    if (err != ESP_OK) { cJSON_Delete(body); return send_error(req, "NVS open failed", 500); }
    nvs_set_str(handle, "api_key", jkey->valuestring);
    nvs_set_str(handle, "location_id", jloc->valuestring);
    nvs_set_str(handle, "city_name", jcity->valuestring);
    nvs_commit(handle);
    nvs_close(handle);

    /* Clear cached weather so stale data is not displayed */
    WeatherCache_Clear();

    cJSON_Delete(body);
    return send_ok(req, NULL);
}
```

> **重要设计选择：** API Key 写入 NVS（存储于 flash，非源码），不在 Kconfig 中直接设置。这样用户通过 Web 配置时不需要重建固件。

- [ ] **Step 3：修改 `api_dispatcher()` 注册路由**

在 dispatcher 的 URI 路由注册区，添加：

```cpp
/* GET /api/weather/config */
httpd_uri_t uri_weather_get = {
    .uri = "/api/weather/config",
    .method = HTTP_GET,
    .handler = api_get_weather_config,
    .user_ctx = nullptr,
};
httpd_register_uri_handler(s_httpd, &uri_weather_get);

/* POST /api/weather/config */
httpd_uri_t uri_weather_post = {
    .uri = "/api/weather/config",
    .method = HTTP_POST,
    .handler = api_post_weather_config,
    .user_ctx = nullptr,
};
httpd_register_uri_handler(s_httpd, &uri_weather_post);
```

同时在 `api_dispatcher` 函数末尾添加或确认这些注册也只发生在 `s_httpd` 非 NULL 时（参考现有 POST `/api/wifi` 注册方式）。

- [ ] **Step 4：修改 weather_cache.c 读取 NVS 覆盖配置**

在 `WeatherFetch_Init()` 中，NVS 覆盖 Kconfig 默认值：

```c
/* After reading Kconfig defaults in WeatherFetch_Init, add: */
nvs_handle_t handle;
esp_err_t err2 = nvs_open("weather", NVS_READONLY, &handle);
if (err2 == ESP_OK) {
    size_t len = sizeof(s_api_key) - 1;
    nvs_get_str(handle, "api_key", s_api_key, &len);
    len = sizeof(s_location_id) - 1;
    nvs_get_str(handle, "location_id", s_location_id, &len);
    len = sizeof(s_city_name) - 1;
    nvs_get_str(handle, "city_name", s_city_name, &len);
    nvs_close(handle);
}
```

这确保 `CONFIG_WEATHER_API_KEY` 为空时也能通过 Web 配置使用天气功能。

- [ ] **Step 5：编译验证**

```bash
env -u MSYSTEM powershell.exe -ExecutionPolicy Bypass -Command \
  "[Environment]::SetEnvironmentVariable('MSYSTEM', \$null, 'Process'); \
   . E:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1; \
   cd E:\aiot; idf.py build"
```

- [ ] **Step 6：串口验证配置接口**

通过浏览器或 curl 测试：
```bash
curl -X POST http://aiot-todo.local/api/weather/config \
  -H "Content-Type: application/json" \
  -d '{"api_key":"your_key","location_id":"101010100","city_name":"北京"}'
```
Response: `{"ok":true}`
然后 GET 返回带 masked key 的配置：
```bash
curl http://aiot-todo.local/api/weather/config
```
Expected:
```json
{"ok":true,"data":{"api_key_masked":"****abcd","location_id":"101010100","city_name":"北京"}}
```

---



## 最终验证

- [ ] **编译：** `idf.py build` 无 errors、warnings 可接受
- [ ] **idf.py size：** 检查 PSRAM 和 Flash 占用
- [ ] **设备实测：** 配置 API Key + Location ID → 主屏右上角显示室外温度和室内温度 → 点击右上角进入天气页 → 逐时预报和未来预报正确显示 → 断电深睡眠 → 重启后主屏仍显示缓存天气 → 恢复 WiFi 后自动刷新
- [ ] **失败降级：** 关闭 WiFi → 主屏显示室内温度 + 缓存时间戳 → 无缓存则显示室内温度 + 未连接标识

---



## 里程碑总结

| 步骤 | 交付物 | 验证命令 |
|------|--------|---------|
| 0 | HTTPS 最小验证通过 | 串口日志 |
| 1 | `weather_fetch` 组件 | `idf.py build` |
| 2 | `weather_cache` 组件 | `idf.py build` |
| 3 | `weather_ui` 组件 | `idf.py build` |
| 4 | ui 主屏改 UI | `idf.py build` |
| 5 | main.cpp 调度 | `idf.py build` + 设备实测 |
| 6 | todo_web 配置 API | 串口 + curl |
