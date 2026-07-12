#include "app_list_ui.h"

#include <string.h>
#include "lvgl.h"
#include "font_cn.h"
#include "esp_log.h"

static const char *TAG = "app_list_ui";

/* ── 尺寸常量 ── */
#define APP_LIST_W   200
#define APP_LIST_H   200

/* 标题占顶部 ~20px；网格区 y=24..196（高 172px）。
 * 2 列 × 2 行，每格 90×80，列间距 8，行间距 8：
 *   列 x: 6, 104   (6 + 90 + 8 = 104, 104 + 90 = 194, 右余 6)
 *   行 y: 24, 112  (24 + 80 + 8 = 112, 112 + 80 = 192, 下余 8) */
#define GRID_CELL_W   90
#define GRID_CELL_H   80
#define GRID_COL_GAP   8
#define GRID_ROW_GAP   8
#define GRID_X0        6
#define GRID_Y0       24
#define GRID_X1       (GRID_X0 + GRID_CELL_W + GRID_COL_GAP)   /* 104 */
#define GRID_Y1       (GRID_Y0 + GRID_CELL_H + GRID_ROW_GAP)   /* 112 */

/* ── 静态状态 ── */
static lv_obj_t *s_screen = NULL;
static void    (*s_back_cb)(void)         = NULL;
static void    (*s_enter_snake_cb)(void)  = NULL;
static void    (*s_enter_draw_cb)(void)   = NULL;

/* ── 左滑手势退出 ──
 * LVGL v9 的 indev 在手指拖动超过 gesture_min_distance 时向 screen 派发
 * LV_EVENT_GESTURE（见 lv_indev.c:indev_gesture，~1806 行设 gesture_dir）。
 * 回调里用 lv_indev_get_gesture_dir(lv_indev_get_act()) 取方向。
 * 方向 = LV_DIR_LEFT 即右→左滑，调 s_back_cb 退回 dashboard。
 * 回调本身在 LVGL 锁内派发，不要再加锁。*/
static void screen_gesture_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_LEFT) {
        if (s_back_cb) s_back_cb();
    }
}

/* 格子点击：贪吃蛇入口。回调在 LVGL 锁内派发。 */
static void snake_cell_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_enter_snake_cb) s_enter_snake_cb();
}

/* 格子点击：画板入口。回调在 LVGL 锁内派发。 */
static void draw_cell_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_enter_draw_cb) s_enter_draw_cb();
}

/* 创建一个网格格子：黑边框(2px)、白底、无圆角、CLICKABLE。
 * 标签居中（chinese_14）。pos/size 由调用方给定。 */
static lv_obj_t *create_cell(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                             const char *text, lv_event_cb_t click_cb)
{
    lv_obj_t *cell = lv_obj_create(parent);
    lv_obj_set_pos(cell, x, y);
    lv_obj_set_size(cell, GRID_CELL_W, GRID_CELL_H);
    lv_obj_set_scrollbar_mode(cell, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(cell, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cell, 2, 0);
    lv_obj_set_style_border_color(cell, lv_color_black(), 0);
    lv_obj_set_style_border_side(cell, LV_BORDER_SIDE_FULL, 0);
    lv_obj_set_style_radius(cell, 0, 0);
    lv_obj_set_style_pad_all(cell, 0, 0);
    lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(cell);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_chinese_14, 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_center(lbl);

    if (click_cb) {
        lv_obj_add_event_cb(cell, click_cb, LV_EVENT_CLICKED, NULL);
    }
    return cell;
}

/* ──────────────────────────────────────────────────────────────
 * 公开 API
 * ────────────────────────────────────────────────────────────── */
esp_err_t AppListUi_Init(void)
{
    if (s_screen) return ESP_OK;

    s_screen = lv_obj_create(NULL);
    lv_obj_set_size(s_screen, APP_LIST_W, APP_LIST_H);
    lv_obj_set_scrollbar_mode(s_screen, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(s_screen, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_set_style_radius(s_screen, 0, 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    /* CLICKABLE 让 screen 能接收 gesture/触摸；scrollable 关掉避免被当滚动容器 */
    lv_obj_add_flag(s_screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    /* 顶部标题 "应用"：chinese_14，黑字，左上角 */
    lv_obj_t *title = lv_label_create(s_screen);
    lv_label_set_text(title, "\xE5\xBA\x94\xE7\x94\xA8");  /* "应用" UTF-8 */
    lv_obj_set_style_text_font(title, &lv_font_chinese_14, 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_set_pos(title, 4, 4);

    /* 2×2 网格 */
    /* 左上：贪吃蛇 */
    create_cell(s_screen, GRID_X0, GRID_Y0,
                "\xE8\xB4\xAA\xE5\x90\x83\xE8\x9B\x87",  /* "贪吃蛇" UTF-8 */
                snake_cell_click_cb);
    /* 右上：画板 */
    create_cell(s_screen, GRID_X1, GRID_Y0,
                "\xE7\x94\xBB\xE6\x9D\xBF",              /* "画板" UTF-8 */
                draw_cell_click_cb);
    /* 左下：黑框占位（待扩展）— 黑色实心，无文字，无回调 */
    {
        lv_obj_t *cell = lv_obj_create(s_screen);
        lv_obj_set_pos(cell, GRID_X0, GRID_Y1);
        lv_obj_set_size(cell, GRID_CELL_W, GRID_CELL_H);
        lv_obj_set_scrollbar_mode(cell, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_style_bg_color(cell, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(cell, 2, 0);
        lv_obj_set_style_border_color(cell, lv_color_black(), 0);
        lv_obj_set_style_border_side(cell, LV_BORDER_SIDE_FULL, 0);
        lv_obj_set_style_radius(cell, 0, 0);
        lv_obj_set_style_pad_all(cell, 0, 0);
        /* 占位格不可点（无回调即可）；不加 CLICKABLE，点击落到 screen 不影响 */
    }
    /* 右下：黑框占位（待扩展）— 同上 */
    {
        lv_obj_t *cell = lv_obj_create(s_screen);
        lv_obj_set_pos(cell, GRID_X1, GRID_Y1);
        lv_obj_set_size(cell, GRID_CELL_W, GRID_CELL_H);
        lv_obj_set_scrollbar_mode(cell, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_style_bg_color(cell, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(cell, 2, 0);
        lv_obj_set_style_border_color(cell, lv_color_black(), 0);
        lv_obj_set_style_border_side(cell, LV_BORDER_SIDE_FULL, 0);
        lv_obj_set_style_radius(cell, 0, 0);
        lv_obj_set_style_pad_all(cell, 0, 0);
    }

    /* 左滑手势 → 退回 dashboard */
    lv_obj_add_event_cb(s_screen, screen_gesture_cb, LV_EVENT_GESTURE, NULL);

    ESP_LOGI(TAG, "app_list_ui init done");
    return ESP_OK;
}

lv_obj_t *AppListUi_GetScreen(void)
{
    return s_screen;
}

void AppListUi_Enter(void)
{
    /* e-ink 无动画，进入无操作；保留钩子供未来刷新状态用。 */
    if (!s_screen) return;
}

void AppListUi_SetBackCallback(void (*cb)(void))
{
    s_back_cb = cb;
}

void AppListUi_SetEnterSnakeCallback(void (*cb)(void))
{
    s_enter_snake_cb = cb;
}

void AppListUi_SetEnterDrawCallback(void (*cb)(void))
{
    s_enter_draw_cb = cb;
}
