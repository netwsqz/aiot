#ifndef WEATHER_CACHE_H_
#define WEATHER_CACHE_H_

#include "weather_fetch.h"
#include <stdbool.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化 NVS（幂等）并打开 "weather" 命名空间句柄。返回 ESP_OK/ESP_FAIL。 */
esp_err_t WeatherCache_Init(void);

/* 写入快照（fetch 成功后调用）。内部会设置 version = WEATHER_CACHE_VERSION。 */
void WeatherCache_Save(const weather_snapshot_t *snapshot);

/* 读取缓存快照（深睡眠唤醒时调用）。返回 true 表示读到有效缓存
 *（valid==1 + blob 大小匹配 + version 匹配）。 */
bool WeatherCache_Load(weather_snapshot_t *snapshot);

/* 清除缓存（城市/API Key 改变时调用）。 */
void WeatherCache_Clear(void);

#ifdef __cplusplus
}
#endif

#endif /* WEATHER_CACHE_H_ */
