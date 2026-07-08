#ifndef UI_H
#define UI_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"
#include "weather_fetch.h"

/* ── Widget handles (runtime-updatable widgets) ────── */
typedef struct {
    lv_obj_t *screen;

    /* Top-left: time + date */
    lv_obj_t *cont_top_left;
    lv_obj_t *lbl_time;
    lv_obj_t *lbl_date;

    /* Top-right: indoor temp/humidity + status (outdoor weather moved to
     * the dedicated weather page — home screen shows indoor sensors only) */
    lv_obj_t *cont_top_right;
    lv_obj_t *img_weather;
    lv_obj_t *lbl_temperature;   /* big indoor temp (montserrat_20, has °) */
    lv_obj_t *lbl_humidity;      /* indoor RH (chinese_14) */
    lv_obj_t *lbl_wifi;
    lv_obj_t *lbl_battery;

    /* Bottom-left: todo list */
    lv_obj_t *cont_bottom_left;
    lv_obj_t *lbl_todo_title;
    lv_obj_t *lbl_todo_1;
    lv_obj_t *lbl_todo_2;
    lv_obj_t *lbl_todo_3;

    /* Bottom-right: image placeholder */
    lv_obj_t *cont_bottom_right;
    lv_obj_t *lbl_image_placeholder;

    /* Bottom-right: pomodoro mini display (shown when a pomodoro is running) */
    lv_obj_t *cont_timer_mini;
    lv_obj_t *lbl_timer_mini_title;
    lv_obj_t *lbl_timer_mini_min;
    lv_obj_t *bar_timer_mini;
} lv_ui;

/** Create the full four-quadrant UI.  Must be called under LVGL lock. */
void ui_init(void);

/* ── Data update functions (call under Lvgl_Lock) ── */
void ui_update_time(const char *time_str, const char *date_str);
void ui_update_temperature(int temp_c);
void ui_update_humidity(int rh_pct);
void ui_update_battery(uint8_t level);
void ui_update_wifi_status(bool connected);
void ui_update_todo_summary(void);
void ui_update_timer_mini(bool running, uint32_t remaining_sec, uint32_t total_sec);

/* ── Weather summary (main-screen top-right) ──
 * snapshot == NULL or outdoor_available == false → show indoor sensors only.
 * Otherwise show outdoor temp/humidity + indoor temp on lbl_indoor_temp.
 * Call under Lvgl_Lock. */
void ui_update_weather_summary(const weather_snapshot_t *snapshot,
                               float indoor_temp_c, bool outdoor_available);

/* ── Page management ── */
typedef enum {
    PAGE_DASHBOARD,
    PAGE_TODO_LISTS,
    PAGE_TODO_TASKS,
    PAGE_TIMER_TOOLS,
    PAGE_POMODORO,
    PAGE_REMINDER,
    PAGE_WEATHER,
    PAGE_WIFI_MGMT,
    PAGE_PET,
} ui_page_t;

void ui_switch_page(ui_page_t page);
ui_page_t ui_get_current_page(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_H */
