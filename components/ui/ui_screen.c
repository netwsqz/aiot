#include "ui.h"
#include "lvgl_port.h"  /* EPD_WIDTH, EPD_HEIGHT */
#include "todo_ui.h"
#include "todo_manager.h"
#include "font_cn.h"
#include "timer_ui.h"
#include "timer_engine.h"
#include "weather_ui.h"
#include "wifi_ui.h"
#include "pet_ui.h"
#include "draw_ui.h"
#include "esp_heap_caps.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static lv_ui ui;
static ui_page_t s_current_page = PAGE_DASHBOARD;
static lv_obj_t *s_dashboard_screen = NULL;
static lv_obj_t *s_todo_screen = NULL;
static lv_obj_t *s_timer_screen = NULL;
static lv_obj_t *s_weather_screen = NULL;
static lv_obj_t *s_wifi_screen = NULL;
static lv_obj_t *s_pet_screen = NULL;
static lv_obj_t *s_draw_screen = NULL;

/* ── Helper: clean container (no border, no scroll, transparent) ── */
static lv_obj_t *create_quadrant(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                                  lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *cont = lv_obj_create(parent);
    lv_obj_set_pos(cont, x, y);
    lv_obj_set_size(cont, w, h);
    lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(cont, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(cont, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(cont, 0, LV_PART_MAIN);
    return cont;
}

/* ── Top-left click → enter timer tools ── */
static void timer_entry_click_cb(lv_event_t *e)
{
    (void)e;
    ui_switch_page(PAGE_TIMER_TOOLS);
}

/* ── Top-right click → enter weather page (forward decl) ── */
static void weather_entry_click_cb(lv_event_t *e);

/* ── Top-left: Time + Date ─────────────────────────── */
static void setup_top_left(lv_ui *ui)
{
    ui->cont_top_left = create_quadrant(ui->screen, 0, 0, 100, 100);

    ui->lbl_time = lv_label_create(ui->cont_top_left);
    lv_obj_set_pos(ui->lbl_time, 0, 20);
    lv_obj_set_size(ui->lbl_time, 100, 40);
    lv_label_set_text(ui->lbl_time, "14:32");
    lv_obj_set_style_text_font(ui->lbl_time, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui->lbl_time, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_text_align(ui->lbl_time, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    ui->lbl_date = lv_label_create(ui->cont_top_left);
    lv_obj_set_pos(ui->lbl_date, 0, 65);
    lv_obj_set_size(ui->lbl_date, 100, 20);
    lv_label_set_text(ui->lbl_date, "6月15日 周日");
    lv_obj_set_style_text_font(ui->lbl_date, &lv_font_chinese_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui->lbl_date, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_text_align(ui->lbl_date, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    /* Whole top-left quadrant is the entry to timer tools */
    lv_obj_add_flag(ui->cont_top_left, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ui->cont_top_left, timer_entry_click_cb, LV_EVENT_CLICKED, NULL);
}

/* ── Top-right: Weather + Temp/Humidity + Status ───── */
static void setup_top_right(lv_ui *ui)
{
    ui->cont_top_right = create_quadrant(ui->screen, 100, 0, 100, 100);

    /* Weather icon placeholder */
    ui->img_weather = lv_image_create(ui->cont_top_right);
    lv_obj_set_pos(ui->img_weather, 38, 2);
    lv_obj_set_size(ui->img_weather, 24, 24);

    /* Temperature — big anchor, shows INDOOR temp (SHTC3).
     * montserrat_20 ships U+00B0 (°), so ° renders fine here. Outdoor temp
     * and city were removed per request — this is now the only temp on the
     * home screen. Fed by ui_update_temperature() from SHTC3. */
    ui->lbl_temperature = lv_label_create(ui->cont_top_right);
    lv_obj_set_pos(ui->lbl_temperature, 0, 28);
    lv_obj_set_size(ui->lbl_temperature, 100, 22);
    lv_label_set_text(ui->lbl_temperature, "25\xC2\xB0" "C");  /* 25°C */
    lv_obj_set_style_text_font(ui->lbl_temperature, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui->lbl_temperature, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_text_align(ui->lbl_temperature, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    /* Humidity — indoor RH from SHTC3 */
    ui->lbl_humidity = lv_label_create(ui->cont_top_right);
    lv_obj_set_pos(ui->lbl_humidity, 0, 52);
    lv_obj_set_size(ui->lbl_humidity, 100, 16);
    lv_label_set_text(ui->lbl_humidity, "62%");
    lv_obj_set_style_text_font(ui->lbl_humidity, &lv_font_chinese_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui->lbl_humidity, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_text_align(ui->lbl_humidity, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    /* (lbl_indoor_temp removed — big lbl_temperature now shows indoor temp,
     * a second small indoor-temp line would be redundant.) */

    /* WiFi + Battery at bottom.
     * These are FontAwesome glyphs — chinese_14 has no symbol range, so
     * use Montserrat 14 which bundles the symbol set. */
    ui->lbl_wifi = lv_label_create(ui->cont_top_right);
    lv_obj_set_pos(ui->lbl_wifi, 4, 84);
    lv_label_set_text(ui->lbl_wifi, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(ui->lbl_wifi, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui->lbl_wifi, lv_color_black(), LV_PART_MAIN);

    ui->lbl_battery = lv_label_create(ui->cont_top_right);
    lv_obj_set_pos(ui->lbl_battery, 70, 84);
    lv_label_set_text(ui->lbl_battery, LV_SYMBOL_BATTERY_FULL);
    lv_obj_set_style_text_font(ui->lbl_battery, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui->lbl_battery, lv_color_black(), LV_PART_MAIN);

    /* Whole top-right quadrant is the entry to the weather page */
    lv_obj_add_flag(ui->cont_top_right, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ui->cont_top_right, weather_entry_click_cb, LV_EVENT_CLICKED, NULL);
}

/* ── Bottom-left quadrant click → navigate to TODO ── */
static void todo_quadrant_click_cb(lv_event_t *e)
{
    (void)e;
    ui_switch_page(PAGE_TODO_LISTS);
}

/* ── Bottom-left: Todo list ────────────────────────── */
static void setup_bottom_left(lv_ui *ui)
{
    ui->cont_bottom_left = create_quadrant(ui->screen, 0, 100, 100, 100);

    /* Make the whole quadrant clickable */
    lv_obj_add_flag(ui->cont_bottom_left, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ui->cont_bottom_left, todo_quadrant_click_cb, LV_EVENT_CLICKED, NULL);

    ui->lbl_todo_title = lv_label_create(ui->cont_bottom_left);
    lv_obj_set_pos(ui->lbl_todo_title, 4, 4);
    lv_label_set_text(ui->lbl_todo_title, "待办");
    lv_obj_set_style_text_font(ui->lbl_todo_title, &lv_font_chinese_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui->lbl_todo_title, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_text_decor(ui->lbl_todo_title, LV_TEXT_DECOR_UNDERLINE, LV_PART_MAIN);

    lv_obj_t **todo_handles[] = { &ui->lbl_todo_1, &ui->lbl_todo_2, &ui->lbl_todo_3 };

    for (int i = 0; i < 3; i++) {
        lv_obj_t *lbl = lv_label_create(ui->cont_bottom_left);
        lv_obj_set_pos(lbl, 4, 24 + i * 22);
        lv_obj_set_size(lbl, 92, 20);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_label_set_text(lbl, "");
        lv_obj_set_style_text_font(lbl, &lv_font_chinese_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(lbl, lv_color_black(), LV_PART_MAIN);
        *todo_handles[i] = lbl;
    }
}

/* ── Bottom-right: 画板入口 ───────────────────────── */
/* 点击右下角 → 进入画板。widget 句柄仍叫 lbl_image_placeholder
 * （保留 ui 结构体字段名，避免改动外层），实际是简洁的中文「画」入口提示；
 * 缩略图由独立的 img_draw_thumb canvas 承载。pomodoro 运行时二者都隐藏
 * （见 ui_update_timer_mini）。画板为次要功能，入口做弱化处理。*/
static void draw_entry_click_cb(lv_event_t *e)
{
    (void)e;
    ui_switch_page(PAGE_DRAW);
}

/* 缩略图尺寸（与 draw_ui 的 THUMB_W/H 对应：右下角占位区 100×100）*/
#define UI_THUMB_W 100
#define UI_THUMB_H 100

/* 缩略图 canvas buffer（100×100 ARGB8888 = 40KB，PSRAM）。static 全局唯一。*/
static lv_color32_t *s_thumb_buf = NULL;

/* 把已保存的画板作品降采样渲染到右下角缩略图 canvas；无作品则显「画」文字。
 * 须在 LVGL 锁内调用。pomodoro 运行时缩略图 canvas 隐藏，本函数仍可安全调用。*/
static void draw_placeholder_refresh(lv_ui *ui)
{
    if (DrawUi_HasSavedImage() && s_thumb_buf &&
        DrawUi_RenderThumbnail((uint8_t *)s_thumb_buf, UI_THUMB_W * UI_THUMB_H * 4)) {
        lv_obj_clear_flag(ui->img_draw_thumb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(ui->img_draw_thumb);
        lv_label_set_text(ui->lbl_image_placeholder, "");
    } else {
        lv_obj_add_flag(ui->img_draw_thumb, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(ui->lbl_image_placeholder, "画");
    }
}

static void setup_bottom_right(lv_ui *ui)
{
    ui->cont_bottom_right = create_quadrant(ui->screen, 100, 100, 100, 100);

    /* 整个右下角象限可点击 → 画板 */
    lv_obj_add_flag(ui->cont_bottom_right, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ui->cont_bottom_right, draw_entry_click_cb, LV_EVENT_CLICKED, NULL);

    /* 缩略图 canvas（100×100，铺满象限）。懒分配 PSRAM。
     * 无作品时 hidden，显示下方画板入口文字「画」。*/
    s_thumb_buf = (lv_color32_t *)heap_caps_malloc(
        UI_THUMB_W * UI_THUMB_H * sizeof(lv_color32_t), MALLOC_CAP_SPIRAM);
    ui->img_draw_thumb = lv_canvas_create(ui->cont_bottom_right);
    if (s_thumb_buf) {
        lv_canvas_set_buffer(ui->img_draw_thumb, s_thumb_buf, UI_THUMB_W, UI_THUMB_H,
                             LV_COLOR_FORMAT_ARGB8888);
    }
    lv_obj_set_pos(ui->img_draw_thumb, 0, 0);
    lv_obj_add_flag(ui->img_draw_thumb, LV_OBJ_FLAG_HIDDEN);

    ui->lbl_image_placeholder = lv_label_create(ui->cont_bottom_right);
    lv_obj_set_pos(ui->lbl_image_placeholder, 0, 40);
    lv_obj_set_size(ui->lbl_image_placeholder, 100, 24);
    lv_label_set_text(ui->lbl_image_placeholder, "画");
    lv_obj_set_style_text_font(ui->lbl_image_placeholder, &lv_font_chinese_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui->lbl_image_placeholder, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_text_align(ui->lbl_image_placeholder, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    /* Pomodoro mini display — hidden until a pomodoro runs (Task 6 drives it) */
    ui->cont_timer_mini = lv_obj_create(ui->cont_bottom_right);
    lv_obj_set_size(ui->cont_timer_mini, 100, 100);
    lv_obj_set_pos(ui->cont_timer_mini, 0, 0);
    lv_obj_set_style_bg_opa(ui->cont_timer_mini, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ui->cont_timer_mini, 0, 0);
    lv_obj_set_style_pad_all(ui->cont_timer_mini, 0, 0);
    lv_obj_set_style_radius(ui->cont_timer_mini, 0, 0);
    lv_obj_set_scrollbar_mode(ui->cont_timer_mini, LV_SCROLLBAR_MODE_OFF);

    ui->lbl_timer_mini_title = lv_label_create(ui->cont_timer_mini);
    lv_obj_set_pos(ui->lbl_timer_mini_title, 0, 8);
    lv_obj_set_size(ui->lbl_timer_mini_title, 100, 16);
    lv_label_set_text(ui->lbl_timer_mini_title, "番茄");
    lv_obj_set_style_text_font(ui->lbl_timer_mini_title, &lv_font_chinese_14, 0);
    lv_obj_set_style_text_color(ui->lbl_timer_mini_title, lv_color_black(), 0);
    lv_obj_set_style_text_align(ui->lbl_timer_mini_title, LV_TEXT_ALIGN_CENTER, 0);

    ui->lbl_timer_mini_min = lv_label_create(ui->cont_timer_mini);
    lv_obj_set_pos(ui->lbl_timer_mini_min, 0, 28);
    lv_obj_set_size(ui->lbl_timer_mini_min, 100, 24);
    lv_label_set_text(ui->lbl_timer_mini_min, "");
    lv_obj_set_style_text_font(ui->lbl_timer_mini_min, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(ui->lbl_timer_mini_min, lv_color_black(), 0);
    lv_obj_set_style_text_align(ui->lbl_timer_mini_min, LV_TEXT_ALIGN_CENTER, 0);

    ui->bar_timer_mini = lv_bar_create(ui->cont_timer_mini);
    lv_obj_set_size(ui->bar_timer_mini, 80, 8);
    lv_obj_set_pos(ui->bar_timer_mini, 10, 64);
    lv_bar_set_range(ui->bar_timer_mini, 0, 100);
    lv_obj_set_style_bg_color(ui->bar_timer_mini, lv_color_white(), 0);
    lv_obj_set_style_border_width(ui->bar_timer_mini, 1, 0);
    lv_obj_set_style_border_color(ui->bar_timer_mini, lv_color_black(), 0);
    lv_obj_set_style_bg_color(ui->bar_timer_mini, lv_color_black(), LV_PART_INDICATOR);
    lv_obj_set_style_radius(ui->bar_timer_mini, 0, 0);
    lv_obj_set_style_radius(ui->bar_timer_mini, 0, LV_PART_INDICATOR);

    lv_obj_add_flag(ui->cont_timer_mini, LV_OBJ_FLAG_HIDDEN);
}

/* ── Public API ────────────────────────────────────── */
void ui_init(void)
{
    /* Root screen: 200×200, white, no scroll, no border */
    ui.screen = lv_obj_create(NULL);
    lv_obj_set_size(ui.screen, EPD_WIDTH, EPD_HEIGHT);
    lv_obj_set_scrollbar_mode(ui.screen, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(ui.screen, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui.screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui.screen, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(ui.screen, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ui.screen, 0, LV_PART_MAIN);

    s_dashboard_screen = ui.screen;

    setup_top_left(&ui);
    setup_top_right(&ui);
    setup_bottom_left(&ui);
    setup_bottom_right(&ui);
    draw_placeholder_refresh(&ui);   /* 有作品则隐藏占位文字 */

    lv_screen_load(ui.screen);
}

/* ── Data update functions ──────────────────────────── */
void ui_update_time(const char *time_str, const char *date_str)
{
    if (time_str)
        lv_label_set_text(ui.lbl_time, time_str);
    if (date_str)
        lv_label_set_text(ui.lbl_date, date_str);
}

void ui_update_temperature(int temp_c)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%d\xC2\xB0""C", temp_c);
    lv_label_set_text(ui.lbl_temperature, buf);
}

void ui_update_humidity(int rh_pct)
{
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", rh_pct);
    lv_label_set_text(ui.lbl_humidity, buf);
}

void ui_update_battery(uint8_t level)
{
    if (level >= 75)
        lv_label_set_text(ui.lbl_battery, LV_SYMBOL_BATTERY_FULL);
    else if (level >= 50)
        lv_label_set_text(ui.lbl_battery, LV_SYMBOL_BATTERY_3);
    else if (level >= 25)
        lv_label_set_text(ui.lbl_battery, LV_SYMBOL_BATTERY_2);
    else if (level > 0)
        lv_label_set_text(ui.lbl_battery, LV_SYMBOL_BATTERY_1);
    else
        lv_label_set_text(ui.lbl_battery, LV_SYMBOL_BATTERY_EMPTY);
}

void ui_update_wifi_status(bool connected)
{
    if (connected) {
        /* WIFI 是 FontAwesome 符号，需 Montserrat14（中文字体无符号字形） */
        lv_obj_set_style_text_font(ui.lbl_wifi, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_label_set_text(ui.lbl_wifi, LV_SYMBOL_WIFI);
    } else {
        /* 短中文提示，需中文字体（Montserrat14 无 CJK 字形） */
        lv_obj_set_style_text_font(ui.lbl_wifi, &lv_font_chinese_14, LV_PART_MAIN);
        lv_label_set_text(ui.lbl_wifi, "未连接");
    }
}

/* ── Dashboard right-bottom pomodoro mini display ── */
void ui_update_timer_mini(bool running, uint32_t remaining_sec, uint32_t total_sec)
{
    static bool     last_running = false;
    static uint32_t last_min     = 0;
    static int      last_pct     = -1;

    if (running != last_running) {
        if (running) {
            lv_obj_clear_flag(ui.cont_timer_mini, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(ui.lbl_image_placeholder, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(ui.img_draw_thumb, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(ui.cont_timer_mini, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(ui.lbl_image_placeholder, LV_OBJ_FLAG_HIDDEN);
            /* pomodoro 结束后刷新占位：可能期间画板存了新图 */
            draw_placeholder_refresh(&ui);
        }
        last_running = running;
        last_min = 0;
        last_pct = -1;
    }

    if (!running) return;

    uint32_t min = (remaining_sec + 59) / 60;
    int pct = (total_sec > 0) ? (int)(100 * (total_sec - remaining_sec) / total_sec) : 0;

    if (min != last_min) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%" PRIu32, min);
        lv_label_set_text(ui.lbl_timer_mini_min, buf);
        last_min = min;
    }
    if (pct != last_pct) {
        lv_bar_set_value(ui.bar_timer_mini, pct, LV_ANIM_OFF);
        last_pct = pct;
    }
}

/* ── Dashboard Todo summary: top 3 by priority across all lists ───── */
void ui_update_todo_summary(void)
{
    lv_obj_t *slots[3] = { ui.lbl_todo_1, ui.lbl_todo_2, ui.lbl_todo_3 };

    /* Use static storage — these arrays are ~1.6KB and would otherwise
     * land on the caller's stack. data_task and app_main both have tight
     * stacks, so keep it off the stack. */
    static todo_list_t lists[TODO_MAX_LISTS];
    static todo_task_t tasks[TODO_MAX_TASKS];
    static todo_task_t top[3];

    uint8_t list_count = 0;
    if (TodoMgr_GetLists(lists, &list_count) != ESP_OK) list_count = 0;

    int top_n = 0;
    for (int i = 0; i < list_count; i++) {
        uint16_t tcount = 0;
        if (TodoMgr_GetTasks(lists[i].id, tasks, &tcount) != ESP_OK) continue;
        for (int j = 0; j < tcount; j++) {
            if (tasks[j].done) continue;
            int pos = top_n;
            while (pos > 0 && top[pos - 1].priority < tasks[j].priority) pos--;
            if (pos >= 3) continue;
            int end = (top_n < 3) ? top_n : 2;
            for (int k = end; k > pos; k--) top[k] = top[k - 1];
            top[pos] = tasks[j];
            if (top_n < 3) top_n++;
        }
    }

    for (int i = 0; i < 3; i++) {
        if (i < top_n) {
            const char *mark = (top[i].priority == 2) ? "!! " :
                               (top[i].priority == 1) ? "! " : "";
            char buf[80];
            snprintf(buf, sizeof(buf), "%d.%s%s", i + 1, mark, top[i].text);
            lv_label_set_text(slots[i], buf);
        } else {
            lv_label_set_text(slots[i], "");
        }
    }
}

/* ── Weather summary (main-screen top-right) ──
 * Home screen no longer shows outdoor temp/city — only indoor sensors.
 * lbl_temperature/lbl_humidity are now fed directly by ui_update_temperature
 * /ui_update_humidity from SHTC3. This function kept for the page-render
 * path (fresh fetch while on the weather page) and ABI stability.
 * Call under Lvgl_Lock. */
void ui_update_weather_summary(const weather_snapshot_t *snapshot,
                               float indoor_temp_c, bool outdoor_available)
{
    (void)indoor_temp_c;
    (void)outdoor_available;

    /* Fresh fetch just completed — repaint the weather page if it's open.
     * The home screen needs no outdoor data anymore. */
    if (snapshot && ui_get_current_page() == PAGE_WEATHER) {
        WeatherUI_Render(snapshot, false);
    }
}

/* ── Top-right click → enter weather page ── */
static void weather_entry_click_cb(lv_event_t *e)
{
    (void)e;
    ui_switch_page(PAGE_WEATHER);
}

/* ── Page management ── */

/* The one back action every sub-page wants. Modules get this injected as
 * their back callback by ui_init_all(), so main.cpp declares no per-page
 * back callbacks at all. */
void ui_back_to_dashboard(void)
{
    ui_switch_page(PAGE_DASHBOARD);
}

void ui_init_all(void)
{
    /* One row per sub-page that owns a screen. Adding a page = adding a row.
     * Order matters: ui_init() must run first (it creates the dashboard this
     * table's screens are siblings of). TimerEngine_Init/Restore stay in
     * main.cpp — they're engine lifecycle, not UI.
     * Screens are lazily fetched by ui_switch_page's own GetScreen calls, so
     * only init + back-callback live here. */
    typedef struct {
        void (*init)(void);
        void (*set_back)(void (*)(void));
    } page_reg_t;

    static const page_reg_t reg[] = {
        { (void (*)(void))TimerUi_Init, TimerUi_SetBackCallback },
        { WeatherUI_Init,               WeatherUI_SetBackCallback },
        { WifiUi_Init,                  WifiUi_SetBackCallback },
        { (void (*)(void))TodoUi_Init,  TodoUi_SetBackCallback },
        { (void (*)(void))PetUi_Init,   PetUi_SetBackCallback },
        { (void (*)(void))DrawUi_Init,  DrawUi_SetBackCallback },
    };

    for (size_t i = 0; i < sizeof reg / sizeof reg[0]; i++) {
        if (!Lvgl_Lock(-1)) continue;   /* ponytail: skip rather than race; -1 never fails today */
        reg[i].init();
        reg[i].set_back(ui_back_to_dashboard);
        Lvgl_Unlock();
    }
}

void ui_switch_page(ui_page_t page)
{
    s_current_page = page;

    /* Lazy screen creation — each page's GetScreen() builds on first call. */
    lv_obj_t **slot = NULL;
    switch (page) {
        case PAGE_TODO_LISTS:
        case PAGE_TODO_TASKS: slot = &s_todo_screen;    break;
        case PAGE_TIMER_TOOLS:
        case PAGE_POMODORO:
        case PAGE_REMINDER:   slot = &s_timer_screen;   break;
        case PAGE_WEATHER:    slot = &s_weather_screen; break;
        case PAGE_WIFI_MGMT:  slot = &s_wifi_screen;    break;
        case PAGE_PET:        slot = &s_pet_screen;     break;
        case PAGE_DRAW:       slot = &s_draw_screen;    break;
        default: break;
    }
    if (slot && !*slot) {
        switch (page) {
            case PAGE_TODO_LISTS:
            case PAGE_TODO_TASKS: *slot = TodoUi_GetScreen();    break;
            case PAGE_TIMER_TOOLS:
            case PAGE_POMODORO:
            case PAGE_REMINDER:   *slot = TimerUi_GetScreen();   break;
            case PAGE_WEATHER:    *slot = WeatherUI_GetScreen(); break;
            case PAGE_WIFI_MGMT:  *slot = WifiUi_GetScreen();    break;
            case PAGE_PET:        *slot = PetUi_GetScreen();     break;
            case PAGE_DRAW:       *slot = DrawUi_GetScreen();    break;
            default: break;
        }
    }

    switch (page) {
    case PAGE_DASHBOARD:
        /* 返回 dashboard 时刷新右下角占位：画板可能刚存了新作品 */
        draw_placeholder_refresh(&ui);
        lv_screen_load(s_dashboard_screen);
        break;
    case PAGE_TODO_LISTS:
        if (s_todo_screen) {
            TodoUi_ShowPage(TODO_PAGE_LISTS, NULL);
            lv_screen_load(s_todo_screen);
        }
        break;
    case PAGE_TODO_TASKS:
        if (s_todo_screen) lv_screen_load(s_todo_screen);
        break;
    case PAGE_TIMER_TOOLS:
    case PAGE_POMODORO:
    case PAGE_REMINDER:
        if (s_timer_screen) {
            timer_page_t tp = (page == PAGE_POMODORO) ? TIMER_PAGE_POMODORO :
                              (page == PAGE_REMINDER)  ? TIMER_PAGE_REMINDER :
                                                         TIMER_PAGE_TOOLS;
            TimerUi_ShowPage(tp);
            lv_screen_load(s_timer_screen);
        }
        break;
    case PAGE_WEATHER:
        if (s_weather_screen) {
            lv_screen_load(s_weather_screen);
            /* Render the current snapshot immediately on entry. data_task
             * only renders when a *fresh* fetch completes while already on
             * this page, so without this call a cached snapshot would show
             * a blank screen full of empty labels until the next fetch. */
            WeatherUI_RenderIfCached();
        }
        break;
    case PAGE_WIFI_MGMT:
        if (s_wifi_screen) lv_screen_load(s_wifi_screen);
        break;
    case PAGE_PET:
        if (s_pet_screen) lv_screen_load(s_pet_screen);
        break;
    case PAGE_DRAW:
        if (s_draw_screen) {
            DrawUi_Enter();   /* 每次进入即清空为空白 */
            DrawUi_SetIndevFast(lv_indev_get_next(NULL));  /* 8ms 采样，避免曲线变折线 */
            lv_screen_load(s_draw_screen);
        }
        break;
    }
}

ui_page_t ui_get_current_page(void)
{
    /* Timer sub-pages can change without ui_switch_page: the right-swipe
     * handler in timer_ui calls TimerUi_ShowPage() directly. s_current_page
     * still reads PAGE_POMODORO/PAGE_REMINDER after such a swipe, which the
     * 60 s idle timer in data_task would then honour — bouncing the user off
     * a page they just navigated to. Ask timer_ui for the real sub-page. */
    if (s_current_page == PAGE_TIMER_TOOLS ||
        s_current_page == PAGE_POMODORO    ||
        s_current_page == PAGE_REMINDER) {
        switch (TimerUi_GetCurrentPage()) {
            case TIMER_PAGE_POMODORO: return PAGE_POMODORO;
            case TIMER_PAGE_REMINDER: return PAGE_REMINDER;
            default:                  return PAGE_TIMER_TOOLS;
        }
    }
    return s_current_page;
}
