## Task 3: todo_web HTTP 服务器

**目标:** REST API 路由 + 嵌入式网页服务

**Files:**
- Create: `components/todo_web/CMakeLists.txt`
- Create: `components/todo_web/todo_web.h`
- Create: `components/todo_web/todo_web.cpp`

**Interfaces:**
- Consumes: `todo_manager.h` API (TodoMgr_GetLists, TodoMgr_CreateList, TodoMgr_UpdateList, TodoMgr_DeleteList, TodoMgr_GetTasks, TodoMgr_CreateTask, TodoMgr_UpdateTask, TodoMgr_DeleteTask)
- Produces: `TodoWeb_Init()`, `TodoWeb_Start()`, `TodoWeb_Stop()`, `TodoWeb_IsRunning()`

---

### Step 1: 创建 CMakeLists.txt

```cmake
idf_component_register(
    SRCS "todo_web.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_http_server todo_manager json)
```

---

### Step 2: 创建 todo_web.h

```c
#ifndef TODO_WEB_H
#define TODO_WEB_H

#include <stdbool.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t TodoWeb_Init(void);
esp_err_t TodoWeb_Start(void);
esp_err_t TodoWeb_Stop(void);
bool TodoWeb_IsRunning(void);

#ifdef __cplusplus
}
#endif

#endif
```

---

### Step 3: 创建 todo_web.cpp 实现

