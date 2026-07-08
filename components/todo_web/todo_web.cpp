#include "todo_web.h"
#include "todo_manager.h"
#include "standby_screen.h"
#include "wifi_bsp.h"
#include "weather_fetch.h"
#include "weather_cache.h"
#include "nvs.h"
#include "nvs_flash.h"
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

    /* Map numeric status codes to HTTP status lines. Earlier code collapsed
     * every non-200 to "400 Bad Request", so 404/500 responses carried the
     * wrong status line and clients couldn't distinguish missing resources
     * from malformed requests. */
    const char *status_line;
    switch (status_code) {
        case 200: status_line = "200 OK"; break;
        case 400: status_line = "400 Bad Request"; break;
        case 404: status_line = "404 Not Found"; break;
        case 500: status_line = "500 Internal Server Error"; break;
        default:  status_line = (status_code >= 500) ? "500 Internal Server Error" : "400 Bad Request"; break;
    }
    httpd_resp_set_status(req, status_line);
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
    /* Boundary validation: TodoMgr truncates with strncpy, but a too-long
     * name would be silently cut (possibly mid-UTF-8). Reject up-front. */
    if (strlen(jname->valuestring) > TODO_NAME_LEN - 1) {
        cJSON_Delete(body);
        return send_error(req, "name too long", 400);
    }

    cJSON *jicon = cJSON_GetObjectItem(body, "icon");
    const char *icon = (jicon && jicon->valuestring) ? jicon->valuestring : "[]";
    if (strlen(icon) > TODO_ICON_LEN - 1) {
        cJSON_Delete(body);
        return send_error(req, "icon too long", 400);
    }

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
    if (name && strlen(name) > TODO_NAME_LEN - 1) {
        cJSON_Delete(body);
        return send_error(req, "name too long", 400);
    }
    if (icon && strlen(icon) > TODO_ICON_LEN - 1) {
        cJSON_Delete(body);
        return send_error(req, "icon too long", 400);
    }
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
    if (strlen(jtext->valuestring) > TODO_TEXT_LEN - 1) {
        cJSON_Delete(body);
        return send_error(req, "text too long", 400);
    }

    cJSON *jpri = cJSON_GetObjectItem(body, "priority");
    cJSON *jdue = cJSON_GetObjectItem(body, "due");

    uint8_t priority = (jpri) ? (uint8_t)jpri->valueint : 0;
    const char *due = (jdue && jdue->valuestring) ? jdue->valuestring : NULL;
    if (due && strlen(due) > TODO_DATE_LEN - 1) {
        cJSON_Delete(body);
        return send_error(req, "due too long", 400);
    }

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
    if (due && strlen(due) > TODO_DATE_LEN - 1) {
        cJSON_Delete(body);
        return send_error(req, "due too long", 400);
    }

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

/* ── Standby image handlers (forward-declared for dispatcher) ── */
static esp_err_t handle_get_standby(httpd_req_t *req);
static esp_err_t handle_upload_standby(httpd_req_t *req);
static esp_err_t handle_delete_standby(httpd_req_t *req);

/* ── WiFi config handlers (forward-declared for dispatcher) ── */
static esp_err_t handle_get_wifi(httpd_req_t *req);
static esp_err_t handle_post_wifi(httpd_req_t *req);

/* ── Weather config handlers (forward-declared for dispatcher) ── */
static esp_err_t handle_get_weather(httpd_req_t *req);
static esp_err_t handle_post_weather(httpd_req_t *req);
static esp_err_t handle_refresh_weather(httpd_req_t *req);

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

    /* ── Standby image API ── */
    if (strcmp(uri, "/api/standby") == 0) {
        if (method == HTTP_GET)  return handle_get_standby(req);
        if (method == HTTP_POST) return handle_upload_standby(req);
        if (method == HTTP_DELETE) return handle_delete_standby(req);
    }

    /* ── WiFi config API ── */
    if (strcmp(uri, "/api/wifi") == 0) {
        if (method == HTTP_GET)  return handle_get_wifi(req);
        if (method == HTTP_POST) return handle_post_wifi(req);
    }

    /* ── Weather config API ── */
    if (strcmp(uri, "/api/weather/config") == 0) {
        if (method == HTTP_GET)  return handle_get_weather(req);
        if (method == HTTP_POST) return handle_post_weather(req);
    }

    /* ── Weather manual refresh ──
     * Asks data_task to fetch immediately (next 1s tick). Returns ok
     * without waiting — caller polls /api/weather/config or watches the
     * screen. Does nothing if WiFi is down or config is incomplete. */
    if (method == HTTP_POST && strcmp(uri, "/api/weather/refresh") == 0) {
        return handle_refresh_weather(req);
    }

    /* OPTIONS — CORS preflight for any /api/ route */
    if (method == HTTP_OPTIONS && strncmp(uri, "/api/", 5) == 0) {
        return handle_options(req);
    }

    ESP_LOGW(TAG, "No route for %s %s", (method == HTTP_GET) ? "GET" :
             (method == HTTP_POST) ? "POST" :
             (method == HTTP_PUT) ? "PUT" :
             (method == HTTP_DELETE) ? "DELETE" : "?", uri);
    return send_error(req, "Not found", 404);
}

