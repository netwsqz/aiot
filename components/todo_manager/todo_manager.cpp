#include "todo_manager.h"
#include <cstring>
#include <cstdio>
#include <nvs_flash.h>
#include <nvs.h>
#include <cJSON.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static const char *TAG = "todo_mgr";
static const char *NVS_NAMESPACE = "todo";
static const char *LISTS_KEY = "lists";
static const int TASKS_PER_CHUNK = 12;

// 内存缓存
static todo_list_t s_lists[TODO_MAX_LISTS];
static uint8_t s_list_count = 0;
static todo_task_t s_tasks[TODO_MAX_LISTS][TODO_MAX_TASKS];
static uint16_t s_task_counts[TODO_MAX_LISTS] = {0};

static SemaphoreHandle_t s_mutex = NULL;
static todo_change_cb_t s_change_cb = NULL;

// NVS handle
static nvs_handle_t s_nvs = 0;

// 内部函数声明
static esp_err_t load_lists_from_nvs(void);
static esp_err_t save_lists_to_nvs(void);
static esp_err_t load_tasks_from_nvs(uint8_t list_idx);
static esp_err_t save_tasks_to_nvs(uint8_t list_idx);
static int find_list_idx(const char *id);
static int find_task_list_idx(const char *task_id, int *task_idx_out);
static void notify_change(void);

esp_err_t TodoMgr_Init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_FAIL;
    }

    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(err));
        return err;
    }

    err = load_lists_from_nvs();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No existing lists, starting fresh");
        s_list_count = 0;
    }

    ESP_LOGI(TAG, "Init done, %d lists loaded", s_list_count);
    return ESP_OK;
}

esp_err_t TodoMgr_GetLists(todo_list_t *lists, uint8_t *count)
{
    if (!lists || !count) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *count = s_list_count;
    memcpy(lists, s_lists, s_list_count * sizeof(todo_list_t));
    xSemaphoreGive(s_mutex);

    return ESP_OK;
}

esp_err_t TodoMgr_CreateList(const char *name, const char *icon, char *out_id)
{
    if (!name || !out_id) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (s_list_count >= TODO_MAX_LISTS) {
        xSemaphoreGive(s_mutex);
        ESP_LOGW(TAG, "Max lists reached");
        return ESP_ERR_NO_MEM;
    }

    // 生成 ID: l_001, l_002, ...
    int idx = s_list_count;
    snprintf(s_lists[idx].id, TODO_ID_LEN, "l_%03d", idx + 1);
    strncpy(s_lists[idx].name, name, TODO_NAME_LEN - 1);
    s_lists[idx].name[TODO_NAME_LEN - 1] = '\0';

    if (icon) {
        strncpy(s_lists[idx].icon, icon, TODO_ICON_LEN - 1);
        s_lists[idx].icon[TODO_ICON_LEN - 1] = '\0';
    } else {
        strcpy(s_lists[idx].icon, "[]");
    }

    s_lists[idx].order = idx;
    s_lists[idx].task_count = 0;
    s_task_counts[idx] = 0;

    s_list_count++;

    esp_err_t err = save_lists_to_nvs();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Save lists failed");
        s_list_count--;
        xSemaphoreGive(s_mutex);
        return err;
    }

    strcpy(out_id, s_lists[idx].id);
    xSemaphoreGive(s_mutex);

    notify_change();
    ESP_LOGI(TAG, "Created list: %s (%s)", out_id, name);
    return ESP_OK;
}

esp_err_t TodoMgr_UpdateList(const char *id, const char *name, const char *icon, const uint8_t *order)
{
    if (!id) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    int idx = find_list_idx(id);
    if (idx < 0) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    if (name) {
        strncpy(s_lists[idx].name, name, TODO_NAME_LEN - 1);
        s_lists[idx].name[TODO_NAME_LEN - 1] = '\0';
    }
    if (icon) {
        strncpy(s_lists[idx].icon, icon, TODO_ICON_LEN - 1);
        s_lists[idx].icon[TODO_ICON_LEN - 1] = '\0';
    }
    if (order != NULL) {
        s_lists[idx].order = *order;
    }

    esp_err_t err = save_lists_to_nvs();
    xSemaphoreGive(s_mutex);

    if (err == ESP_OK) notify_change();
    return err;
}

