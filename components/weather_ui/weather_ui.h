#ifndef WEATHER_UI_H_
#define WEATHER_UI_H_

#include <lvgl.h>
#include <stdbool.h>
#include "weather_fetch.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 天气页面初始化（须在 LVGL 锁内调用）。创建 200×200 白底页面根对象。 */
void WeatherUI_Init(void);

/* 返回天气页面根对象（首次 Init 后有效）。 */
lv_obj_t *WeatherUI_GetScreen(void);

/* 渲染天气页快照。is_cached=true 时底部标注缓存/过旧。
 * 须在 LVGL 锁内调用。snapshot 不可为 NULL。 */
void WeatherUI_Render(const weather_snapshot_t *snapshot, bool is_cached);

/* 若已渲染过快照（s_last_snapshot 有效），用其重绘当前页。
 * 用于进入天气页时立即显示缓存内容，避免空白。
 * 须在 LVGL 锁内调用。无快照时为 no-op。 */
void WeatherUI_RenderIfCached(void);

/* 注册"返回主界面"回调：天气页的左滑手势触发。
 * 须在 Init 前调用。cb 不可为 NULL。
 * 注：刷新天气入口已移至 WiFi 管理页（长按 BOOT 进入）。 */
void WeatherUI_SetBackCallback(void (*cb)(void));

#ifdef __cplusplus
}
#endif

#endif /* WEATHER_UI_H_ */
