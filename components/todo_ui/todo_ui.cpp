#include "todo_ui.h"
#include "todo_manager.h"
#include "font_cn.h"
#include <cstring>
#include <cstdio>
#include <esp_log.h>
#include <lvgl.h>

static const char *TAG = "todo_ui";

#define ITEMS_PER_PAGE  4

static lv_obj_t *s_todo_screen = NULL;
static lv_obj_t *s_lists_page = NULL;
static lv_obj_t *s_tasks_page = NULL;
static char s_current_list_id[8] = {0};
static todo_page_type_t s_current_page = TODO_PAGE_LISTS;

/* Stable storage for IDs passed as user_data to LVGL callbacks.
 * Stack-allocated `lists[]` / `tasks[]` arrays in refresh_*() go away
 * the moment the function returns, so we MUST copy IDs into persistent
 * memory before handing pointers to LVGL events. */
static char s_list_id_pool[TODO_MAX_LISTS][TODO_ID_LEN];
static char s_task_id_pool[TODO_MAX_TASKS][TODO_ID_LEN];

/* Pagination state */
static uint8_t s_list_page = 0;
static uint8_t s_task_page = 0;
static uint8_t s_list_total = 0;
static uint16_t s_task_total = 0;

static void task_check_cb(lv_event_t *e);
static void task_long_press_cb(lv_event_t *e);
static void refresh_lists_page(void);
static void refresh_tasks_page(void);

// Back callback
static void (*s_back_cb)(void) = NULL;

// 清单页组件
static lv_obj_t *s_list_container = NULL;
static lv_obj_t *s_list_title = NULL;
static lv_obj_t *s_list_pager = NULL;
static lv_obj_t *s_list_page_label = NULL;
static lv_obj_t *s_list_prev_btn = NULL;
static lv_obj_t *s_list_next_btn = NULL;

// 任务页组件
static lv_obj_t *s_task_container = NULL;
static lv_obj_t *s_task_title = NULL;
static lv_obj_t *s_task_pager = NULL;
static lv_obj_t *s_task_page_label = NULL;
static lv_obj_t *s_task_prev_btn = NULL;
static lv_obj_t *s_task_next_btn = NULL;

/* ---- Pagination helpers ---- */

/* Swipe gesture on todo screen → go back (left-to-right swipe = back) */
static void swipe_gesture_cb(lv_event_t *e)
{
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
    if (dir == LV_DIR_RIGHT) {
        if (s_current_page == TODO_PAGE_TASKS) {
            TodoUi_ShowPage(TODO_PAGE_LISTS, NULL);
        } else if (s_back_cb) {
            s_back_cb();
        }
    }
}

static void list_prev_cb(lv_event_t *e) { (void)e; if (s_list_page > 0) { s_list_page--; refresh_lists_page(); } }
static void list_next_cb(lv_event_t *e) { (void)e; uint8_t total = (s_list_total + ITEMS_PER_PAGE - 1) / ITEMS_PER_PAGE; if (s_list_page + 1 < total) { s_list_page++; refresh_lists_page(); } }
static void task_prev_cb(lv_event_t *e) { (void)e; if (s_task_page > 0) { s_task_page--; refresh_tasks_page(); } }
static void task_next_cb(lv_event_t *e) { (void)e; uint8_t total = (s_task_total + ITEMS_PER_PAGE - 1) / ITEMS_PER_PAGE; if (s_task_page + 1 < total) { s_task_page++; refresh_tasks_page(); } }

/* Create a pager bar: ◀ page/total ▶
 * Returns the pager container. Writes child pointers into out_label/out_prev/out_next. */
