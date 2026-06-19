#include "timer_ui.h"
#include "timer_engine.h"
#include "port_rtc.h"
#include "font_cn.h"
#include <stdio.h>
#include <string.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

static const char *TAG = "timer_ui";

static lv_obj_t *s_timer_screen = NULL;
static lv_obj_t *s_tools_page   = NULL;
static lv_obj_t *s_pomo_page    = NULL;
static lv_obj_t *s_rem_page     = NULL;
static timer_page_t s_current_page = TIMER_PAGE_TOOLS;
static void (*s_back_cb)(void) = NULL;

/* ---- Pomodoro 页控件（Task 3 填充） ---- */
static lv_obj_t *s_pomo_idle_cont = NULL;
static lv_obj_t *s_pomo_run_cont  = NULL;
static lv_obj_t *s_pomo_min_label = NULL;
static lv_obj_t *s_pomo_bar       = NULL;

/* ---- Reminder 页控件（Task 5 填充） ---- */
static lv_obj_t *s_rem_title      = NULL;
static lv_obj_t *s_rem_btns[6]    = {0};
static lv_obj_t *s_rem_list_cont  = NULL;
static lv_obj_t *s_confirm_panel  = NULL;
static uint8_t   s_confirm_id     = 0;

/* ---- 满屏提醒（Task 4 填充） ---- */
typedef struct {
    uint8_t      id;
    timer_kind_t kind;
    uint32_t     total_seconds;
} fired_item_t;
static QueueHandle_t s_fired_queue        = NULL;
static lv_obj_t     *s_alert_screen       = NULL;
static lv_obj_t     *s_alert_type_lbl     = NULL;
static lv_obj_t     *s_alert_dur_lbl      = NULL;
static lv_obj_t     *s_alert_time_lbl     = NULL;
static lv_obj_t     *s_screen_before_alert = NULL;
static bool          s_alert_showing      = false;
static timer_kind_t  s_last_fired_kind    = TIMER_KIND_POMODORO;

static const uint32_t POMO_PRESETS[3] __attribute__((unused)) = {15, 25, 50};
static const uint32_t REM_PRESETS[6] __attribute__((unused))  = {5, 10, 15, 30, 45, 60};

/* ============ 通用：返回按钮 + 手势（仿 todo_ui） ============ */
static void back_btn_cb(lv_event_t *e)
{
    (void)e;
    if (s_current_page == TIMER_PAGE_POMODORO || s_current_page == TIMER_PAGE_REMINDER)
        TimerUi_ShowPage(TIMER_PAGE_TOOLS);
    else if (s_back_cb) s_back_cb();
}

static void swipe_cb(lv_event_t *e)
{
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
    if (dir == LV_DIR_RIGHT) back_btn_cb(e);
}

static lv_obj_t *create_back_btn(lv_obj_t *parent)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 40, 32);
    lv_obj_set_pos(btn, 4, 4);
    lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_add_event_cb(btn, back_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(lbl);
    return btn;
}

static lv_obj_t *make_page(lv_obj_t *parent)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_size(p, 200, 200);
    lv_obj_set_pos(p, 0, 0);
    lv_obj_set_style_bg_color(p, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_radius(p, 0, 0);
    lv_obj_set_style_pad_all(p, 0, 0);
    lv_obj_set_scrollbar_mode(p, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(p, LV_OBJ_FLAG_GESTURE_BUBBLE);
    return p;
}

/* ============ tools 页 ============ */
static void tools_pomo_cb(lv_event_t *e) { (void)e; TimerUi_ShowPage(TIMER_PAGE_POMODORO); }
static void tools_rem_cb(lv_event_t *e)  { (void)e; TimerUi_ShowPage(TIMER_PAGE_REMINDER); }

static void create_tools_page(lv_obj_t *parent)
{
    s_tools_page = make_page(parent);
    create_back_btn(s_tools_page);

    lv_obj_t *title = lv_label_create(s_tools_page);
    lv_obj_set_pos(title, 48, 10);
    lv_label_set_text(title, "时间工具");
    lv_obj_set_style_text_font(title, &lv_font_chinese_14, 0);
    lv_obj_set_style_text_decor(title, LV_TEXT_DECOR_UNDERLINE, 0);

    const char *names[2] = {"番茄钟", "定时提醒"};
    void (*cb_arr[2])(lv_event_t *) = {tools_pomo_cb, tools_rem_cb};
    for (int i = 0; i < 2; i++) {
        lv_obj_t *btn = lv_btn_create(s_tools_page);
        lv_obj_set_size(btn, 180, 56);
        lv_obj_set_pos(btn, 10, 50 + i * 66);
        lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_radius(btn, 4, 0);
        lv_obj_add_event_cb(btn, cb_arr[i], LV_EVENT_CLICKED, NULL);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, names[i]);
        lv_obj_set_style_text_font(lbl, &lv_font_chinese_14, 0);
        lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
        lv_obj_center(lbl);
    }
}

