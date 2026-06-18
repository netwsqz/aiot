#include "todo_ui.h"
#include "todo_manager.h"
#include "font_cn.h"
#include <cstring>
#include <cstdio>
#include <lvgl.h>

static lv_obj_t *s_todo_screen = NULL;
static lv_obj_t *s_lists_page = NULL;
static lv_obj_t *s_tasks_page = NULL;
static char s_current_list_id[8] = {0};
static todo_page_type_t s_current_page = TODO_PAGE_LISTS;

// Swipe-to-go-back state
static lv_coord_t swipe_start_x = 0;
static bool swiping = false;

// Back callback
static void (*s_back_cb)(void) = NULL;

// 清单页组件
static lv_obj_t *s_list_container = NULL;
static lv_obj_t *s_list_title = NULL;

// 任务页组件
static lv_obj_t *s_task_container = NULL;
static lv_obj_t *s_task_title = NULL;

/* Back button callback */
static void back_btn_cb(lv_event_t *e)
{
    (void)e;
    if (s_current_page == TODO_PAGE_TASKS) {
        /* In tasks page → go back to lists */
        TodoUi_ShowPage(TODO_PAGE_LISTS, NULL);
    } else if (s_back_cb) {
        /* In lists page → go back to dashboard */
        s_back_cb();
    }
}

/* Create a reusable back button */
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
    lv_obj_set_style_text_font(lbl, &lv_font_chinese_14, 0);
    lv_obj_center(lbl);

    return btn;
}

/* Swipe gesture on todo screen → go back */
static void swipe_gesture_cb(lv_event_t *e)
{
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
    /* User swipes from left to right (LV_DIR_RIGHT) = go back */
    if (dir == LV_DIR_RIGHT) {
        if (s_current_page == TODO_PAGE_TASKS) {
            TodoUi_ShowPage(TODO_PAGE_LISTS, NULL);
        } else if (s_back_cb) {
            s_back_cb();
        }
    }
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

    /* Back button */
    create_back_btn(s_lists_page);

    s_list_title = lv_label_create(s_lists_page);
    lv_obj_set_pos(s_list_title, 34, 4);
    lv_label_set_text(s_list_title, "My Lists");
    lv_obj_set_style_text_font(s_list_title, &lv_font_chinese_14, 0);
    lv_obj_set_style_text_decor(s_list_title, LV_TEXT_DECOR_UNDERLINE, 0);

    s_list_container = lv_obj_create(s_lists_page);
    lv_obj_set_size(s_list_container, 196, 170);
    lv_obj_set_pos(s_list_container, 2, 26);
    lv_obj_set_style_bg_opa(s_list_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list_container, 0, 0);
    lv_obj_set_style_pad_all(s_list_container, 2, 0);
    lv_obj_set_flex_flow(s_list_container, LV_FLEX_FLOW_COLUMN);

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

    /* Back button */
    create_back_btn(s_tasks_page);

    s_task_title = lv_label_create(s_tasks_page);
    lv_obj_set_pos(s_task_title, 34, 4);
    lv_label_set_text(s_task_title, "Tasks");
    lv_obj_set_style_text_font(s_task_title, &lv_font_chinese_14, 0);

    s_task_container = lv_obj_create(s_tasks_page);
    lv_obj_set_size(s_task_container, 196, 170);
    lv_obj_set_pos(s_task_container, 2, 26);
    lv_obj_set_style_bg_opa(s_task_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_task_container, 0, 0);
    lv_obj_set_style_pad_all(s_task_container, 2, 0);
    lv_obj_set_flex_flow(s_task_container, LV_FLEX_FLOW_COLUMN);

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

static void refresh_lists_page(void)
{
    lv_obj_clean(s_list_container);

    todo_list_t lists[TODO_MAX_LISTS];
    uint8_t count = 0;
    TodoMgr_GetLists(lists, &count);

    if (count == 0) {
        lv_obj_t *lbl = lv_label_create(s_list_container);
        lv_label_set_text(lbl, "No lists yet");
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x999999), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_chinese_14, 0);
        return;
    }

    for (int i = 0; i < count; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s %s  (%d)",
                 lists[i].icon, lists[i].name, lists[i].task_count);

        lv_obj_t *btn = lv_btn_create(s_list_container);
        lv_obj_set_size(btn, 190, 28);
        lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_add_event_cb(btn, list_click_cb, LV_EVENT_CLICKED, (void *)lists[i].id);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_chinese_14, 0);
        lv_obj_center(lbl);
    }
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

    if (task_count == 0) {
        lv_obj_t *lbl = lv_label_create(s_task_container);
        lv_label_set_text(lbl, "No tasks");
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x999999), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_chinese_14, 0);
        return;
    }

    for (int i = 0; i < task_count; i++) {
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

        if (tasks[i].done) {
            lv_obj_add_state(cb, LV_STATE_CHECKED);
            lv_obj_set_style_text_color(cb, lv_color_hex(0x999999), 0);
        }

        lv_obj_add_event_cb(cb, task_check_cb, LV_EVENT_VALUE_CHANGED, (void *)tasks[i].id);
    }
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
        refresh_lists_page();
    } else {
        lv_obj_add_flag(s_lists_page, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_tasks_page, LV_OBJ_FLAG_HIDDEN);
        if (list_id) {
            strncpy(s_current_list_id, list_id, sizeof(s_current_list_id) - 1);
            s_current_list_id[sizeof(s_current_list_id) - 1] = '\0';
        }
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
