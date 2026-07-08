#ifndef WEATHER_FETCH_H_
#define WEATHER_FETCH_H_

#include <stdint.h>
#include <stdbool.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Data model ── */
#define WEATHER_TEXT_LEN        16
#define WEATHER_ICON_CODE_LEN   8
#define WEATHER_DATE_LEN        12
#define WEATHER_HOURLY_COUNT    4
#define WEATHER_DAILY_COUNT     1   /* only "明日" — drop "后日" to free vertical space on the 200×200 screen */
#define WEATHER_CACHE_VERSION   1

/* Config string buffer capacities (incl. NUL). Exposed so the Web config
 * API can validate input length against what WeatherFetch_Init will read. */
#define WEATHER_API_KEY_LEN     64
#define WEATHER_LOCATION_ID_LEN 32
#define WEATHER_CITY_NAME_LEN   32

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

/* 初始化：读 Kconfig 默认值，再用 NVS namespace "weather" 覆盖（api_key /
 * location_id / city_name）。不联网。返回 ESP_OK/ESP_FAIL（仅 NVS 读取
 * 严重失败时返回 FAIL；未配置 API Key 不算失败）。 */
esp_err_t WeatherFetch_Init(void);

/* 返回是否具备拉取条件（已初始化 + API Key + Location ID + City Name 均非空）。 */
bool WeatherFetch_IsReady(void);

/* 执行一次完整拉取（now + 24h + 3d），串行 3 个 HTTPS 请求，写入 snapshot。
 * 必须在非 LVGL 任务上下文调用（不在 Lvgl_Lock 内）。返回 true 表示成功。 */
bool WeatherFetch_Update(weather_snapshot_t *snapshot);

/* 返回最近一次错误描述（用于日志）。 */
const char *WeatherFetch_LastError(void);

/* 将 QWeather 图标码分类：0=sun 1=cloud 2=rain 3=snow 4=fog。 */
int WeatherFetch_IconCategory(const char *icon_code);

/* 重新从 NVS 加载配置（api_key / location_id / city_name / base_url）。
 * Web 配置接口 POST /api/weather/config 写入 NVS 后调用，使新配置立即生效，
 * 无需重启。幂等：可多次调用。 */
esp_err_t WeatherFetch_ReloadConfig(void);

/* 请求立即拉取一次（异步）：data_task 下一轮检测到标志后执行。
 * 用于 Web API 手动触发。线程安全（portMUX 临界区保护）。 */
void WeatherFetch_RequestRefresh(void);

/* 清除并返回"是否被请求立即拉取"。data_task 每轮调用。 */
bool WeatherFetch_ConsumeRefreshRequest(void);

#ifdef __cplusplus
}
#endif

#endif /* WEATHER_FETCH_H_ */
