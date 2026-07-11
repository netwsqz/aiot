#ifndef SNAKE_UI_H
#define SNAKE_UI_H

#include <lvgl.h>
#include <esp_err.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 贪吃蛇屏幕 — 200×200 网格游戏（10×10 格，20×20 网格）。
 *
 * 渲染：1bpp buffer（EPD 格式 bit=1白/0黑）+ ARGB8888 canvas 局部刷新，
 * 仿 draw_ui 范式。每步只 invalidate 蛇头新格 + 旧尾格 + 食物格，
 * 避免 e-ink 300ms/次全屏闪。
 *
 * 节奏：lv_timer 500ms/步（data_task 不驱动，独立 timer）。
 * 操控：点击屏幕四象限转向（上/下/左/右），禁 180° 反向。
 * 入口：dashboard 右下角长按（短按仍画板）。
 *
 * 须在 LVGL 锁内调用所有公开 API。*/

/* 初始化贪吃蛇屏幕。须在 LVGL 锁内调用。幂等。 */
esp_err_t SnakeUi_Init(void);

/* 获取贪吃蛇屏幕，供 ui_switch_page 懒加载。 */
lv_obj_t *SnakeUi_GetScreen(void);

/* 进入贪吃蛇页面时调用：重置为 READY 状态（清盘 + 画边框 + "点屏幕开始"提示），
 * 并启动 500ms 步进 lv_timer。须在 LVGL 锁内调用。 */
void SnakeUi_Enter(void);

/* 退出贪吃蛇页面前调用：停步进 lv_timer（保留当前局状态不重置）。
 * 须在 LVGL 锁内调用。由 ui_switch_page 或 BOOT 短按退出路径触发。 */
void SnakeUi_Exit(void);

/* 退出前的回调（由 main.cpp 注入，设为 ui_switch_page(PAGE_DASHBOARD)）。
 * 运行在 LVGL 锁内。SnakeUi 在回调触发前已 SnakeUi_Exit 停 timer。 */
void SnakeUi_SetBackCallback(void (*cb)(void));

/* 供 PC sim 手动驱动一步（固件用 lv_timer，不调此函数）。
 * 须在 LVGL 锁内调用。 */
void SnakeUi_Tick(void);

#ifdef __cplusplus
}
#endif

#endif /* SNAKE_UI_H */
