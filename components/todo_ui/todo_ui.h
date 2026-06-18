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

/* Callback for navigation */
typedef void (*todo_nav_cb_t)(todo_page_type_t page);

esp_err_t TodoUi_Init(void);
void TodoUi_Refresh(void);
void TodoUi_ShowPage(todo_page_type_t page, const char *list_id);
todo_page_type_t TodoUi_GetCurrentPage(void);
lv_obj_t *TodoUi_GetScreen(void);

/* Set callback invoked when user presses back button on lists page */
void TodoUi_SetBackCallback(void (*cb)(void));

#ifdef __cplusplus
}
#endif

#endif
