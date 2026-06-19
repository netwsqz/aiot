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
static const uint32_t REM_PRESETS[6]   = {5, 10, 15, 30, 45, 60};

/* forward decl: refresh_pomodoro_page is defined later but called from pomo_start/cancel_cb */
static void refresh_pomodoro_page(void);
static void refresh_reminder_page(void);

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

/* ============ pomodoro / reminder 页 ============ */
static void pomo_start_cb(lv_event_t *e)
{
    uint32_t min = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    if (TimerEngine_StartPomodoro(min, NULL) == ESP_OK) refresh_pomodoro_page();
}

static void pomo_cancel_cb(lv_event_t *e)
{
    (void)e;
    const timer_instance_t *p = TimerEngine_GetPomodoro();
    if (p) TimerEngine_Cancel(p->id);
    refresh_pomodoro_page();
}

static void create_pomo_page(lv_obj_t *parent)
{
    s_pomo_page = make_page(parent);
    create_back_btn(s_pomo_page);

    lv_obj_t *title = lv_label_create(s_pomo_page);
    lv_obj_set_pos(title, 48, 10);
    lv_label_set_text(title, "番茄钟");
    lv_obj_set_style_text_font(title, &lv_font_chinese_14, 0);
    lv_obj_set_style_text_decor(title, LV_TEXT_DECOR_UNDERLINE, 0);

    /* ---- 未运行：选时长 ---- */
    s_pomo_idle_cont = lv_obj_create(s_pomo_page);
    lv_obj_set_size(s_pomo_idle_cont, 200, 160);
    lv_obj_set_pos(s_pomo_idle_cont, 0, 40);
    lv_obj_set_style_bg_opa(s_pomo_idle_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_pomo_idle_cont, 0, 0);
    lv_obj_set_style_pad_all(s_pomo_idle_cont, 0, 0);

    lv_obj_t *hint = lv_label_create(s_pomo_idle_cont);
    lv_obj_set_pos(hint, 0, 10);
    lv_obj_set_size(hint, 200, 16);
    lv_label_set_text(hint, "选择时长");
    lv_obj_set_style_text_font(hint, &lv_font_chinese_14, 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);

    for (int i = 0; i < 3; i++) {
        lv_obj_t *btn = lv_btn_create(s_pomo_idle_cont);
        lv_obj_set_size(btn, 50, 36);
        lv_obj_set_pos(btn, 20 + i * 55, 36);
        lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_radius(btn, 4, 0);
        lv_obj_add_event_cb(btn, pomo_start_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)POMO_PRESETS[i]);
        lv_obj_t *lbl = lv_label_create(btn);
        char buf[8];
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)POMO_PRESETS[i]);
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
        lv_obj_center(lbl);
    }

    lv_obj_t *unit = lv_label_create(s_pomo_idle_cont);
    lv_obj_set_pos(unit, 0, 80);
    lv_obj_set_size(unit, 200, 16);
    lv_label_set_text(unit, "分钟");
    lv_obj_set_style_text_font(unit, &lv_font_chinese_14, 0);
    lv_obj_set_style_text_align(unit, LV_TEXT_ALIGN_CENTER, 0);

    /* ---- 运行中：倒计时 + 进度条 + 取消 ---- */
    s_pomo_run_cont = lv_obj_create(s_pomo_page);
    lv_obj_set_size(s_pomo_run_cont, 200, 160);
    lv_obj_set_pos(s_pomo_run_cont, 0, 40);
    lv_obj_set_style_bg_opa(s_pomo_run_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_pomo_run_cont, 0, 0);
    lv_obj_set_style_pad_all(s_pomo_run_cont, 0, 0);

    s_pomo_min_label = lv_label_create(s_pomo_run_cont);
    lv_obj_set_pos(s_pomo_min_label, 0, 6);
    lv_obj_set_size(s_pomo_min_label, 200, 32);
    lv_label_set_text(s_pomo_min_label, "25");
    lv_obj_set_style_text_font(s_pomo_min_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_pomo_min_label, lv_color_black(), 0);
    lv_obj_set_style_text_align(s_pomo_min_label, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *minu = lv_label_create(s_pomo_run_cont);
    lv_obj_set_pos(minu, 0, 40);
    lv_obj_set_size(minu, 200, 16);
    lv_label_set_text(minu, "min");
    lv_obj_set_style_text_font(minu, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(minu, LV_TEXT_ALIGN_CENTER, 0);

    s_pomo_bar = lv_bar_create(s_pomo_run_cont);
    lv_obj_set_size(s_pomo_bar, 160, 10);
    lv_obj_set_pos(s_pomo_bar, 20, 66);
    lv_bar_set_range(s_pomo_bar, 0, 100);
    lv_obj_set_style_bg_color(s_pomo_bar, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_pomo_bar, 1, 0);
    lv_obj_set_style_border_color(s_pomo_bar, lv_color_black(), 0);
    lv_obj_set_style_bg_color(s_pomo_bar, lv_color_black(), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_pomo_bar, 0, 0);
    lv_obj_set_style_radius(s_pomo_bar, 0, LV_PART_INDICATOR);

    lv_obj_t *cancel = lv_btn_create(s_pomo_run_cont);
    lv_obj_set_size(cancel, 100, 30);
    lv_obj_set_pos(cancel, 50, 92);
    lv_obj_set_style_bg_color(cancel, lv_color_white(), 0);
    lv_obj_set_style_border_width(cancel, 1, 0);
    lv_obj_set_style_radius(cancel, 4, 0);
    lv_obj_add_event_cb(cancel, pomo_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *clbl = lv_label_create(cancel);
    lv_label_set_text(clbl, "取消");
    lv_obj_set_style_text_font(clbl, &lv_font_chinese_14, 0);
    lv_obj_set_style_text_color(clbl, lv_color_black(), 0);
    lv_obj_center(clbl);

    lv_obj_add_flag(s_pomo_run_cont, LV_OBJ_FLAG_HIDDEN);
}

static void rem_start_cb(lv_event_t *e)
{
    /* Guard: pre-check active reminder count to make the disabled-button
     * intent explicit. LV_STATE_DISABLED is styling only — clicks still
     * dispatch in LVGL v9. */
    timer_instance_t arr[TIMER_MAX_SLOTS];
    uint8_t n = 0;
    TimerEngine_GetAll(arr, &n);
    uint8_t rem_n = 0;
    for (int i = 0; i < n; i++) if (arr[i].kind == TIMER_KIND_REMINDER) rem_n++;
    if (rem_n >= 3) return;

    uint32_t min = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    uint8_t new_id;
    if (TimerEngine_StartReminder(min, &new_id) == ESP_OK) refresh_reminder_page();
}

static void confirm_yes_cb(lv_event_t *e)
{
    (void)e;
    TimerEngine_Cancel(s_confirm_id);
    lv_obj_add_flag(s_confirm_panel, LV_OBJ_FLAG_HIDDEN);
    refresh_reminder_page();
}

static void confirm_no_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_add_flag(s_confirm_panel, LV_OBJ_FLAG_HIDDEN);
}

static void rem_item_click_cb(lv_event_t *e)
{
    uint8_t id = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    s_confirm_id = id;
    lv_obj_clear_flag(s_confirm_panel, LV_OBJ_FLAG_HIDDEN);
}

static void create_rem_page(lv_obj_t *parent)
{
    s_rem_page = make_page(parent);
    create_back_btn(s_rem_page);

    s_rem_title = lv_label_create(s_rem_page);
    lv_obj_set_pos(s_rem_title, 48, 10);
    lv_label_set_text(s_rem_title, "定时提醒 (0/3)");
    lv_obj_set_style_text_font(s_rem_title, &lv_font_chinese_14, 0);
    lv_obj_set_style_text_decor(s_rem_title, LV_TEXT_DECOR_UNDERLINE, 0);

    /* 6 预设按钮 2x3 */
    for (int i = 0; i < 6; i++) {
        lv_obj_t *btn = lv_btn_create(s_rem_page);
        lv_obj_set_size(btn, 55, 30);
        lv_obj_set_pos(btn, 15 + (i % 3) * 60, 34 + (i / 3) * 36);
        lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_radius(btn, 4, 0);
        lv_obj_add_event_cb(btn, rem_start_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)REM_PRESETS[i]);
        lv_obj_t *lbl = lv_label_create(btn);
        char buf[8];
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)REM_PRESETS[i]);
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
        lv_obj_center(lbl);
        s_rem_btns[i] = btn;
    }

    lv_obj_t *hint = lv_label_create(s_rem_page);
    lv_obj_set_pos(hint, 0, 110);
    lv_obj_set_size(hint, 200, 14);
    lv_label_set_text(hint, "分钟后提醒");
    lv_obj_set_style_text_font(hint, &lv_font_chinese_14, 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);

    /* 已挂列表 */
    s_rem_list_cont = lv_obj_create(s_rem_page);
    lv_obj_set_size(s_rem_list_cont, 190, 66);
    lv_obj_set_pos(s_rem_list_cont, 5, 128);
    lv_obj_set_style_bg_opa(s_rem_list_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_rem_list_cont, 0, 0);
    lv_obj_set_style_pad_all(s_rem_list_cont, 0, 0);
    lv_obj_set_style_pad_row(s_rem_list_cont, 2, 0);
    lv_obj_set_flex_flow(s_rem_list_cont, LV_FLEX_FLOW_COLUMN);

    /* 取消确认面板（隐藏，覆盖在 reminder 页上） */
    s_confirm_panel = lv_obj_create(s_rem_page);
    lv_obj_set_size(s_confirm_panel, 130, 70);
    lv_obj_set_pos(s_confirm_panel, 35, 65);
    lv_obj_set_style_bg_color(s_confirm_panel, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_confirm_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_confirm_panel, 1, 0);
    lv_obj_set_style_radius(s_confirm_panel, 4, 0);
    lv_obj_set_style_pad_all(s_confirm_panel, 4, 0);

    lv_obj_t *q = lv_label_create(s_confirm_panel);
    lv_obj_set_pos(q, 0, 6);
    lv_obj_set_size(q, 122, 16);
    lv_label_set_text(q, "确认取消？");
    lv_obj_set_style_text_font(q, &lv_font_chinese_14, 0);
    lv_obj_set_style_text_align(q, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *no_btn = lv_btn_create(s_confirm_panel);
    lv_obj_set_size(no_btn, 50, 24);
    lv_obj_set_pos(no_btn, 8, 34);
    lv_obj_set_style_bg_color(no_btn, lv_color_white(), 0);
    lv_obj_set_style_border_width(no_btn, 1, 0);
    lv_obj_set_style_radius(no_btn, 4, 0);
    lv_obj_add_event_cb(no_btn, confirm_no_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *nlbl = lv_label_create(no_btn);
    lv_label_set_text(nlbl, "否");
    lv_obj_set_style_text_font(nlbl, &lv_font_chinese_14, 0);
    lv_obj_center(nlbl);

    lv_obj_t *yes_btn = lv_btn_create(s_confirm_panel);
    lv_obj_set_size(yes_btn, 50, 24);
    lv_obj_set_pos(yes_btn, 72, 34);
    lv_obj_set_style_bg_color(yes_btn, lv_color_white(), 0);
    lv_obj_set_style_border_width(yes_btn, 1, 0);
    lv_obj_set_style_radius(yes_btn, 4, 0);
    lv_obj_add_event_cb(yes_btn, confirm_yes_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ylbl = lv_label_create(yes_btn);
    lv_label_set_text(ylbl, "是");
    lv_obj_set_style_text_font(ylbl, &lv_font_chinese_14, 0);
    lv_obj_center(ylbl);

    lv_obj_add_flag(s_confirm_panel, LV_OBJ_FLAG_HIDDEN);
}

static void refresh_pomodoro_page(void)
{
    const timer_instance_t *p = TimerEngine_GetPomodoro();
    if (p) {
        lv_obj_add_flag(s_pomo_idle_cont, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_pomo_run_cont, LV_OBJ_FLAG_HIDDEN);
        char buf[12];
        snprintf(buf, sizeof(buf), "%u", (unsigned)((p->remaining_sec + 59) / 60));
        lv_label_set_text(s_pomo_min_label, buf);
        int pct = (int)(100 * (p->total_seconds - p->remaining_sec) / p->total_seconds);
        lv_bar_set_value(s_pomo_bar, pct, LV_ANIM_OFF);
    } else {
        lv_obj_clear_flag(s_pomo_idle_cont, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_pomo_run_cont, LV_OBJ_FLAG_HIDDEN);
    }
}
static void refresh_reminder_page(void)
{
    lv_obj_clean(s_rem_list_cont);

    timer_instance_t all[TIMER_MAX_SLOTS];
    uint8_t n = 0;
    TimerEngine_GetAll(all, &n);

    uint8_t rem_count = 0;
    for (int i = 0; i < n; i++) {
        if (all[i].kind != TIMER_KIND_REMINDER) continue;
        uint32_t min = (all[i].remaining_sec + 59) / 60;
        char buf[24];
        snprintf(buf, sizeof(buf), "还剩 %u min", (unsigned)min);

        lv_obj_t *btn = lv_btn_create(s_rem_list_cont);
        lv_obj_set_size(btn, 188, 20);
        lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_radius(btn, 2, 0);
        lv_obj_add_event_cb(btn, rem_item_click_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)all[i].id);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_font(lbl, &lv_font_chinese_14, 0);
        lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
        lv_obj_center(lbl);
        rem_count++;
    }

    /* 满 3 个 = 预设按钮置灰 */
    bool full = (rem_count >= 3);
    for (int i = 0; i < 6; i++) {
        if (full) lv_obj_add_state(s_rem_btns[i], LV_STATE_DISABLED);
        else      lv_obj_clear_state(s_rem_btns[i], LV_STATE_DISABLED);
    }

    char t[32];
    snprintf(t, sizeof(t), "定时提醒 (%u/3)", rem_count);
    lv_label_set_text(s_rem_title, t);
}

/* ============ 满屏到点提醒 ============ */
static void alert_click_cb(lv_event_t *e)
{
    (void)e;
    s_alert_showing = false;
    if (s_last_fired_kind == TIMER_KIND_POMODORO) {
        /* 番茄钟到点 → 回番茄钟页（已变未运行） */
        TimerUi_ShowPage(TIMER_PAGE_POMODORO);
        lv_screen_load(s_timer_screen);
    } else {
        /* 提醒到点 → 回到弹提醒前的屏幕 */
        lv_screen_load(s_screen_before_alert);
    }
    /* 下一秒 TickUpdate 会自动弹队列里下一个 */
}

static void build_alert_screen(void)
{
    s_alert_screen = lv_obj_create(NULL);
    lv_obj_set_size(s_alert_screen, 200, 200);
    lv_obj_set_scrollbar_mode(s_alert_screen, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(s_alert_screen, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_alert_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_alert_screen, 0, 0);
    lv_obj_set_style_radius(s_alert_screen, 0, 0);
    lv_obj_set_style_pad_all(s_alert_screen, 0, 0);
    lv_obj_add_flag(s_alert_screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_alert_screen, alert_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *bell = lv_label_create(s_alert_screen);
    lv_obj_set_pos(bell, 0, 18);
    lv_obj_set_size(bell, 200, 32);
    lv_label_set_text(bell, LV_SYMBOL_BELL);
    lv_obj_set_style_text_font(bell, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_align(bell, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *title = lv_label_create(s_alert_screen);
    lv_obj_set_pos(title, 0, 56);
    lv_obj_set_size(title, 200, 18);
    lv_label_set_text(title, "时间到");
    lv_obj_set_style_text_font(title, &lv_font_chinese_14, 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

    s_alert_type_lbl = lv_label_create(s_alert_screen);
    lv_obj_set_pos(s_alert_type_lbl, 0, 80);
    lv_obj_set_size(s_alert_type_lbl, 200, 16);
    lv_obj_set_style_text_font(s_alert_type_lbl, &lv_font_chinese_14, 0);
    lv_obj_set_style_text_align(s_alert_type_lbl, LV_TEXT_ALIGN_CENTER, 0);

    s_alert_dur_lbl = lv_label_create(s_alert_screen);
    lv_obj_set_pos(s_alert_dur_lbl, 0, 100);
    lv_obj_set_size(s_alert_dur_lbl, 200, 16);
    lv_obj_set_style_text_font(s_alert_dur_lbl, &lv_font_chinese_14, 0);
    lv_obj_set_style_text_align(s_alert_dur_lbl, LV_TEXT_ALIGN_CENTER, 0);

    s_alert_time_lbl = lv_label_create(s_alert_screen);
    lv_obj_set_pos(s_alert_time_lbl, 0, 124);
    lv_obj_set_size(s_alert_time_lbl, 200, 32);
    lv_obj_set_style_text_font(s_alert_time_lbl, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_align(s_alert_time_lbl, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *hint = lv_label_create(s_alert_screen);
    lv_obj_set_pos(hint, 0, 172);
    lv_obj_set_size(hint, 200, 16);
    lv_label_set_text(hint, "点屏幕任意处关闭");
    lv_obj_set_style_text_font(hint, &lv_font_chinese_14, 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
}

static void fill_alert_content(const fired_item_t *item)
{
    char buf[64];
    if (item->kind == TIMER_KIND_POMODORO) {
        lv_label_set_text(s_alert_type_lbl, "番茄钟");
        snprintf(buf, sizeof(buf), "%u 分钟已完成", (unsigned)(item->total_seconds / 60));
    } else {
        lv_label_set_text(s_alert_type_lbl, "定时提醒");
        snprintf(buf, sizeof(buf), "%u 分钟已到", (unsigned)(item->total_seconds / 60));
    }
    lv_label_set_text(s_alert_dur_lbl, buf);

    rtc_datetime_t dt = PortRtc_GetTime();
    snprintf(buf, sizeof(buf), "%02u:%02u", dt.hour, dt.minute);
    lv_label_set_text(s_alert_time_lbl, buf);
}

static void show_alert(const fired_item_t *item)
{
    s_alert_showing = true;
    s_last_fired_kind = item->kind;
    s_screen_before_alert = lv_screen_active();
    if (!s_alert_screen) build_alert_screen();
    fill_alert_content(item);
    lv_screen_load(s_alert_screen);
    ESP_LOGI(TAG, "Alert shown: kind=%d", item->kind);
}

static void refresh_visible_page(void)
{
    if (s_current_page == TIMER_PAGE_POMODORO) refresh_pomodoro_page();
    else if (s_current_page == TIMER_PAGE_REMINDER) refresh_reminder_page();
}

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
    if (!s_fired_queue) return;
    fired_item_t item = { id, kind, total_seconds };
    xQueueSend(s_fired_queue, &item, 0);
}

void TimerUi_TickUpdate(void)
{
    if (!s_fired_queue) return;

    /* 1. 每秒轮询到点队列 */
    if (!s_alert_showing) {
        fired_item_t item;
        if (xQueueReceive(s_fired_queue, &item, 0) == pdTRUE) {
            show_alert(&item);
        }
    }
    if (s_alert_showing) return;

    /* 2. 每分钟刷新可见的 timer 子页面（倒计时数字/进度条/提醒列表） */
    static uint32_t sec = 0;
    sec++;
    if (lv_screen_active() == s_timer_screen && (sec % 60 == 0)) {
        refresh_visible_page();
    }
}
