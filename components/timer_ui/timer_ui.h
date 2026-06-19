#ifndef TIMER_UI_H
#define TIMER_UI_H

#include <lvgl.h>
#include <esp_err.h>
#include "timer_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TIMER_PAGE_TOOLS = 0,
    TIMER_PAGE_POMODORO,
    TIMER_PAGE_REMINDER,
} timer_page_t;

esp_err_t TimerUi_Init(void);

/* 获取 timer 屏幕（含 tools/pomodoro/reminder 子页面） */
lv_obj_t *TimerUi_GetScreen(void);

/* 切换 timer 屏幕内的子页面。须在 LVGL 锁内调用 */
void TimerUi_ShowPage(timer_page_t page);

/* 在 tools 页按返回时调用（→ 回主屏） */
void TimerUi_SetBackCallback(void (*cb)(void));

/* 由 engine fired_cb（timer 任务上下文）调用，仅入队 */
void TimerUi_PushFiredEvent(uint8_t id, timer_kind_t kind, uint32_t total_seconds);

/* 由 data_task 每秒在 LVGL 锁内调用：轮询到点队列 + 每分钟刷新可见页 */
void TimerUi_TickUpdate(void);

#ifdef __cplusplus
}
#endif

#endif /* TIMER_UI_H */