/* ── Standby image handlers ─────────────────────────── */

#define BMP_HEADER_SIZE   54
#define BMP_COLOR_TABLE   8   /* 2 entries × 4 bytes */
#define BMP_DATA_OFFSET   (BMP_HEADER_SIZE + BMP_COLOR_TABLE)  /* 62 */
#define BMP_ROW_BYTES     25  /* 200/8 */
#define BMP_ROW_PADDED    28  /* next 4-byte boundary */
#define BMP_PIXEL_SIZE    5000

/* GET /api/standby — return current custom image as BMP, or 404 */
static esp_err_t handle_get_standby(httpd_req_t *req)
{
    FILE *f = fopen("/spiffs/standby.bmp", "rb");
    if (!f) {
        return send_error(req, "No custom standby image", 404);
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    httpd_resp_set_type(req, "image/bmp");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    /* Stream in chunks to avoid large allocation */
    uint8_t chunk[512];
    long remaining = fsize;
    while (remaining > 0) {
        int to_read = (remaining > (long)sizeof(chunk)) ? (int)sizeof(chunk) : (int)remaining;
        int got = fread(chunk, 1, to_read, f);
        if (got <= 0) break;
        httpd_resp_send_chunk(req, (const char *)chunk, got);
        remaining -= got;
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);  /* end chunked response */
    return ESP_OK;
}

/* POST /api/standby — upload raw 1bpp pixel data (5000 bytes) as JSON base64,
 * or as raw binary body. We accept raw binary (application/octet-stream)
 * of exactly 5000 bytes. */
static esp_err_t handle_upload_standby(httpd_req_t *req)
{
    if (req->content_len != BMP_PIXEL_SIZE) {
        ESP_LOGW(TAG, "Standby upload: expected %d bytes, got %d", BMP_PIXEL_SIZE, req->content_len);
        return send_error(req, "Expected 5000 bytes (200x200 1bpp raw)", 400);
    }

    uint8_t *buf = (uint8_t *)malloc(BMP_PIXEL_SIZE);
    if (!buf) return send_error(req, "Out of memory", 500);

    int total = 0;
    while (total < BMP_PIXEL_SIZE) {
        int ret = httpd_req_recv(req, (char *)buf + total, BMP_PIXEL_SIZE - total);
        if (ret <= 0) {
            free(buf);
            return send_error(req, "Receive failed", 500);
        }
        total += ret;
    }

    bool ok = StandbyScreen_SaveBmp(buf, BMP_PIXEL_SIZE);
    free(buf);

    if (!ok) return send_error(req, "Save failed", 500);
    return send_ok(req, NULL);
}

/* DELETE /api/standby — remove custom image */
static esp_err_t handle_delete_standby(httpd_req_t *req)
{
    StandbyScreen_DeleteBmp();
    return send_ok(req, NULL);
}

/* ── WiFi config handlers ─────────────────────────── */

/* GET /api/wifi — return current SSID (no password) and connection status */
static esp_err_t handle_get_wifi(httpd_req_t *req)
{
    char ssid[33] = {0};
    WifiBsp_GetSsid(ssid, sizeof(ssid));

    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "ssid", ssid);
    cJSON_AddBoolToObject(data, "connected", WifiBsp_IsConnected());
    return send_ok(req, data);
}

