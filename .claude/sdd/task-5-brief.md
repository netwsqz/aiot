## Task 5: todo_ui LVGL 页面

**目标:** 墨水屏 TODO 显示页面，支持触摸操作

**Files:**
- Create: `components/todo_ui/CMakeLists.txt`
- Create: `components/todo_ui/todo_ui.h`
- Create: `components/todo_ui/todo_ui.cpp`

**Interfaces:**
- Consumes: `todo_manager.h` API (TodoMgr_GetLists, TodoMgr_GetTasks, TodoMgr_UpdateTask)
- Produces: `TodoUi_Init()`, `TodoUi_Refresh()`, `TodoUi_ShowPage()`, `TodoUi_GetCurrentPage()`

---

### Step 1: 创建 CMakeLists.txt

```cmake
idf_component_register(
    SRCS "todo_ui.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES todo_manager lvgl epaper_driver_bsp)
```

---

### Step 2: 创建 todo_ui.h

```c
#ifndef TODO_UI_H
#define TODO_UI_H

#include <lvgl.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TODO_PAGE_LISTS,
    TODO_PAGE_TASKS,
} todo_page_type_t;

esp_err_t TodoUi_Init(lv_obj_t *parent);
void TodoUi_Refresh(void);
void TodoUi_ShowPage(todo_page_type_t page, const char *list_id);
todo_page_type_t TodoUi_GetCurrentPage(void);

#ifdef __cplusplus
}
#endif

#endif
```

---

### Step 3: 创建 todo_ui.cpp 实现

```cpp
#include "todo_ui.h"
#include "todo_manager.h"
#include <cstring>
#include <cstdio>
#include <lvgl.h>

static lv_obj_t *s_parent = NULL;
static lv_obj_t *s_lists_page = NULL;
static lv_obj_t *s_tasks_page = NULL;
static char s_current_list_id[8] = {0};
static todo_page_type_t s_current_page = TODO_PAGE_LISTS;

// 状态栏
static lv_obj_t *s_status_bar = NULL;
static lv_obj_t *s_lbl_time = NULL;
static lv_obj_t *s_lbl_battery = NULL;
static lv_obj_t *s_lbl_wifi = NULL;

// 清单页组件
static lv_obj_t *s_list_container = NULL;

// 任务页组件
static lv_obj_t *s_task_container = NULL;
static lv_obj_t *s_task_title = NULL;

static void create_status_bar(lv_obj_t *parent)
{
    s_status_bar = lv_obj_create(parent);
    lv_obj_set_size(s_status_bar, 200, 20);
    lv_obj_set_pos(s_status_bar, 0, 0);
    lv_obj_set_style_bg_color(s_status_bar, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_status_bar, 0, 0);
    lv_obj_set_style_pad_all(s_status_bar, 2, 0);

    s_lbl_time = lv_label_create(s_status_bar);
    lv_obj_set_pos(s_lbl_time, 4, 2);
    lv_label_set_text(s_lbl_time, "00:00");
    lv_obj_set_style_text_font(s_lbl_time, &lv_font_montserrat_14, 0);

    s_lbl_battery = lv_label_create(s_status_bar);
    lv_obj_set_pos(s_lbl_battery, 160, 2);
    lv_label_set_text(s_lbl_battery, LV_SYMBOL_BATTERY_FULL);
    lv_obj_set_style_text_font(s_lbl_battery, &lv_font_montserrat_14, 0);

    s_lbl_wifi = lv_label_create(s_status_bar);
    lv_obj_set_pos(s_lbl_wifi, 180, 2);
    lv_label_set_text(s_lbl_wifi, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(s_lbl_wifi, &lv_font_montserrat_14, 0);
}

static void create_lists_page(lv_obj_t *parent)
{
    s_lists_page = lv_obj_create(parent);
    lv_obj_set_size(s_lists_page, 200, 180);
    lv_obj_set_pos(s_lists_page, 0, 20);
    lv_obj_set_style_bg_color(s_lists_page, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_lists_page, 0, 0);

    lv_obj_t *title = lv_label_create(s_lists_page);
    lv_obj_set_pos(title, 10, 10);
    lv_label_set_text(title, "My Lists");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_decor(title, LV_TEXT_DECOR_UNDERLINE, 0);

    s_list_container = lv_obj_create(s_lists_page);
    lv_obj_set_size(s_list_container, 180, 140);
    lv_obj_set_pos(s_list_container, 10, 30);
    lv_obj_set_style_bg_opa(s_list_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list_container, 0, 0);
    lv_obj_set_flex_flow(s_list_container, LV_FLEX_FLOW_COLUMN);
}

static void create_tasks_page(lv_obj_t *parent)
{
    s_tasks_page = lv_obj_create(parent);
    lv_obj_set_size(s_tasks_page, 200, 180);
    lv_obj_set_pos(s_tasks_page, 0, 20);
    lv_obj_set_style_bg_color(s_tasks_page, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_tasks_page, 0, 0);

    s_task_title = lv_label_create(s_tasks_page);
    lv_obj_set_pos(s_task_title, 10, 10);
    lv_label_set_text(s_task_title, "Tasks");
    lv_obj_set_style_text_font(s_task_title, &lv_font_montserrat_14, 0);

    s_task_container = lv_obj_create(s_tasks_page);
    lv_obj_set_size(s_task_container, 180, 140);
    lv_obj_set_pos(s_task_container, 10, 30);
    lv_obj_set_style_bg_opa(s_task_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_task_container, 0, 0);
    lv_obj_set_flex_flow(s_task_container, LV_FLEX_FLOW_COLUMN);
}

static void list_click_cb(lv_event_t *e)
{
    const char *id = (const char *)lv_event_get_user_data(e);
    TodoUi_ShowPage(TODO_PAGE_TASKS, id);
}

static void task_check_cb(lv_event_t *e)
{
    const char *task_id = (const char *)lv_event_get_user_data(e);
    lv_obj_t *cb = lv_event_get_target(e);
    bool done = lv_obj_has_state(cb, LV_STATE_CHECKED);
    TodoMgr_UpdateTask(task_id, &done, NULL, NULL);
}

static void refresh_lists_page(void)
{
    // 清空
    lv_obj_clean(s_list_container);

    todo_list_t lists[TODO_MAX_LISTS];
    uint8_t count = 0;
    TodoMgr_GetLists(lists, &count);

    for (int i = 0; i < count; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s %s  %d/%d",
                 lists[i].icon, lists[i].name,
                 lists[i].task_count > 0 ? lists[i].task_count - 0 : 0,
                 lists[i].task_count);

        lv_obj_t *btn = lv_btn_create(s_list_container);
        lv_obj_set_size(btn, 170, 30);
        lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_add_event_cb(btn, list_click_cb, LV_EVENT_CLICKED, (void *)lists[i].id);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
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

esp_err_t TodoUi_Init(lv_obj_t *parent)
{
    s_parent = parent;

    create_status_bar(parent);
    create_lists_page(parent);
    create_tasks_page(parent);

    // 初始显示清单页
    lv_obj_clear_flag(s_lists_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_tasks_page, LV_OBJ_FLAG_HIDDEN);

    refresh_lists_page();

    return ESP_OK;
}

void TodoUi_Refresh(void)
{
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
        }
        refresh_tasks_page();
    }
}

todo_page_type_t TodoUi_GetCurrentPage(void)
{
    return s_current_page;
}
```

---

### Step 4: 验证编译

```bash
cd E:/aiot
idf.py build
```

**预期:** 编译通过

---

### Step 5: 提交

```bash
git add components/todo_ui/
git commit -m "feat: add todo_ui LVGL component for e-ink display"
```