/* ============ pomodoro / reminder 页（本 Task 占位，Task 3/5 实现） ============ */
static void create_pomo_page(lv_obj_t *parent)
{
    s_pomo_page = make_page(parent);
    create_back_btn(s_pomo_page);
    lv_obj_t *t = lv_label_create(s_pomo_page);
    lv_obj_set_pos(t, 48, 10);
    lv_label_set_text(t, "番茄钟");
    lv_obj_set_style_text_font(t, &lv_font_chinese_14, 0);
}

static void create_rem_page(lv_obj_t *parent)
{
    s_rem_page = make_page(parent);
    create_back_btn(s_rem_page);
    s_rem_title = lv_label_create(s_rem_page);
    lv_obj_set_pos(s_rem_title, 48, 10);
    lv_label_set_text(s_rem_title, "定时提醒");
    lv_obj_set_style_text_font(s_rem_title, &lv_font_chinese_14, 0);
}

static void refresh_pomodoro_page(void) { /* Task 3 */ }
static void refresh_reminder_page(void) { /* Task 5 */ }

/* ============ 公开 API ============ */
esp_err_t TimerUi_Init(void)
{
    s_fired_queue = xQueueCreate(4, sizeof(fired_item_t));
    if (!s_fired_queue) return ESP_FAIL;

    s_timer_screen = lv_obj_create(NULL);
    lv_obj_set_size(s_timer_screen, 200, 200);
    lv_obj_set_scrollbar_mode(s_timer_screen, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(s_timer_screen, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_timer_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_timer_screen, 0, 0);
    lv_obj_set_style_radius(s_timer_screen, 0, 0);
    lv_obj_set_style_pad_all(s_timer_screen, 0, 0);

    create_tools_page(s_timer_screen);
    create_pomo_page(s_timer_screen);
    create_rem_page(s_timer_screen);

    lv_obj_clear_flag(s_tools_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_pomo_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_rem_page, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_event_cb(s_timer_screen, swipe_cb, LV_EVENT_GESTURE, NULL);
    return ESP_OK;
}

lv_obj_t *TimerUi_GetScreen(void) { return s_timer_screen; }

void TimerUi_ShowPage(timer_page_t page)
{
    s_current_page = page;
    lv_obj_add_flag(s_tools_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_pomo_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_rem_page, LV_OBJ_FLAG_HIDDEN);
    if (page == TIMER_PAGE_TOOLS) {
        lv_obj_clear_flag(s_tools_page, LV_OBJ_FLAG_HIDDEN);
    } else if (page == TIMER_PAGE_POMODORO) {
        lv_obj_clear_flag(s_pomo_page, LV_OBJ_FLAG_HIDDEN);
        refresh_pomodoro_page();
    } else {
        lv_obj_clear_flag(s_rem_page, LV_OBJ_FLAG_HIDDEN);
        refresh_reminder_page();
    }
}

void TimerUi_SetBackCallback(void (*cb)(void)) { s_back_cb = cb; }

void TimerUi_PushFiredEvent(uint8_t id, timer_kind_t kind, uint32_t total_seconds)
{
    (void)id; (void)kind; (void)total_seconds;  /* Task 4 实现 */
}

void TimerUi_TickUpdate(void) { /* Task 4 实现 */ }