/* POST /api/wifi — save new credentials and reconnect */
static esp_err_t handle_post_wifi(httpd_req_t *req)
{
    cJSON *body = parse_body(req);
    if (!body) return send_error(req, "Invalid JSON", 400);

    cJSON *jssid = cJSON_GetObjectItem(body, "ssid");
    if (!jssid || !jssid->valuestring || strlen(jssid->valuestring) == 0) {
        cJSON_Delete(body);
        return send_error(req, "Missing SSID", 400);
    }
    /* Validate SSID/password length against wifi_config_t field sizes.
     * wifi_config_t.sta.ssid is 32 bytes total (incl. NUL) → 31 usable chars;
     * .password is 64 bytes total → 63 usable chars. Reject up-front so
     * oversized values aren't stored in NVS and then silently fail to load
     * back into the smaller stack buffers in WifiBsp_Reconnect(). */
    if (strlen(jssid->valuestring) > 31) {
        cJSON_Delete(body);
        return send_error(req, "SSID too long (max 31 chars)", 400);
    }

    cJSON *jpass = cJSON_GetObjectItem(body, "password");
    const char *password = (jpass && jpass->valuestring) ? jpass->valuestring : "";
    if (strlen(password) > 63) {
        cJSON_Delete(body);
        return send_error(req, "Password too long (max 63 chars)", 400);
    }

    /* Snapshot the SSID into a local buffer BEFORE cJSON_Delete(body) frees
     * jssid->valuestring. The earlier code kept a raw pointer into the cJSON
     * tree and used it after free (use-after-free) when building the
     * success response below. wifi_config_t.sta.ssid fits 31 chars + NUL. */
    char echo_ssid[32];
    strncpy(echo_ssid, jssid->valuestring, sizeof(echo_ssid) - 1);
    echo_ssid[sizeof(echo_ssid) - 1] = '\0';

    esp_err_t err = WifiBsp_SetCredentials(jssid->valuestring, password);
    if (err != ESP_OK) {
        cJSON_Delete(body);
        return send_error(req, "Failed to save credentials", 500);
    }

    /* Reconnect with new credentials */
    err = WifiBsp_Reconnect();
    cJSON_Delete(body);

    if (err != ESP_OK) return send_error(req, "Reconnect failed", 500);

    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "ssid", echo_ssid);
    cJSON_AddBoolToObject(data, "reconnecting", true);
    return send_ok(req, data);
}

/* ── Weather config handlers ─────────────────────────── */

/* Read a config field from NVS namespace "weather", falling back to the
 * Kconfig default if not stored. Writes a NUL-terminated copy into out. */
static void read_weather_field(const char *key, const char *kconfig_default,
                               char *out, size_t out_size)
{
    nvs_handle_t h;
    if (nvs_open("weather", NVS_READONLY, &h) == ESP_OK) {
        size_t len = out_size;
        if (nvs_get_str(h, key, out, &len) == ESP_OK) {
            nvs_close(h);
            return;
        }
        nvs_close(h);
    }
    strncpy(out, kconfig_default ? kconfig_default : "", out_size - 1);
    out[out_size - 1] = '\0';
}

/* Mask an API key: keep last 4 chars, prefix with asterisks. */
static void mask_api_key(const char *key, char *out, size_t out_size)
{
    size_t len = key ? strlen(key) : 0;
    if (len == 0) { if (out_size) out[0] = '\0'; return; }
    if (len <= 4) {
        strncpy(out, key, out_size - 1);
        out[out_size - 1] = '\0';
        return;
    }
    size_t stars = len - 4;
    if (stars > out_size - 5) stars = out_size - 5;  /* leave room for last 4 + NUL */
    memset(out, '*', stars);
    strncpy(out + stars, key + len - 4, 4);
    out[stars + 4] = '\0';
}

