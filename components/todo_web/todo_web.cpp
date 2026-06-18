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

    int total = 0;
    while (total < req->content_len) {
        int ret = httpd_req_recv(req, buf + total, req->content_len - total);
        if (ret <= 0) {
            free(buf);
            return NULL;
        }
        total += ret;
    }
    buf[total] = '\0';

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

// 嵌入式网页数据
#include "web_page.h"

// GET /
static esp_err_t handle_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_send(req, (const char *)web_page_gz, web_page_gz_len);
    return ESP_OK;
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
static esp_err_t handle_get_tasks(httpd_req_t *req, const char *list_id)
{
    todo_task_t tasks[TODO_MAX_TASKS];
    uint16_t count = 0;
    esp_err_t err = TodoMgr_GetTasks(list_id, tasks, &count);

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
static esp_err_t handle_create_task(httpd_req_t *req, const char *list_id)
{
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
    esp_err_t err = TodoMgr_CreateTask(list_id, jtext->valuestring, priority, due, task_id);
    cJSON_Delete(body);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CreateTask failed: %s", esp_err_to_name(err));
        return send_error(req, "Create failed", 500);
    }

    ESP_LOGI(TAG, "Task created: %s in %s", task_id, list_id);

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

/* ── Main dispatcher: single catch-all for all API routes ── */
static esp_err_t api_dispatcher(httpd_req_t *req)
{
    const char *uri = req->uri;
    int method = req->method;

    ESP_LOGI(TAG, "API: %s %s len=%d", (method == HTTP_GET) ? "GET" :
             (method == HTTP_POST) ? "POST" :
             (method == HTTP_PUT) ? "PUT" :
             (method == HTTP_DELETE) ? "DELETE" : "?",
             uri, req->content_len);

    // OPTIONS — CORS preflight
    if (method == HTTP_GET && strcmp(uri, "/api/lists") == 0) {
        return handle_get_lists(req);
    }

    if (method == HTTP_POST && strcmp(uri, "/api/lists") == 0) {
        return handle_create_list(req);
    }

    // /api/lists/{id} — no trailing path
    if (strncmp(uri, "/api/lists/", 11) == 0) {
        const char *id_start = uri + 11;
        const char *slash = strchr(id_start, '/');

        if (!slash) {
            // /api/lists/l_001 — update or delete
            if (method == HTTP_PUT) return handle_update_list(req);
            if (method == HTTP_DELETE) return handle_delete_list(req);
        } else if (strncmp(slash, "/tasks", 6) == 0 && *(slash + 6) == '\0') {
            // /api/lists/l_001/tasks
            char list_id[TODO_ID_LEN];
            int len = slash - id_start;
            if (len <= 0 || len >= TODO_ID_LEN) return send_error(req, "Invalid ID", 400);
            strncpy(list_id, id_start, len);
            list_id[len] = '\0';

            if (method == HTTP_GET) return handle_get_tasks(req, list_id);
            if (method == HTTP_POST) return handle_create_task(req, list_id);
        }
    }

    if (strncmp(uri, "/api/tasks/", 11) == 0) {
        if (method == HTTP_PUT) return handle_update_task(req);
        if (method == HTTP_DELETE) return handle_delete_task(req);
    }

    ESP_LOGW(TAG, "No route for %s %s", (method == HTTP_GET) ? "GET" :
             (method == HTTP_POST) ? "POST" :
             (method == HTTP_PUT) ? "PUT" :
             (method == HTTP_DELETE) ? "DELETE" : "?", uri);
    return send_error(req, "Not found", 404);
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
    config.max_uri_handlers = 4;
    config.stack_size = 8192;
    config.uri_match_fn = httpd_uri_match_wildcard;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return err;
    }

    // Root page
    httpd_uri_t uri_root = { .uri = "/", .method = HTTP_GET, .handler = handle_root };
    httpd_register_uri_handler(s_server, &uri_root);

    // Catch-all API dispatcher for GET/POST/PUT/DELETE on /api/*
    httpd_uri_t uri_api = { .uri = "/api/*", .method = HTTP_GET, .handler = api_dispatcher };
    httpd_register_uri_handler(s_server, &uri_api);

    httpd_uri_t uri_api_post = { .uri = "/api/*", .method = HTTP_POST, .handler = api_dispatcher };
    httpd_register_uri_handler(s_server, &uri_api_post);

    httpd_uri_t uri_api_put = { .uri = "/api/*", .method = HTTP_PUT, .handler = api_dispatcher };
    httpd_register_uri_handler(s_server, &uri_api_put);

    httpd_uri_t uri_api_delete = { .uri = "/api/*", .method = HTTP_DELETE, .handler = api_dispatcher };
    httpd_register_uri_handler(s_server, &uri_api_delete);

    ESP_LOGI(TAG, "HTTP server started (catch-all /api/* dispatcher)");
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
