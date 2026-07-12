#ifndef APP_LIST_UI_H
#define APP_LIST_UI_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"
#include "esp_err.h"

/* 应用列表屏幕 — 200×200，顶部标题 "应用" + 2×2 网格。
 *
 * 格子布局（200×200，标题占顶部 ~20px，网格区 y=24..196）：
 *   左上 = 贪吃蛇入口   右上 = 画板入口
 *   左下 / 右下 = 黑框占位（待扩展）
 *
 * 退出：左滑手势（LVGL 的 LV_EVENT_GESTURE，方向 = LV_DIR_LEFT），
 * 监听整个 screen，触发后调 s_back_cb（main.cpp 注入的 ui_switch_page(DASHBOARD)）。
 * 不用 LVGL 长按事件——之前在 dashboard 右下角用 LONG_PRESSED 区分短/长按失败 3 次，
 * 这次彻底改用左滑手势。
 *
 * 解耦：本组件不依赖 ui 组件（避免 PRIV_REQUIRES 循环）。进入贪吃蛇/画板
 * 的动作通过 s_enter_snake_cb / s_enter_draw_cb 由 main.cpp 注入，
 * 退出通过 s_back_cb 由 main.cpp 注入。
 *
 * 须在 LVGL 锁内调用所有公开 API。组件初始化由 main 在锁内调用，
 * 回调本身在锁内被 LVGL 派发——回调里不要再加锁。*/

/* 初始化应用列表屏幕。须在 LVGL 锁内调用。幂等。 */
esp_err_t AppListUi_Init(void);

/* 获取应用列表屏幕，供 ui_switch_page 懒加载。 */
lv_obj_t *AppListUi_GetScreen(void);

/* 进入应用列表页时调用。须在 LVGL 锁内调用。
 * 当前仅占位，未来可做进入动效（e-ink 无动画，暂为空钩子）。 */
void AppListUi_Enter(void);

/* 退出回调（左滑手势触发，main.cpp 注入为 ui_switch_page(PAGE_DASHBOARD)）。
 * 运行在 LVGL 锁内。 */
void AppListUi_SetBackCallback(void (*cb)(void));

/* 进入贪吃蛇格子点击回调（main.cpp 注入为 ui_switch_page(PAGE_SNAKE)）。
 * 运行在 LVGL 锁内。 */
void AppListUi_SetEnterSnakeCallback(void (*cb)(void));

/* 进入画板格子点击回调（main.cpp 注入为 ui_switch_page(PAGE_DRAW)）。
 * 运行在 LVGL 锁内。 */
void AppListUi_SetEnterDrawCallback(void (*cb)(void));

#ifdef __cplusplus
}
#endif

#endif /* APP_LIST_UI_H */