/* GET /api/weather/config — return masked key + location + city (from NVS). */
static esp_err_t handle_get_weather(httpd_req_t *req)
{
    char api_key[64];
    char location_id[32];
    char city_name[32];
    char base_url[64];
    char masked[40];

    read_weather_field("api_key", CONFIG_WEATHER_API_KEY, api_key, sizeof(api_key));
    read_weather_field("location_id", CONFIG_WEATHER_LOCATION_ID, location_id, sizeof(location_id));
    read_weather_field("city_name", CONFIG_WEATHER_CITY_NAME, city_name, sizeof(city_name));
    read_weather_field("base_url", CONFIG_WEATHER_API_BASE_URL, base_url, sizeof(base_url));

    mask_api_key(api_key, masked, sizeof(masked));

    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "api_key_masked", masked);
    cJSON_AddStringToObject(data, "location_id", location_id);
    cJSON_AddStringToObject(data, "city_name", city_name);
    cJSON_AddStringToObject(data, "base_url", base_url);
    return send_ok(req, data);
}

/* POST /api/weather/config — validate + write to NVS, then clear cache. */
static esp_err_t handle_post_weather(httpd_req_t *req)
{
    cJSON *body = parse_body(req);
    if (!body) return send_error(req, "Invalid JSON", 400);

    cJSON *jkey = cJSON_GetObjectItem(body, "api_key");
    cJSON *jloc = cJSON_GetObjectItem(body, "location_id");
    cJSON *jcity = cJSON_GetObjectItem(body, "city_name");

    if (!cJSON_IsString(jkey) || jkey->valuestring[0] == '\0') {
        cJSON_Delete(body); return send_error(req, "Missing api_key", 400);
    }
    if (!cJSON_IsString(jloc) || jloc->valuestring[0] == '\0') {
        cJSON_Delete(body); return send_error(req, "Missing location_id", 400);
    }
    if (!cJSON_IsString(jcity) || jcity->valuestring[0] == '\0') {
        cJSON_Delete(body); return send_error(req, "Missing city_name", 400);
    }
    if (strlen(jcity->valuestring) > WEATHER_CITY_NAME_LEN - 1) {
        cJSON_Delete(body); return send_error(req, "city_name too long", 400);
    }
    if (strlen(jkey->valuestring) > WEATHER_API_KEY_LEN - 1) {
        cJSON_Delete(body); return send_error(req, "api_key too long", 400);
    }
    if (strlen(jloc->valuestring) > WEATHER_LOCATION_ID_LEN - 1) {
        cJSON_Delete(body); return send_error(req, "location_id too long", 400);
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open("weather", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        cJSON_Delete(body);
        return send_error(req, "NVS open failed", 500);
    }
    nvs_set_str(handle, "api_key", jkey->valuestring);
    nvs_set_str(handle, "location_id", jloc->valuestring);
    nvs_set_str(handle, "city_name", jcity->valuestring);
    /* Optional base_url override (QWeather per-developer API host). If the
     * client omits it we don't touch the stored value, so the field acts as
     * a partial update. WeatherFetch's s_base_url buffer is 64 bytes. */
    cJSON *jbase = cJSON_GetObjectItem(body, "base_url");
    if (cJSON_IsString(jbase) && jbase->valuestring[0] != '\0' &&
        strlen(jbase->valuestring) < 64) {
        nvs_set_str(handle, "base_url", jbase->valuestring);
    }
    nvs_commit(handle);
    nvs_close(handle);

    /* Reload live config so the new key/city/host takes effect immediately —
     * previously WeatherFetch kept stale buffers until reboot. */
    WeatherFetch_ReloadConfig();

    /* Clear stale weather cache so the old city's data is not shown. */
    WeatherCache_Clear();

    ESP_LOGI(TAG, "Weather config updated: city=%s location=%s",
             jcity->valuestring, jloc->valuestring);
    cJSON_Delete(body);
    return send_ok(req, NULL);
}

/* POST /api/weather/refresh — trigger an immediate fetch (async). */
static esp_err_t handle_refresh_weather(httpd_req_t *req)
{
    if (!WifiBsp_IsConnected()) {
        return send_error(req, "WiFi not connected", 400);
    }
    WeatherFetch_RequestRefresh();
    ESP_LOGI(TAG, "Weather refresh requested");
    return send_ok(req, NULL);
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
    config.max_uri_handlers = 8;  /* 6 used (root + GET/POST/PUT/DELETE/OPTIONS
                                   * catch-all); leave headroom for future
                                   * handlers so registration doesn't fail. */
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

    httpd_uri_t uri_api_options = { .uri = "/api/*", .method = HTTP_OPTIONS, .handler = api_dispatcher };
    httpd_register_uri_handler(s_server, &uri_api_options);

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
