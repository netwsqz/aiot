#ifndef TODO_MANAGER_H
#define TODO_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TODO_MAX_LISTS      10
#define TODO_MAX_TASKS      32
#define TODO_TEXT_LEN       64
#define TODO_ID_LEN         8
#define TODO_NAME_LEN       32
#define TODO_ICON_LEN       8
#define TODO_DATE_LEN       11

typedef struct {
    char id[TODO_ID_LEN];
    char text[TODO_TEXT_LEN];
    bool done;
    uint8_t priority;    // 0=普通, 1=重要, 2=紧急
    char due[TODO_DATE_LEN];
    uint32_t created;
} todo_task_t;

typedef struct {
    char id[TODO_ID_LEN];
    char name[TODO_NAME_LEN];
    char icon[TODO_ICON_LEN];
    uint8_t order;
    uint16_t task_count;
} todo_list_t;

typedef void (*todo_change_cb_t)(void);

esp_err_t TodoMgr_Init(void);

esp_err_t TodoMgr_GetLists(todo_list_t *lists, uint8_t *count);
esp_err_t TodoMgr_CreateList(const char *name, const char *icon, char *out_id);
esp_err_t TodoMgr_UpdateList(const char *id, const char *name, const char *icon, const uint8_t *order);
esp_err_t TodoMgr_DeleteList(const char *id, uint16_t *deleted_tasks);

esp_err_t TodoMgr_GetTasks(const char *list_id, todo_task_t *tasks, uint16_t *count);
esp_err_t TodoMgr_CreateTask(const char *list_id, const char *text, uint8_t priority, const char *due, char *out_id);
esp_err_t TodoMgr_UpdateTask(const char *task_id, const bool *done, const uint8_t *priority, const char *due);
esp_err_t TodoMgr_DeleteTask(const char *task_id);

void TodoMgr_SetChangeCallback(todo_change_cb_t cb);

#ifdef __cplusplus
}
#endif

#endif