esp_err_t TodoMgr_DeleteList(const char *id, uint16_t *deleted_tasks)
{
    if (!id) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    int idx = find_list_idx(id);
    if (idx < 0) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    if (deleted_tasks) *deleted_tasks = s_task_counts[idx];

    // 删除 NVS 中的任务块
    char key[24];
    snprintf(key, sizeof(key), "%s_count", s_lists[idx].id);
    nvs_erase_key(s_nvs, key);

    int chunks = (s_task_counts[idx] + TASKS_PER_CHUNK - 1) / TASKS_PER_CHUNK;
    for (int i = 0; i < chunks; i++) {
        snprintf(key, sizeof(key), "%s_%d", s_lists[idx].id, i);
        nvs_erase_key(s_nvs, key);
    }

    // 移动后续列表
    for (int i = idx; i < s_list_count - 1; i++) {
        s_lists[i] = s_lists[i + 1];
        s_task_counts[i] = s_task_counts[i + 1];
        memcpy(s_tasks[i], s_tasks[i + 1], sizeof(s_tasks[0]));
    }
    s_list_count--;

    esp_err_t err = save_lists_to_nvs();
    nvs_commit(s_nvs);
    xSemaphoreGive(s_mutex);

    if (err == ESP_OK) notify_change();
    ESP_LOGI(TAG, "Deleted list: %s (%d tasks)", id, deleted_tasks ? *deleted_tasks : 0);
    return err;
}

esp_err_t TodoMgr_GetTasks(const char *list_id, todo_task_t *tasks, uint16_t *count)
{
    if (!list_id || !tasks || !count) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    int idx = find_list_idx(list_id);
    if (idx < 0) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    *count = s_task_counts[idx];
    memcpy(tasks, s_tasks[idx], s_task_counts[idx] * sizeof(todo_task_t));
    xSemaphoreGive(s_mutex);

    return ESP_OK;
}

esp_err_t TodoMgr_CreateTask(const char *list_id, const char *text, uint8_t priority, const char *due, char *out_id)
{
    if (!list_id || !text || !out_id) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    int idx = find_list_idx(list_id);
    if (idx < 0) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    if (s_task_counts[idx] >= TODO_MAX_TASKS) {
        xSemaphoreGive(s_mutex);
        ESP_LOGW(TAG, "Max tasks reached for list %s", list_id);
        return ESP_ERR_NO_MEM;
    }

    int task_idx = s_task_counts[idx];
    todo_task_t *task = &s_tasks[idx][task_idx];

    // 生成 ID: t_xxxx (时间戳低4位)
    uint32_t ts = (uint32_t)(esp_timer_get_time() / 1000000);
    snprintf(task->id, TODO_ID_LEN, "t_%04u", (unsigned int)(ts % 10000));
    strncpy(task->text, text, TODO_TEXT_LEN - 1);
    task->text[TODO_TEXT_LEN - 1] = '\0';
    task->done = false;
    task->priority = priority;
    task->created = ts;

    if (due) {
        strncpy(task->due, due, TODO_DATE_LEN - 1);
        task->due[TODO_DATE_LEN - 1] = '\0';
    } else {
        task->due[0] = '\0';
    }

    s_task_counts[idx]++;
    s_lists[idx].task_count = s_task_counts[idx];

    esp_err_t err = save_tasks_to_nvs(idx);
    if (err != ESP_OK) {
        s_task_counts[idx]--;
        s_lists[idx].task_count = s_task_counts[idx];
        xSemaphoreGive(s_mutex);
        return err;
    }

    save_lists_to_nvs();
    strcpy(out_id, task->id);
    xSemaphoreGive(s_mutex);

    notify_change();
    ESP_LOGI(TAG, "Created task: %s in %s", out_id, list_id);
    return ESP_OK;
}

esp_err_t TodoMgr_UpdateTask(const char *task_id, const bool *done, const uint8_t *priority, const char *due)
{
    if (!task_id) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    int list_idx, task_idx;
    list_idx = find_task_list_idx(task_id, &task_idx);
    if (list_idx < 0) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    todo_task_t *task = &s_tasks[list_idx][task_idx];
    if (done) task->done = *done;
    if (priority != NULL) task->priority = *priority;
    if (due) {
        strncpy(task->due, due, TODO_DATE_LEN - 1);
        task->due[TODO_DATE_LEN - 1] = '\0';
    }

    esp_err_t err = save_tasks_to_nvs(list_idx);
    xSemaphoreGive(s_mutex);

    if (err == ESP_OK) notify_change();
    return err;
}