```cpp
#include "todo_web.h"
#include "todo_manager.h"
#include <cstring>
#include <esp_http_server.h>
#include <esp_log.h>
#include <cJSON.h>

static const char *TAG = "todo_web";
static httpd_handle_t s_server = NULL;
static bool s_initialized = false;

// 响应 helper
static esp_err_t send_json(httpd_req_t *req, cJSON *json, int status_code)
{
    char *str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!str) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON error");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_status(req, status_code == 200 ? "200 OK" : "400 Bad Request");
    httpd_resp_send(req, str, strlen(str));
    free(str);
    return ESP_OK;
}

static esp_err_t send_ok(httpd_req_t *req, cJSON *data)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    if (data) cJSON_AddItemToObject(resp, "data", data);
    return send_json(req, resp, 200);
}

static esp_err_t send_error(httpd_req_t *req, const char *msg, int code)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", false);
    cJSON_AddStringToObject(resp, "error", msg);
    return send_json(req, resp, code);
}

// 解析请求体 JSON
static cJSON *parse_body(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 2048) return NULL;

    char *buf = (char *)malloc(req->content_len + 1);
    if (!buf) return NULL;

    int ret = httpd_req_recv(req, buf, req->content_len);
    if (ret <= 0) {
        free(buf);
        return NULL;
    }
    buf[ret] = '\0';

    cJSON *json = cJSON_Parse(buf);
    free(buf);
    return json;
}

// 从 URI 提取 ID: /api/lists/l_001 → "l_001"
static bool extract_id(const char *uri, const char *prefix, char *out, int out_len)
{
    const char *start = strstr(uri, prefix);
    if (!start) return false;
    start += strlen(prefix);

    const char *end = strchr(start, '/');
    if (!end) end = start + strlen(start);

    int len = end - start;
    if (len <= 0 || len >= out_len) return false;

    strncpy(out, start, len);
    out[len] = '\0';
    return true;
}

// GET /api/lists
static esp_err_t handle_get_lists(httpd_req_t *req)
{
    todo_list_t lists[TODO_MAX_LISTS];
    uint8_t count = 0;
    TodoMgr_GetLists(lists, &count);

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", lists[i].id);
        cJSON_AddStringToObject(item, "name", lists[i].name);
        cJSON_AddStringToObject(item, "icon", lists[i].icon);
        cJSON_AddNumberToObject(item, "order", lists[i].order);
        cJSON_AddNumberToObject(item, "task_count", lists[i].task_count);

        // 计算已完成数
        todo_task_t tasks[TODO_MAX_TASKS];
        uint16_t task_count = 0;
        TodoMgr_GetTasks(lists[i].id, tasks, &task_count);
        uint16_t done_count = 0;
        for (int j = 0; j < task_count; j++) {
            if (tasks[j].done) done_count++;
        }
        cJSON_AddNumberToObject(item, "done_count", done_count);

        cJSON_AddItemToArray(arr, item);
    }

    return send_ok(req, arr);
}

// POST /api/lists
static esp_err_t handle_create_list(httpd_req_t *req)
{
    cJSON *body = parse_body(req);
    if (!body) return send_error(req, "Invalid JSON", 400);

    cJSON *jname = cJSON_GetObjectItem(body, "name");
    if (!jname || !jname->valuestring) {
        cJSON_Delete(body);
        return send_error(req, "Missing name", 400);
    }

    cJSON *jicon = cJSON_GetObjectItem(body, "icon");
    const char *icon = (jicon && jicon->valuestring) ? jicon->valuestring : "[]";

    char id[TODO_ID_LEN];
    esp_err_t err = TodoMgr_CreateList(jname->valuestring, icon, id);
    cJSON_Delete(body);

    if (err != ESP_OK) {
        return send_error(req, "Create failed", 500);
    }

    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "id", id);
    return send_ok(req, data);
}

// PUT /api/lists/{id}
static esp_err_t handle_update_list(httpd_req_t *req)
{
    char id[TODO_ID_LEN];
    if (!extract_id(req->uri, "/api/lists/", id, sizeof(id))) {
        return send_error(req, "Invalid ID", 400);
    }

    cJSON *body = parse_body(req);
    if (!body) return send_error(req, "Invalid JSON", 400);

    cJSON *jname = cJSON_GetObjectItem(body, "name");
    cJSON *jicon = cJSON_GetObjectItem(body, "icon");
    cJSON *jorder = cJSON_GetObjectItem(body, "order");

    const char *name = (jname && jname->valuestring) ? jname->valuestring : NULL;
    const char *icon = (jicon && jicon->valuestring) ? jicon->valuestring : NULL;
    uint8_t *order_ptr = NULL;
    uint8_t order_val = 0;
    if (jorder) {
        order_val = (uint8_t)jorder->valueint;
        order_ptr = &order_val;
    }

    esp_err_t err = TodoMgr_UpdateList(id, name, icon, order_ptr);
    cJSON_Delete(body);

    if (err != ESP_OK) return send_error(req, "Update failed", 500);
    return send_ok(req, NULL);
}

// DELETE /api/lists/{id}
static esp_err_t handle_delete_list(httpd_req_t *req)
{
    char id[TODO_ID_LEN];
    if (!extract_id(req->uri, "/api/lists/", id, sizeof(id))) {
        return send_error(req, "Invalid ID", 400);
    }

    uint16_t deleted = 0;
    esp_err_t err = TodoMgr_DeleteList(id, &deleted);

    if (err != ESP_OK) return send_error(req, "Delete failed", 500);

    cJSON *data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "deleted_tasks", deleted);
    return send_ok(req, data);
}

// GET /api/lists/{id}/tasks
static esp_err_t handle_get_tasks(httpd_req_t *req)
{
    char id[TODO_ID_LEN];
    if (!extract_id(req->uri, "/api/lists/", id, sizeof(id))) {
        return send_error(req, "Invalid ID", 400);
    }

    todo_task_t tasks[TODO_MAX_TASKS];
    uint16_t count = 0;
    esp_err_t err = TodoMgr_GetTasks(id, tasks, &count);

    if (err != ESP_OK) return send_error(req, "List not found", 404);

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", tasks[i].id);
        cJSON_AddStringToObject(item, "text", tasks[i].text);
        cJSON_AddBoolToObject(item, "done", tasks[i].done);
        cJSON_AddNumberToObject(item, "priority", tasks[i].priority);
        cJSON_AddStringToObject(item, "due", tasks[i].due);
        cJSON_AddNumberToObject(item, "created", tasks[i].created);
        cJSON_AddItemToArray(arr, item);
    }

    return send_ok(req, arr);
}

// POST /api/lists/{id}/tasks
static esp_err_t handle_create_task(httpd_req_t *req)
{
    char id[TODO_ID_LEN];
    if (!extract_id(req->uri, "/api/lists/", id, sizeof(id))) {
        return send_error(req, "Invalid ID", 400);
    }

    cJSON *body = parse_body(req);
    if (!body) return send_error(req, "Invalid JSON", 400);

    cJSON *jtext = cJSON_GetObjectItem(body, "text");
    if (!jtext || !jtext->valuestring) {
        cJSON_Delete(body);
        return send_error(req, "Missing text", 400);
    }

    cJSON *jpri = cJSON_GetObjectItem(body, "priority");
    cJSON *jdue = cJSON_GetObjectItem(body, "due");

    uint8_t priority = (jpri) ? (uint8_t)jpri->valueint : 0;
    const char *due = (jdue && jdue->valuestring) ? jdue->valuestring : NULL;

    char task_id[TODO_ID_LEN];
    esp_err_t err = TodoMgr_CreateTask(id, jtext->valuestring, priority, due, task_id);
    cJSON_Delete(body);

    if (err != ESP_OK) return send_error(req, "Create failed", 500);

    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "id", task_id);
    return send_ok(req, data);
}

// PUT /api/tasks/{id}
static esp_err_t handle_update_task(httpd_req_t *req)
{
    char id[TODO_ID_LEN];
    if (!extract_id(req->uri, "/api/tasks/", id, sizeof(id))) {
        return send_error(req, "Invalid ID", 400);
    }

    cJSON *body = parse_body(req);
    if (!body) return send_error(req, "Invalid JSON", 400);

    cJSON *jdone = cJSON_GetObjectItem(body, "done");
    cJSON *jpri = cJSON_GetObjectItem(body, "priority");
    cJSON *jdue = cJSON_GetObjectItem(body, "due");

    bool *done_ptr = NULL;
    bool done_val = false;
    if (jdone) {
        done_val = cJSON_IsTrue(jdone);
        done_ptr = &done_val;
    }

    uint8_t *pri_ptr = NULL;
    uint8_t pri_val = 0;
    if (jpri) {
        pri_val = (uint8_t)jpri->valueint;
        pri_ptr = &pri_val;
    }

    const char *due = (jdue && jdue->valuestring) ? jdue->valuestring : NULL;

    esp_err_t err = TodoMgr_UpdateTask(id, done_ptr, pri_ptr, due);
    cJSON_Delete(body);

    if (err != ESP_OK) return send_error(req, "Update failed", 500);
    return send_ok(req, NULL);
}

// DELETE /api/tasks/{id}
static esp_err_t handle_delete_task(httpd_req_t *req)
{
    char id[TODO_ID_LEN];
    if (!extract_id(req->uri, "/api/tasks/", id, sizeof(id))) {
        return send_error(req, "Invalid ID", 400);
    }

    esp_err_t err = TodoMgr_DeleteTask(id);
    if (err != ESP_OK) return send_error(req, "Delete failed", 500);
    return send_ok(req, NULL);
}

// OPTIONS handler (CORS preflight)
static esp_err_t handle_options(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

esp_err_t TodoWeb_Init(void)
{
    s_initialized = true;
    ESP_LOGI(TAG, "Init done");
    return ESP_OK;
}

esp_err_t TodoWeb_Start(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (s_server) return ESP_OK;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 12;
    config.stack_size = 8192;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return err;
    }

    // 注册路由
    httpd_uri_t uri_get_lists = { .uri = "/api/lists", .method = HTTP_GET, .handler = handle_get_lists };
    httpd_uri_t uri_create_list = { .uri = "/api/lists", .method = HTTP_POST, .handler = handle_create_list };
    httpd_uri_t uri_update_list = { .uri = "/api/lists/*", .method = HTTP_PUT, .handler = handle_update_list };
    httpd_uri_t uri_delete_list = { .uri = "/api/lists/*", .method = HTTP_DELETE, .handler = handle_delete_list };
    httpd_uri_t uri_get_tasks = { .uri = "/api/lists/*/tasks", .method = HTTP_GET, .handler = handle_get_tasks };
    httpd_uri_t uri_create_task = { .uri = "/api/lists/*/tasks", .method = HTTP_POST, .handler = handle_create_task };
    httpd_uri_t uri_update_task = { .uri = "/api/tasks/*", .method = HTTP_PUT, .handler = handle_update_task };
    httpd_uri_t uri_delete_task = { .uri = "/api/tasks/*", .method = HTTP_DELETE, .handler = handle_delete_task };
    httpd_uri_t uri_options = { .uri = "/*", .method = HTTP_OPTIONS, .handler = handle_options };

    httpd_register_uri_handler(s_server, &uri_get_lists);
    httpd_register_uri_handler(s_server, &uri_create_list);
    httpd_register_uri_handler(s_server, &uri_update_list);
    httpd_register_uri_handler(s_server, &uri_delete_list);
    httpd_register_uri_handler(s_server, &uri_get_tasks);
    httpd_register_uri_handler(s_server, &uri_create_task);
    httpd_register_uri_handler(s_server, &uri_update_task);
    httpd_register_uri_handler(s_server, &uri_delete_task);
    httpd_register_uri_handler(s_server, &uri_options);

    ESP_LOGI(TAG, "HTTP server started");
    return ESP_OK;
}

esp_err_t TodoWeb_Stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "HTTP server stopped");
    }
    return ESP_OK;
}

bool TodoWeb_IsRunning(void)
{
    return s_server != NULL;
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
git add components/todo_web/
git commit -m "feat: add todo_web HTTP server with REST API"
```