static lv_obj_t *create_pager(lv_obj_t *parent, lv_obj_t **out_label,
                              lv_obj_t **out_prev, lv_obj_t **out_next,
                              lv_event_cb_t prev_cb, lv_event_cb_t next_cb)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, 196, 24);
    lv_obj_set_pos(bar, 2, 174);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, 4, 0);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    /* ◀ prev button */
    lv_obj_t *prev = lv_btn_create(bar);
    lv_obj_set_size(prev, 28, 22);
    lv_obj_set_style_bg_color(prev, lv_color_white(), 0);
    lv_obj_set_style_border_width(prev, 1, 0);
    lv_obj_set_style_radius(prev, 2, 0);
    lv_obj_set_style_shadow_width(prev, 0, 0);
    lv_obj_set_style_pad_all(prev, 0, 0);
    lv_obj_add_event_cb(prev, prev_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *pl = lv_label_create(prev);
    lv_label_set_text(pl, "<");
    lv_obj_set_style_text_font(pl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(pl, lv_color_black(), 0);
    lv_obj_center(pl);

    /* page indicator label */
    lv_obj_t *lbl = lv_label_create(bar);
    lv_label_set_text(lbl, "1/1");
    lv_obj_set_style_text_font(lbl, &lv_font_chinese_14, 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);

    /* ▶ next button */
    lv_obj_t *next = lv_btn_create(bar);
    lv_obj_set_size(next, 28, 22);
    lv_obj_set_style_bg_color(next, lv_color_white(), 0);
    lv_obj_set_style_border_width(next, 1, 0);
    lv_obj_set_style_radius(next, 2, 0);
    lv_obj_set_style_shadow_width(next, 0, 0);
    lv_obj_set_style_pad_all(next, 0, 0);
    lv_obj_add_event_cb(next, next_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *nl = lv_label_create(next);
    lv_label_set_text(nl, ">");
    lv_obj_set_style_text_font(nl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(nl, lv_color_black(), 0);
    lv_obj_center(nl);

    *out_label = lbl;
    *out_prev = prev;
    *out_next = next;
    return bar;
}

static void update_pager(lv_obj_t *label, lv_obj_t *prev_btn, lv_obj_t *next_btn,
                         uint8_t page, uint16_t total)
{
    uint8_t pages = (total + ITEMS_PER_PAGE - 1) / ITEMS_PER_PAGE;
    if (pages == 0) pages = 1;
    char buf[12];
    snprintf(buf, sizeof(buf), "%d/%d", (int)(page + 1), (int)pages);
    lv_label_set_text(label, buf);
    /* Dim disabled buttons */
    lv_obj_set_style_border_color(prev_btn, page > 0 ? lv_color_black() : lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_border_color(next_btn, (page + 1 < pages) ? lv_color_black() : lv_color_hex(0xCCCCCC), 0);
}

static void create_lists_page(lv_obj_t *parent)
{
    s_lists_page = lv_obj_create(parent);
    lv_obj_set_size(s_lists_page, 200, 200);
    lv_obj_set_pos(s_lists_page, 0, 0);
    lv_obj_set_style_bg_color(s_lists_page, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_lists_page, 0, 0);
    lv_obj_set_style_radius(s_lists_page, 0, 0);
    lv_obj_set_style_pad_all(s_lists_page, 0, 0);

    /* Title row: centered, full-width — no back button, swipe to go back */
    s_list_title = lv_label_create(s_lists_page);
    lv_obj_set_pos(s_list_title, 0, 10);
    lv_obj_set_size(s_list_title, 200, 16);
    lv_label_set_text(s_list_title, "My Lists");
    lv_obj_set_style_text_font(s_list_title, &lv_font_chinese_14, 0);
    lv_obj_set_style_text_color(s_list_title, lv_color_black(), 0);
    lv_obj_set_style_text_align(s_list_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_decor(s_list_title, LV_TEXT_DECOR_UNDERLINE, 0);

    s_list_container = lv_obj_create(s_lists_page);
    lv_obj_set_size(s_list_container, 196, 140);
    lv_obj_set_pos(s_list_container, 2, 30);
    lv_obj_set_style_bg_opa(s_list_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list_container, 0, 0);
    lv_obj_set_style_pad_all(s_list_container, 2, 0);
    lv_obj_set_flex_flow(s_list_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(s_list_container, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(s_list_container, LV_OBJ_FLAG_SCROLLABLE);

    /* Pager bar */
    s_list_pager = create_pager(s_lists_page, &s_list_page_label,
                                &s_list_prev_btn, &s_list_next_btn,
                                list_prev_cb, list_next_cb);

    /* Let swipe gestures bubble up to the screen handler */
    lv_obj_add_flag(s_lists_page, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(s_list_container, LV_OBJ_FLAG_GESTURE_BUBBLE);
}

static void create_tasks_page(lv_obj_t *parent)
{
    s_tasks_page = lv_obj_create(parent);
    lv_obj_set_size(s_tasks_page, 200, 200);
    lv_obj_set_pos(s_tasks_page, 0, 0);
    lv_obj_set_style_bg_color(s_tasks_page, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_tasks_page, 0, 0);
    lv_obj_set_style_radius(s_tasks_page, 0, 0);
    lv_obj_set_style_pad_all(s_tasks_page, 0, 0);

    /* Title row: centered, full-width — no back button, swipe to go back */
    s_task_title = lv_label_create(s_tasks_page);
    lv_obj_set_pos(s_task_title, 0, 10);
    lv_obj_set_size(s_task_title, 200, 16);
    lv_label_set_text(s_task_title, "Tasks");
    lv_obj_set_style_text_font(s_task_title, &lv_font_chinese_14, 0);
    lv_obj_set_style_text_color(s_task_title, lv_color_black(), 0);
    lv_obj_set_style_text_align(s_task_title, LV_TEXT_ALIGN_CENTER, 0);

    s_task_container = lv_obj_create(s_tasks_page);
    lv_obj_set_size(s_task_container, 196, 140);
    lv_obj_set_pos(s_task_container, 2, 30);
    lv_obj_set_style_bg_opa(s_task_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_task_container, 0, 0);
    lv_obj_set_style_pad_all(s_task_container, 2, 0);
    lv_obj_set_flex_flow(s_task_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(s_task_container, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(s_task_container, LV_OBJ_FLAG_SCROLLABLE);

    /* Pager bar */
    s_task_pager = create_pager(s_tasks_page, &s_task_page_label,
                                &s_task_prev_btn, &s_task_next_btn,
                                task_prev_cb, task_next_cb);

    /* Let swipe gestures bubble up to the screen handler */
    lv_obj_add_flag(s_tasks_page, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(s_task_container, LV_OBJ_FLAG_GESTURE_BUBBLE);
}

static void list_click_cb(lv_event_t *e)
{
    const char *id = (const char *)lv_event_get_user_data(e);
    TodoUi_ShowPage(TODO_PAGE_TASKS, id);
}

static void task_check_cb(lv_event_t *e)
{
    const char *task_id = (const char *)lv_event_get_user_data(e);
    lv_obj_t *cb = (lv_obj_t *)lv_event_get_target(e);
    bool done = lv_obj_has_state(cb, LV_STATE_CHECKED);
    TodoMgr_UpdateTask(task_id, &done, NULL, NULL);
}

/* Long-press on checkbox → delete task */
static void task_long_press_cb(lv_event_t *e)
{
    const char *task_id = (const char *)lv_event_get_user_data(e);
    ESP_LOGI(TAG, "Long press delete: %s", task_id);
    TodoMgr_DeleteTask(task_id);
    refresh_tasks_page();
}

static void refresh_lists_page(void)
{
    lv_obj_clean(s_list_container);

    todo_list_t lists[TODO_MAX_LISTS];
    uint8_t count = 0;
    TodoMgr_GetLists(lists, &count);
    s_list_total = count;

    /* Clamp page index */
    uint8_t pages = (count + ITEMS_PER_PAGE - 1) / ITEMS_PER_PAGE;
    if (pages == 0) pages = 1;
    if (s_list_page >= pages) s_list_page = pages - 1;

    if (count == 0) {
        lv_obj_t *lbl = lv_label_create(s_list_container);
        lv_label_set_text(lbl, "No lists yet");
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x999999), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_chinese_14, 0);
        update_pager(s_list_page_label, s_list_prev_btn, s_list_next_btn, s_list_page, 0);
        return;
    }

    uint8_t start = s_list_page * ITEMS_PER_PAGE;
    uint8_t end = start + ITEMS_PER_PAGE;
    if (end > count) end = count;

    for (int i = start; i < end; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s %s  (%d)",
                 lists[i].icon, lists[i].name, lists[i].task_count);

        /* Copy ID into stable static pool — stack `lists[]` dies on return */
        if (i < TODO_MAX_LISTS) {
            strncpy(s_list_id_pool[i], lists[i].id, TODO_ID_LEN - 1);
            s_list_id_pool[i][TODO_ID_LEN - 1] = '\0';
        }

        lv_obj_t *btn = lv_btn_create(s_list_container);
        lv_obj_set_size(btn, 190, 28);
        lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_add_event_cb(btn, list_click_cb, LV_EVENT_CLICKED, (void *)s_list_id_pool[i]);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_chinese_14, 0);
        lv_obj_center(lbl);
    }

    update_pager(s_list_page_label, s_list_prev_btn, s_list_next_btn, s_list_page, count);
}

static void refresh_tasks_page(void)
{
    if (s_current_list_id[0] == '\0') return;

    lv_obj_clean(s_task_container);

    // 更新标题
    todo_list_t lists[TODO_MAX_LISTS];
    uint8_t list_count = 0;
    TodoMgr_GetLists(lists, &list_count);

    for (int i = 0; i < list_count; i++) {
        if (strcmp(lists[i].id, s_current_list_id) == 0) {
            char title[48];
            snprintf(title, sizeof(title), "%s %s", lists[i].icon, lists[i].name);
            lv_label_set_text(s_task_title, title);
            break;
        }
    }

    todo_task_t tasks[TODO_MAX_TASKS];
    uint16_t task_count = 0;
    TodoMgr_GetTasks(s_current_list_id, tasks, &task_count);
    s_task_total = task_count;

    /* Clamp page index */
    uint8_t pages = (task_count + ITEMS_PER_PAGE - 1) / ITEMS_PER_PAGE;
    if (pages == 0) pages = 1;
    if (s_task_page >= pages) s_task_page = pages - 1;

    if (task_count == 0) {
        lv_obj_t *lbl = lv_label_create(s_task_container);
        lv_label_set_text(lbl, "No tasks");
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x999999), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_chinese_14, 0);
        update_pager(s_task_page_label, s_task_prev_btn, s_task_next_btn, s_task_page, 0);
        return;
    }

    uint16_t start = s_task_page * ITEMS_PER_PAGE;
    uint16_t end = start + ITEMS_PER_PAGE;
    if (end > task_count) end = task_count;

    for (int i = start; i < end; i++) {
        lv_obj_t *cb = lv_checkbox_create(s_task_container);
        char label[80];
        const char *pri = "";
        if (tasks[i].priority == 1) pri = " [!]";
        else if (tasks[i].priority == 2) pri = " [!!]";

        snprintf(label, sizeof(label), "%s%s%s%s",
                 tasks[i].text,
                 pri,
                 tasks[i].due[0] ? " " : "",
                 tasks[i].due);
        lv_checkbox_set_text(cb, label);
        /* Checkbox text defaults to Montserrat (no CJK) — use the
         * chinese font so task text in 中文 displays correctly. */
        lv_obj_set_style_text_font(cb, &lv_font_chinese_14, LV_PART_MAIN);

        if (tasks[i].done) {
            lv_obj_add_state(cb, LV_STATE_CHECKED);
            lv_obj_set_style_text_color(cb, lv_color_black(), 0);
            lv_obj_set_style_text_decor(cb, LV_TEXT_DECOR_STRIKETHROUGH, LV_PART_MAIN);
        }

        /* Enable text wrapping on the checkbox's internal label */
        lv_obj_t *cb_label = lv_obj_get_child(cb, 1);  /* child 0=indicator, child 1=label */
        if (cb_label) {
            lv_label_set_long_mode(cb_label, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(cb_label, 150);
        }

        /* Limit checkbox width so flex layout triggers label wrapping */
        lv_obj_set_width(cb, 192);

        /* Copy ID into stable static pool — stack `tasks[]` dies on return */
        if (i < TODO_MAX_TASKS) {
            strncpy(s_task_id_pool[i], tasks[i].id, TODO_ID_LEN - 1);
            s_task_id_pool[i][TODO_ID_LEN - 1] = '\0';
        }
        lv_obj_add_event_cb(cb, task_check_cb, LV_EVENT_VALUE_CHANGED, (void *)s_task_id_pool[i]);
        lv_obj_add_event_cb(cb, task_long_press_cb, LV_EVENT_LONG_PRESSED, (void *)s_task_id_pool[i]);
    }

    update_pager(s_task_page_label, s_task_prev_btn, s_task_next_btn, s_task_page, task_count);
}

esp_err_t TodoUi_Init(void)
{
    /* Create a standalone LVGL screen for TODO */
    s_todo_screen = lv_obj_create(NULL);
    lv_obj_set_size(s_todo_screen, 200, 200);
    lv_obj_set_scrollbar_mode(s_todo_screen, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(s_todo_screen, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_todo_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_todo_screen, 0, 0);
    lv_obj_set_style_radius(s_todo_screen, 0, 0);
    lv_obj_set_style_pad_all(s_todo_screen, 0, 0);

    create_lists_page(s_todo_screen);
    create_tasks_page(s_todo_screen);

    /* Show lists page initially */
    lv_obj_clear_flag(s_lists_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_tasks_page, LV_OBJ_FLAG_HIDDEN);

    /* Swipe gesture on the screen itself to go back */
    lv_obj_add_event_cb(s_todo_screen, swipe_gesture_cb, LV_EVENT_GESTURE, NULL);

    refresh_lists_page();

    return ESP_OK;
}

void TodoUi_Refresh(void)
{
    if (!s_todo_screen) return;
    if (s_current_page == TODO_PAGE_LISTS) {
        refresh_lists_page();
    } else {
        refresh_tasks_page();
    }
}

void TodoUi_ShowPage(todo_page_type_t page, const char *list_id)
{
    s_current_page = page;

    if (page == TODO_PAGE_LISTS) {
        lv_obj_clear_flag(s_lists_page, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_tasks_page, LV_OBJ_FLAG_HIDDEN);
        s_list_page = 0;
        refresh_lists_page();
    } else {
        lv_obj_add_flag(s_lists_page, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_tasks_page, LV_OBJ_FLAG_HIDDEN);
        if (list_id) {
            strncpy(s_current_list_id, list_id, sizeof(s_current_list_id) - 1);
            s_current_list_id[sizeof(s_current_list_id) - 1] = '\0';
        }
        s_task_page = 0;
        refresh_tasks_page();
    }
}

todo_page_type_t TodoUi_GetCurrentPage(void)
{
    return s_current_page;
}

lv_obj_t *TodoUi_GetScreen(void)
{
    return s_todo_screen;
}

void TodoUi_SetBackCallback(void (*cb)(void))
{
    s_back_cb = cb;
}