esp_err_t TodoMgr_DeleteTask(const char *task_id)
{
    if (!task_id) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    int list_idx, task_idx;
    list_idx = find_task_list_idx(task_id, &task_idx);
    if (list_idx < 0) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    // 移动后续任务
    for (int i = task_idx; i < s_task_counts[list_idx] - 1; i++) {
        s_tasks[list_idx][i] = s_tasks[list_idx][i + 1];
    }
    s_task_counts[list_idx]--;
    s_lists[list_idx].task_count = s_task_counts[list_idx];

    esp_err_t err = save_tasks_to_nvs(list_idx);
    save_lists_to_nvs();
    xSemaphoreGive(s_mutex);

    if (err == ESP_OK) notify_change();
    ESP_LOGI(TAG, "Deleted task: %s", task_id);
    return err;
}

void TodoMgr_SetChangeCallback(todo_change_cb_t cb)
{
    s_change_cb = cb;
}

// 内部函数实现

static esp_err_t load_lists_from_nvs(void)
{
    size_t required_size = 0;
    esp_err_t err = nvs_get_blob(s_nvs, LISTS_KEY, NULL, &required_size);
    if (err != ESP_OK || required_size == 0) return ESP_ERR_NOT_FOUND;

    char *buf = (char *)malloc(required_size);
    if (!buf) return ESP_ERR_NO_MEM;

    err = nvs_get_blob(s_nvs, LISTS_KEY, buf, &required_size);
    if (err != ESP_OK) {
        free(buf);
        return err;
    }

    cJSON *json = cJSON_Parse(buf);
    free(buf);
    if (!json || !cJSON_IsArray(json)) {
        cJSON_Delete(json);
        return ESP_ERR_INVALID_ARG;
    }

    s_list_count = cJSON_GetArraySize(json);
    if (s_list_count > TODO_MAX_LISTS) s_list_count = TODO_MAX_LISTS;

    for (int i = 0; i < s_list_count; i++) {
        cJSON *item = cJSON_GetArrayItem(json, i);
        cJSON *jid = cJSON_GetObjectItem(item, "id");
        cJSON *jname = cJSON_GetObjectItem(item, "name");
        cJSON *jicon = cJSON_GetObjectItem(item, "icon");
        cJSON *jorder = cJSON_GetObjectItem(item, "order");

        if (jid && jid->valuestring)
            strncpy(s_lists[i].id, jid->valuestring, TODO_ID_LEN - 1);
        if (jname && jname->valuestring)
            strncpy(s_lists[i].name, jname->valuestring, TODO_NAME_LEN - 1);
        if (jicon && jicon->valuestring)
            strncpy(s_lists[i].icon, jicon->valuestring, TODO_ICON_LEN - 1);
        if (jorder)
            s_lists[i].order = (uint8_t)jorder->valueint;

        s_lists[i].task_count = 0;
        load_tasks_from_nvs(i);
    }

    cJSON_Delete(json);
    return ESP_OK;
}

static esp_err_t save_lists_to_nvs(void)
{
    cJSON *json = cJSON_CreateArray();
    if (!json) return ESP_ERR_NO_MEM;

    for (int i = 0; i < s_list_count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", s_lists[i].id);
        cJSON_AddStringToObject(item, "name", s_lists[i].name);
        cJSON_AddStringToObject(item, "icon", s_lists[i].icon);
        cJSON_AddNumberToObject(item, "order", s_lists[i].order);
        cJSON_AddItemToArray(json, item);
    }

    char *str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!str) return ESP_ERR_NO_MEM;

    esp_err_t err = nvs_set_blob(s_nvs, LISTS_KEY, str, strlen(str));
    free(str);

    if (err == ESP_OK) err = nvs_commit(s_nvs);
    return err;
}

static esp_err_t load_tasks_from_nvs(uint8_t list_idx)
{
    char count_key[16];
    snprintf(count_key, sizeof(count_key), "%s_count", s_lists[list_idx].id);

    uint16_t count = 0;
    esp_err_t err = nvs_get_u16(s_nvs, count_key, &count);
    if (err != ESP_OK || count == 0) return ESP_OK;

    if (count > TODO_MAX_TASKS) count = TODO_MAX_TASKS;
    s_task_counts[list_idx] = count;
    s_lists[list_idx].task_count = count;

    int chunks = (count + TASKS_PER_CHUNK - 1) / TASKS_PER_CHUNK;
    int loaded = 0;

    for (int c = 0; c < chunks && loaded < count; c++) {
        char key[24];
        snprintf(key, sizeof(key), "%s_%d", s_lists[list_idx].id, c);

        size_t len = 0;
        err = nvs_get_blob(s_nvs, key, NULL, &len);
        if (err != ESP_OK || len == 0) continue;

        char *buf = (char *)malloc(len);
        if (!buf) continue;

        err = nvs_get_blob(s_nvs, key, buf, &len);
        if (err != ESP_OK) {
            free(buf);
            continue;
        }

        cJSON *arr = cJSON_Parse(buf);
        free(buf);
        if (!arr || !cJSON_IsArray(arr)) continue;

        int arr_size = cJSON_GetArraySize(arr);
        for (int i = 0; i < arr_size && loaded < count; i++) {
            cJSON *item = cJSON_GetArrayItem(arr, i);
            todo_task_t *t = &s_tasks[list_idx][loaded];

            cJSON *jid = cJSON_GetObjectItem(item, "id");
            cJSON *jtext = cJSON_GetObjectItem(item, "text");
            cJSON *jdone = cJSON_GetObjectItem(item, "done");
            cJSON *jpri = cJSON_GetObjectItem(item, "priority");
            cJSON *jdue = cJSON_GetObjectItem(item, "due");
            cJSON *jcreated = cJSON_GetObjectItem(item, "created");

            if (jid) strncpy(t->id, jid->valuestring, TODO_ID_LEN - 1);
            if (jtext) strncpy(t->text, jtext->valuestring, TODO_TEXT_LEN - 1);
            if (jdone) t->done = cJSON_IsTrue(jdone);
            if (jpri) t->priority = (uint8_t)jpri->valueint;
            if (jdue) strncpy(t->due, jdue->valuestring, TODO_DATE_LEN - 1);
            if (jcreated) t->created = (uint32_t)jcreated->valueint;

            loaded++;
        }
        cJSON_Delete(arr);
    }

    return ESP_OK;
}

static esp_err_t save_tasks_to_nvs(uint8_t list_idx)
{
    uint16_t count = s_task_counts[list_idx];
    char count_key[16];
    snprintf(count_key, sizeof(count_key), "%s_count", s_lists[list_idx].id);

    nvs_set_u16(s_nvs, count_key, count);

    int chunks = (count + TASKS_PER_CHUNK - 1) / TASKS_PER_CHUNK;
    int saved = 0;

    for (int c = 0; c < chunks; c++) {
        cJSON *arr = cJSON_CreateArray();
        int end = saved + TASKS_PER_CHUNK;
        if (end > count) end = count;

        for (int i = saved; i < end; i++) {
            todo_task_t *t = &s_tasks[list_idx][i];
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "id", t->id);
            cJSON_AddStringToObject(item, "text", t->text);
            cJSON_AddBoolToObject(item, "done", t->done);
            cJSON_AddNumberToObject(item, "priority", t->priority);
            cJSON_AddStringToObject(item, "due", t->due);
            cJSON_AddNumberToObject(item, "created", t->created);
            cJSON_AddItemToArray(arr, item);
        }

        char *str = cJSON_PrintUnformatted(arr);
        cJSON_Delete(arr);

        if (str) {
            char key[24];
            snprintf(key, sizeof(key), "%s_%d", s_lists[list_idx].id, c);
            nvs_set_blob(s_nvs, key, str, strlen(str));
            free(str);
        }
        saved = end;
    }

    return nvs_commit(s_nvs);
}

static int find_list_idx(const char *id)
{
    for (int i = 0; i < s_list_count; i++) {
        if (strcmp(s_lists[i].id, id) == 0) return i;
    }
    return -1;
}

static int find_task_list_idx(const char *task_id, int *task_idx_out)
{
    for (int i = 0; i < s_list_count; i++) {
        for (int j = 0; j < s_task_counts[i]; j++) {
            if (strcmp(s_tasks[i][j].id, task_id) == 0) {
                if (task_idx_out) *task_idx_out = j;
                return i;
            }
        }
    }
    return -1;
}

static void notify_change(void)
{
    if (s_change_cb) s_change_cb();
}
