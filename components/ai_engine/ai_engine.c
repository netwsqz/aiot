/**
 * @file ai_engine.c
 * @brief AI voice chat engine — Tick-driven state machine (spec §3.1).
 *
 * State machine driven by AiEngine_Tick() called from data_task (1s period).
 * Tick() is NON-BLOCKING — it only checks flags and timeouts.
 *
 * Heavy work runs in dedicated FreeRTOS tasks:
 *   - Audio streaming task: reads PCM from AudioBsp, base64-encodes, sends WS
 *   - LLM task: blocking HTTP POST to DeepSeek
 *
 * WS events arrive on the ws client task and are stored in volatile
 * buffers protected by a mutex, consumed in Tick().
 *
 * Dependencies:
 *   - audio_bsp: AudioBsp_InitRecord / Read / DeinitRecord
 *   - wifi_bsp:  WifiBsp_IsConnected
 *   - nvs:       NVS namespace "ai"
 *   - esp_websocket_client: Qwen ASR WebSocket
 *   - esp_http_client:      DeepSeek LLM POST
 *   - mbedtls:   base64 encode
 */

#include "ai_engine.h"
#include "audio_bsp.h"
#include "wifi_bsp.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_err.h"
#include "nvs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

/* esp_websocket_client */
#include "esp_websocket_client.h"

/* esp_http_client */
#include "esp_http_client.h"

/* mbedtls base64 */
#include "mbedtls/base64.h"

static const char *TAG = "ai_engine";

/* ================================================================ *
 * Constants (spec §3.1 — timing)                                    *
 * ================================================================ */

#define NVS_NS               "ai"
#define NVS_KEY_ASR          "asr_key"
#define NVS_KEY_LLM          "llm_key"

/* Audio chunk: 100 ms @ 16 kHz mono 16-bit = 3200 bytes */
#define AUDIO_CHUNK_BYTES    3200

/* Streaming state max ticks before auto-stop */
#define STREAMING_MAX_TICKS  15          /* 15 s */
/* Wait for final transcript after session.finish */
#define FINALIZE_TIMEOUT_TICKS 5         /* 5 s */
/* LLM HTTP timeout */
#define LLM_TIMEOUT_TICKS     30         /* 30 s */
/* Display hold before returning to IDLE */
#define DISPLAY_HOLD_TICKS    30         /* 30 s */
/* Total session timeout from StartRecording */
#define SESSION_MAX_TICKS     60         /* 60 s */

/* WS buffer sizes */
#define WS_BUF_SIZE           4096

/* Chat history ring buffer (spec §3.4) */
#define AI_HISTORY_MAX        10
#define AI_USER_TEXT_LEN      256
#define AI_LLM_TEXT_LEN       512

/* Task stack sizes */
#define AUDIO_STREAM_TASK_STACK  8192
#define LLM_TASK_STACK           8192
#define WS_CLIENT_TASK_STACK     8192

/* ================================================================ *
 * Types                                                             *
 * ================================================================ */

typedef struct {
    char user_text[AI_USER_TEXT_LEN];   /* ASR transcript */
    char llm_text[AI_LLM_TEXT_LEN];     /* LLM reply */
} ai_message_t;

/* WS event types produced by the ws client task, consumed in Tick() */
typedef enum {
    WS_EVT_NONE = 0,
    WS_EVT_CONNECTED,
    WS_EVT_DATA,            /* text frame received */
    WS_EVT_DISCONNECTED,
    WS_EVT_ERROR,
} ws_event_type_t;

/* ================================================================ *
 * Static state                                                       *
 * ================================================================ */

/* ── State machine ────────────────────────────────────────────────── */
static ai_state_t    s_state        = AI_STATE_IDLE;
static int           s_state_ticks  = 0;    /* ticks in current state */
static int           s_session_ticks = 0;   /* ticks since StartRecording */
static SemaphoreHandle_t s_state_mtx   = NULL;

/* ── Callbacks (set in Init) ──────────────────────────────────────── */
static ai_state_cb_t      s_cb_state     = NULL;
static ai_transcript_cb_t s_cb_transcript = NULL;
static ai_llm_result_cb_t s_cb_llm       = NULL;

/* ── WebSocket ────────────────────────────────────────────────────── */
static esp_websocket_client_handle_t s_ws      = NULL;
static bool                      s_ws_connected  = false;
static bool                      s_ws_config_sent = false;

/* WS event message — enqueued by ws_event_handler, dequeued by consume_ws_event */
typedef struct {
    ws_event_type_t type;
    int  data_len;
    char data[WS_BUF_SIZE];
} ws_event_msg_t;

typedef struct {
    bool asr_received;
    char transcript[AI_USER_TEXT_LEN];
    bool ws_error;
} ws_consume_result_t;

#define WS_QUEUE_DEPTH 8

static QueueHandle_t s_ws_queue = NULL;

/* ── Audio streaming task ─────────────────────────────────────────── */
static TaskHandle_t   s_audio_task_handle = NULL;
static volatile bool  s_audio_task_running = false;

/* ── LLM task ─────────────────────────────────────────────────────── */
static TaskHandle_t   s_llm_task_handle = NULL;
static volatile bool  s_llm_task_running = false;

/* ── Transcript / result storage ──────────────────────────────────── */
static char             s_asr_transcript[AI_USER_TEXT_LEN];
static bool             s_asr_received   = false;

static char             s_llm_reply[AI_LLM_TEXT_LEN];
static volatile bool    s_llm_received   = false;
static volatile bool    s_llm_started    = false;  /* LLM task has been launched */

/* ── Chat history ring buffer (spec §3.4) ─────────────────────────── */
static ai_message_t     s_history[AI_HISTORY_MAX];
static int              s_history_head   = 0;
static int              s_history_count  = 0;

/* ── API keys ─────────────────────────────────────────────────────── */
static char             s_asr_key[128];
static char             s_llm_key[128];

/* ── Abort flag — set by AiEngine_Abort(), checked by worker tasks ── */
static volatile bool    s_abort_flag     = false;

/* ================================================================ *
 * Base64 helper (mbedtls)                                           *
 * ================================================================ */

static int base64_encode(const uint8_t *input, size_t ilen,
                         char *output, size_t olen)
{
    size_t written = 0;
    int ret = mbedtls_base64_encode((unsigned char *)output, olen,
                                    &written, input, ilen);
    if (ret != 0) return -1;
    output[written] = '\0';
    return (int)written;
}

/* ================================================================ *
 * JSON helpers (no cJSON — strstr-based, scoped to sub-strings)     *
 * Extract value of "key":"value" → caller-provided buf.             *
 * Returns NULL on miss.                                             *
 * ================================================================ */

static const char *json_get_str_in(const char *json, const char *key,
                                   char *buf, size_t buf_size)
{
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":\"", key);
    const char *start = strstr(json, search);
    if (!start) return NULL;
    start += strlen(search);
    const char *end = strchr(start, '"');
    if (!end) return NULL;
    size_t len = (size_t)(end - start);
    if (len >= buf_size) len = buf_size - 1;
    memcpy(buf, start, len);
    buf[len] = '\0';
    return buf;
}

/* ================================================================ *
 * JSON string escaping for LLM request body                         *
 * Escapes " and \ in-place into a separate buffer.                  *
 * Returns number of chars written (excluding NUL).                  *
 * ================================================================ */

static int json_escape(const char *src, char *dst, size_t dst_size)
{
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 2 < dst_size; i++) {
        if (src[i] == '"' || src[i] == '\\') {
            dst[j++] = '\\';
        }
        dst[j++] = src[i];
    }
    if (j < dst_size) dst[j] = '\0';
    else dst[dst_size - 1] = '\0';
    return (int)j;
}

/* ================================================================ *
 * NVS helpers                                                       *
 * ================================================================ */

static esp_err_t nvs_open_ai(nvs_handle_t *h)
{
    return nvs_open(NVS_NS, NVS_READWRITE, h);
}

static esp_err_t load_key_from_nvs(const char *key_name, char *out, size_t out_size)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open_ai(&h);
    if (err != ESP_OK) return err;
    size_t len = out_size;
    err = nvs_get_str(h, key_name, out, &len);
    nvs_close(h);
    return err;
}

static esp_err_t save_key_to_nvs(const char *key_name, const char *value)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open_ai(&h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, key_name, value);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static void load_keys(void)
{
    if (load_key_from_nvs(NVS_KEY_ASR, s_asr_key, sizeof(s_asr_key)) != ESP_OK)
        s_asr_key[0] = '\0';
    if (load_key_from_nvs(NVS_KEY_LLM, s_llm_key, sizeof(s_llm_key)) != ESP_OK)
        s_llm_key[0] = '\0';
    ESP_LOGI(TAG, "ASR key=%s LLM key=%s",
             s_asr_key[0] ? "set" : "unset",
             s_llm_key[0] ? "set" : "unset");
}

/* ================================================================ *
 * Chat history ring buffer (spec §3.4)                              *
 * ================================================================ */

static void history_push(const char *user_text, const char *llm_text)
{
    if (s_history_count < AI_HISTORY_MAX) {
        s_history_count++;
    } else {
        /* Overwrite oldest entry */
        s_history_head = (s_history_head + 1) % AI_HISTORY_MAX;
    }
    int idx = (s_history_head + s_history_count - 1) % AI_HISTORY_MAX;
    strncpy(s_history[idx].user_text, user_text ? user_text : "",
            AI_USER_TEXT_LEN - 1);
    s_history[idx].user_text[AI_USER_TEXT_LEN - 1] = '\0';
    strncpy(s_history[idx].llm_text, llm_text ? llm_text : "",
            AI_LLM_TEXT_LEN - 1);
    s_history[idx].llm_text[AI_LLM_TEXT_LEN - 1] = '\0';
    ESP_LOGI(TAG, "History push: idx=%d count=%d", idx, s_history_count);
}

/* ================================================================ *
 * WebSocket callbacks                                               *
 * Runs on ws client task; enqueues to s_ws_queue                    *
 * ================================================================ */

static void ws_event_handler(void *arg, esp_event_base_t base,
                             int32_t event_id, void *data)
{
    (void)arg;
    (void)base;
    if (!s_ws_queue) return;

    ws_event_msg_t msg = { .type = WS_EVT_NONE, .data_len = 0 };

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WS connected");
        s_ws_connected = true;
        msg.type = WS_EVT_CONNECTED;
        break;

    case WEBSOCKET_EVENT_DATA: {
        esp_websocket_event_data_t *evt = (esp_websocket_event_data_t *)data;
        if (evt->op_code == 1) { /* text frame */
            int copy_len = evt->data_len;
            if (copy_len > (int)sizeof(msg.data) - 1)
                copy_len = (int)sizeof(msg.data) - 1;
            memcpy(msg.data, evt->data_ptr, (size_t)copy_len);
            msg.data[copy_len] = '\0';
            msg.data_len = copy_len;
            msg.type = WS_EVT_DATA;
        }
        break;
    }

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "WS disconnected");
        s_ws_connected = false;
        msg.type = WS_EVT_DISCONNECTED;
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "WS error");
        s_ws_connected = false;
        msg.type = WS_EVT_ERROR;
        break;

    default:
        break;
    }

    if (msg.type != WS_EVT_NONE) {
        /* If queue full, drop oldest to make room */
        if (xQueueSend(s_ws_queue, &msg, 0) != pdTRUE) {
            ws_event_msg_t drop;
            xQueueReceive(s_ws_queue, &drop, 0);
            xQueueSend(s_ws_queue, &msg, 0);
        }
    }
}

/* ================================================================ *
 * WebSocket helpers                                                  *
 * ================================================================ */

static void ws_close(void)
{
    if (s_ws) {
        esp_websocket_client_stop(s_ws);
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
    }
    s_ws_connected = false;
    s_ws_config_sent = false;
}

static esp_err_t ws_connect(void)
{
    if (s_ws) ws_close();

    if (s_asr_key[0] == '\0') {
        ESP_LOGW(TAG, "ASR key not configured");
        return ESP_ERR_INVALID_ARG;
    }

    const char *ws_url =
        "wss://dashscope.aliyuncs.com/api-ws/v1/realtime"
        "?model=qwen3-asr-flash-realtime";

    /* Build auth header — config.headers is a single string with
     * each header line terminated by \r\n. */
    char auth_header[160];
    snprintf(auth_header, sizeof(auth_header),
             "Authorization: Bearer %s\r\n", s_asr_key);

    esp_websocket_client_config_t cfg = {
        .uri          = ws_url,
        .headers      = auth_header,
        .task_stack   = WS_CLIENT_TASK_STACK,
        .buffer_size  = 4096,
    };

    s_ws = esp_websocket_client_init(&cfg);
    if (!s_ws) return ESP_FAIL;

    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY,
                                  ws_event_handler, NULL);

    /* Reset config-sent flag — will be sent on WS_EVT_CONNECTED in Tick */
    s_ws_config_sent = false;

    esp_err_t err = esp_websocket_client_start(s_ws);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WS start failed: %s", esp_err_to_name(err));
        ws_close();
        return err;
    }
    return ESP_OK;
}

/* Send session config once on connect */
static esp_err_t ws_send_session_config(void)
{
    if (!s_ws || !s_ws_connected) return ESP_FAIL;

    const char *config =
        "{"
        "\"type\":\"session.update\","
        "\"session\":{"
          "\"modalities\":[\"audio\"],"
          "\"input_audio_transcription\":{"
            "\"model\":\"qwen3-asr-flash-realtime\""
          "}"
        "}"
        "}";

    int sent = esp_websocket_client_send_text(s_ws, config,
                                              (int)strlen(config), portMAX_DELAY);
    if (sent > 0) {
        s_ws_config_sent = true;
        ESP_LOGI(TAG, "WS session config sent");
        return ESP_OK;
    }
    return ESP_FAIL;
}

/* Stream one audio chunk (100ms PCM → base64 → JSON) */
static esp_err_t ws_send_audio(const uint8_t *pcm, int pcm_len)
{
    if (!s_ws || !s_ws_connected) return ESP_FAIL;

    /* base64: 3200 bytes → ~4270 chars. Use static buffer to avoid stack overflow. */
    static char s_audio_b64[5800];
    static SemaphoreHandle_t s_b64_mtx = NULL;
    if (!s_b64_mtx) s_b64_mtx = xSemaphoreCreateMutex();

    xSemaphoreTake(s_b64_mtx, portMAX_DELAY);

    int b64_len = base64_encode(pcm, (size_t)pcm_len,
                                s_audio_b64, sizeof(s_audio_b64));
    if (b64_len < 0) {
        xSemaphoreGive(s_b64_mtx);
        return ESP_FAIL;
    }

    /* Build JSON in static buffer — avoids 4KB+ on stack */
    static char s_audio_json[6000];
    int n = snprintf(s_audio_json, sizeof(s_audio_json),
                     "{\"type\":\"input_audio_buffer.append\",\"audio\":\"%s\"}",
                     s_audio_b64);
    xSemaphoreGive(s_b64_mtx);

    if (n >= (int)sizeof(s_audio_json)) return ESP_FAIL;

    int sent = esp_websocket_client_send_text(s_ws, s_audio_json, n, portMAX_DELAY);
    return (sent > 0) ? ESP_OK : ESP_FAIL;
}

/*
 * Send session.finish sequence:
 *   input_audio_buffer.commit → conversation.item.create → response.create → session.finish
 */
static esp_err_t ws_send_finish(void)
{
    if (!s_ws || !s_ws_connected) return ESP_FAIL;

    static const char commit[] =
        "{\"type\":\"input_audio_buffer.commit\"}";
    esp_websocket_client_send_text(s_ws, commit, (int)sizeof(commit) - 1, portMAX_DELAY);

    static const char item_create[] =
        "{\"type\":\"conversation.item.create\","
        "\"item\":{"
          "\"type\":\"message\","
          "\"role\":\"user\","
          "\"content\":[{\"type\":\"input_audio\"}]"
        "}}";
    esp_websocket_client_send_text(s_ws, item_create, (int)sizeof(item_create) - 1, portMAX_DELAY);

    static const char resp_create[] =
        "{\"type\":\"response.create\"}";
    esp_websocket_client_send_text(s_ws, resp_create, (int)sizeof(resp_create) - 1, portMAX_DELAY);

    static const char finish[] =
        "{\"type\":\"session.finish\"}";
    int sent = esp_websocket_client_send_text(s_ws, finish, (int)sizeof(finish) - 1, portMAX_DELAY);
    return (sent > 0) ? ESP_OK : ESP_FAIL;
}

/* ================================================================ *
 * Audio streaming task — reads PCM, sends via WS                    *
 * Runs independently; exits when s_audio_task_running = false       *
 * ================================================================ */

static void audio_stream_task(void *arg)
{
    (void)arg;
    static uint8_t audio_buf[AUDIO_CHUNK_BYTES];
    int total_sent = 0;

    ESP_LOGI(TAG, "Audio stream task started");
    s_audio_task_running = true;

    while (s_audio_task_running && !s_abort_flag) {
        /* Wait for WS to be connected and config sent */
        if (!s_ws_connected || !s_ws_config_sent) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        int read = AudioBsp_Read(audio_buf, AUDIO_CHUNK_BYTES);
        if (read != AUDIO_CHUNK_BYTES) {
            /* No data or error — small delay to avoid busy loop */
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (ws_send_audio(audio_buf, AUDIO_CHUNK_BYTES) == ESP_OK) {
            total_sent += AUDIO_CHUNK_BYTES;
        } else {
            ESP_LOGW(TAG, "WS send audio failed, stopping stream");
            break;
        }

        /* ~100ms of audio per chunk, pace naturally */
    }

    s_audio_task_running = false;
    ESP_LOGI(TAG, "Audio stream task done, sent %d bytes", total_sent);
    vTaskDelete(NULL);
}

/* ================================================================ *
 * LLM HTTP task — blocking POST to DeepSeek                        *
 * Sets s_llm_received = true when done (success or error).          *
 * ================================================================ */

static char  s_http_resp[AI_LLM_TEXT_LEN];
static int   s_http_resp_len = 0;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        int copy = evt->data_len;
        int space = (int)sizeof(s_http_resp) - 1 - s_http_resp_len;
        if (copy > space) copy = space;
        if (copy > 0) {
            memcpy(s_http_resp + s_http_resp_len, evt->data, (size_t)copy);
            s_http_resp_len += copy;
        }
    }
    return ESP_OK;
}

static void llm_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "LLM task started");
    s_llm_task_running = true;

    if (s_llm_key[0] == '\0') {
        ESP_LOGW(TAG, "LLM key not configured");
        snprintf(s_llm_reply, sizeof(s_llm_reply), "[AI配置错误: 缺少LLM密钥]");
        s_llm_received = true;
        s_llm_task_running = false;
        vTaskDelete(NULL);
        return;
    }

    /* Build messages JSON — system + up to AI_HISTORY_MAX rounds + current user */
    /* Escape strings to avoid breaking JSON */
    char body[4096];
    int pos = snprintf(body, sizeof(body),
        "{"
        "\"model\":\"deepseek-chat\","
        "\"stream\":false,"
        "\"messages\":["
        "{\"role\":\"system\",\"content\":\"你是小智AI助手，请简洁回复，不超过200字。\"}");

    char esc_buf[AI_USER_TEXT_LEN * 2];
    char esc_llm[AI_LLM_TEXT_LEN * 2];

    for (int i = 0; i < s_history_count; i++) {
        int idx = (s_history_head + i) % AI_HISTORY_MAX;
        if (s_history[idx].user_text[0]) {
            json_escape(s_history[idx].user_text, esc_buf, sizeof(esc_buf));
            json_escape(s_history[idx].llm_text, esc_llm, sizeof(esc_llm));
            int n = snprintf(body + pos, sizeof(body) - (size_t)pos,
                ",{\"role\":\"user\",\"content\":\"%s\"}"
                ",{\"role\":\"assistant\",\"content\":\"%s\"}",
                esc_buf, esc_llm);
            if (pos + n < (int)sizeof(body)) pos += n;
            else break;  /* body full, stop adding history */
        }
    }

    /* Append current user message */
    json_escape(s_asr_transcript, esc_buf, sizeof(esc_buf));
    int n = snprintf(body + pos, sizeof(body) - (size_t)pos,
        ",{\"role\":\"user\",\"content\":\"%s\"}", esc_buf);
    if (pos + n < (int)sizeof(body)) pos += n;

    snprintf(body + pos, sizeof(body) - (size_t)pos, "]}");

    ESP_LOGI(TAG, "LLM request: body_len=%d", (int)strlen(body));

    /* Reset response buffer */
    s_http_resp[0] = '\0';
    s_http_resp_len = 0;

    esp_http_client_config_t cfg = {
        .url        = "https://api.deepseek.com/v1/chat/completions",
        .method     = HTTP_METHOD_POST,
        .event_handler = http_event_handler,
        .timeout_ms = 25000,   /* 25s < 30s LLM timeout */
        .buffer_size  = 1024,
        .buffer_size_tx = 512,
    };

    char auth[140];
    snprintf(auth, sizeof(auth), "Bearer %s", s_llm_key);

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        snprintf(s_llm_reply, sizeof(s_llm_reply), "[AI回复失败: HTTP初始化错误]");
        s_llm_received = true;
        s_llm_task_running = false;
        vTaskDelete(NULL);
        return;
    }

    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, (int)strlen(body));

    esp_err_t err = esp_http_client_perform(client);

    if (s_abort_flag) {
        ESP_LOGI(TAG, "LLM task aborted");
        esp_http_client_cleanup(client);
        s_llm_task_running = false;
        vTaskDelete(NULL);
        return;
    }

    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "LLM HTTP status=%d resp_len=%d", status, s_http_resp_len);

        if (status == 200 && s_http_resp_len > 0) {
            s_http_resp[s_http_resp_len] = '\0';

            /* Parse OpenAI-compatible response: choices[0].message.content
             * Scope search to after "choices" to avoid matching wrong "content" */
            char parse_buf[AI_LLM_TEXT_LEN];
            const char *choices = strstr(s_http_resp, "\"choices\"");
            const char *content = NULL;
            if (choices) {
                const char *msg = strstr(choices, "\"message\"");
                if (msg) {
                    content = json_get_str_in(msg, "content", parse_buf, sizeof(parse_buf));
                }
            }
            /* Fallback: try top-level content (shouldn't happen but defensive) */
            if (!content) {
                content = json_get_str_in(s_http_resp, "content", parse_buf, sizeof(parse_buf));
            }

            if (content) {
                strncpy(s_llm_reply, content, AI_LLM_TEXT_LEN - 1);
                s_llm_reply[AI_LLM_TEXT_LEN - 1] = '\0';
                s_llm_received = true;
                ESP_LOGI(TAG, "LLM reply: %s", s_llm_reply);
            } else {
                ESP_LOGW(TAG, "No 'content' in LLM response");
                snprintf(s_llm_reply, sizeof(s_llm_reply),
                         "[AI回复解析失败]");
                s_llm_received = true;
            }
        } else {
            snprintf(s_llm_reply, sizeof(s_llm_reply),
                     "[AI回复失败: HTTP %d]", status);
            s_llm_received = true;
        }
    } else {
        ESP_LOGW(TAG, "LLM perform failed: %s", esp_err_to_name(err));
        snprintf(s_llm_reply, sizeof(s_llm_reply),
                 "[AI回复失败: 网络错误]");
        s_llm_received = true;
    }

    esp_http_client_cleanup(client);
    s_llm_task_running = false;
    ESP_LOGI(TAG, "LLM task done");
    vTaskDelete(NULL);
}

/* ================================================================ *
 * WS event consumer — dequeues from s_ws_queue, returns result struct *
 * ================================================================ */

static ws_consume_result_t consume_ws_event(void)
{
    ws_consume_result_t r = {0};
    if (!s_ws_queue) return r;

    ws_event_msg_t msg;
    while (xQueueReceive(s_ws_queue, &msg, 0) == pdTRUE) {
        switch (msg.type) {
        case WS_EVT_CONNECTED:
            ws_send_session_config();
            break;
        case WS_EVT_DATA:
            /* Final transcription */
            if (strstr(msg.data, "transcription.completed")) {
                json_get_str_in(msg.data, "transcript",
                                r.transcript, sizeof(r.transcript));
                r.asr_received = true;
            }
            /* Partial — debug log only */
            if (strstr(msg.data, "transcription.partial")) {
                char tmp[128];
                if (json_get_str_in(msg.data, "transcript", tmp, sizeof(tmp))) {
                    ESP_LOGD(TAG, "ASR partial: %s", tmp);
                }
            }
            break;
        case WS_EVT_DISCONNECTED:
        case WS_EVT_ERROR:
            r.ws_error = true;
            break;
        default:
            break;
        }
    }
    return r;
}

/* ================================================================ *
 * State transitions                                                  *
 * ================================================================ */

static void set_state(ai_state_t new_state)
{
    if (s_state == new_state) return;
    ai_state_t old = s_state;
    s_state = new_state;
    s_state_ticks = 0;
    ESP_LOGI(TAG, "State: %d -> %d", (int)old, (int)new_state);
    if (s_cb_state) s_cb_state(new_state);
}

/* ================================================================ *
 * Cleanup helper — force back to IDLE                               *
 * ================================================================ */

static void force_cleanup(void)
{
    /* Stop audio streaming task */
    s_audio_task_running = false;
    s_abort_flag = true;
    /* Give tasks a moment to notice abort */
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Stop LLM task if running */
    if (s_llm_task_handle) {
        /* LLM task is self-deleting, just wait briefly */
        vTaskDelay(pdMS_TO_TICKS(50));
        s_llm_task_handle = NULL;
    }

    AudioBsp_DeinitRecord();
    ws_close();

    s_asr_received   = false;
    s_llm_received   = false;
    s_llm_started    = false;
    s_session_ticks  = 0;
    s_abort_flag     = false;
    set_state(AI_STATE_IDLE);
}

/* ================================================================ *
 * AiEngine_Tick — state machine driver (spec §3.1)                 *
 * Called every 1s from data_task. NON-BLOCKING.                     *
 * Caller holds Lvgl_Lock.                                           *
 * ================================================================ */

void AiEngine_Tick(void)
{
    if (!s_state_mtx) return;
    if (xSemaphoreTake(s_state_mtx, 0) != pdTRUE) return;

    s_state_ticks++;
    if (s_state != AI_STATE_IDLE) s_session_ticks++;

    /* ── Consume WS events ─────────────────────── */
    ws_consume_result_t wr = consume_ws_event();
    if (wr.asr_received) {
        strncpy(s_asr_transcript, wr.transcript, AI_USER_TEXT_LEN - 1);
        s_asr_transcript[AI_USER_TEXT_LEN - 1] = '\0';
        s_asr_received = true;
        ESP_LOGI(TAG, "ASR final: %s", s_asr_transcript);
        if (s_cb_transcript) s_cb_transcript(s_asr_transcript, true);
    }
    if (wr.ws_error) {
        s_ws_connected = false;
    }

    /* ── WiFi guard (spec §3.5) ──────────────────────── */
    if (s_state != AI_STATE_IDLE && !WifiBsp_IsConnected()) {
        ESP_LOGW(TAG, "WiFi lost → IDLE");
        force_cleanup();
        xSemaphoreGive(s_state_mtx);
        return;
    }

    /* ── Session total timeout (spec §3.5: ≤60 s) ─────── */
    if (s_session_ticks > SESSION_MAX_TICKS && s_state != AI_STATE_IDLE) {
        ESP_LOGW(TAG, "Session timeout (%d ticks)", s_session_ticks);
        force_cleanup();
        xSemaphoreGive(s_state_mtx);
        return;
    }

    switch (s_state) {

    case AI_STATE_IDLE:
        break;

    /* =========================================================== *
     * STREAMING: audio task is running, auto-stop after 15 s       *
     * =========================================================== */
    case AI_STATE_STREAMING: {
        if (s_state_ticks >= STREAMING_MAX_TICKS) {
            ESP_LOGI(TAG, "Streaming auto-stop at %d ticks", s_state_ticks);
            /* Stop audio task */
            s_audio_task_running = false;
            AudioBsp_DeinitRecord();
            ws_send_finish();
            set_state(AI_STATE_FINALIZING);
            break;
        }

        /* Check if audio task died unexpectedly */
        if (!s_audio_task_running && s_state_ticks > 2) {
            ESP_LOGW(TAG, "Audio task died unexpectedly");
            AudioBsp_DeinitRecord();
            ws_send_finish();
            set_state(AI_STATE_FINALIZING);
        }
        break;
    }

    /* =========================================================== *
     * FINALIZING: wait for ASR transcript or timeout (5 s)         *
     * =========================================================== */
    case AI_STATE_FINALIZING: {
        if (s_asr_received) {
            if (s_asr_transcript[0] == '\0') {
                /* Silence → treat as no input (spec §3.5) */
                ESP_LOGW(TAG, "Empty transcript (silence) → IDLE");
                force_cleanup();
                xSemaphoreGive(s_state_mtx);
                return;
            }
            ESP_LOGI(TAG, "ASR done: \"%s\"", s_asr_transcript);
            set_state(AI_STATE_WAITING_LLM);
            break;
        }
        if (s_state_ticks >= FINALIZE_TIMEOUT_TICKS) {
            ESP_LOGW(TAG, "FINALIZING timeout → IDLE");
            force_cleanup();
            xSemaphoreGive(s_state_mtx);
            return;
        }
        break;
    }

    /* =========================================================== *
     * WAITING_LLM: launch LLM task on tick 1, then poll result.   *
     * =========================================================== */
    case AI_STATE_WAITING_LLM: {
        if (s_llm_received) {
            history_push(s_asr_transcript, s_llm_reply);
            if (s_cb_llm) s_cb_llm(s_llm_reply);
            set_state(AI_STATE_DISPLAYING);
            break;
        }

        /* Launch LLM task on first tick in this state */
        if (!s_llm_started) {
            s_llm_started = true;
            s_llm_received = false;
            BaseType_t ret = xTaskCreate(llm_task, "ai_llm",
                                         LLM_TASK_STACK, NULL, 5,
                                         (TaskHandle_t *)&s_llm_task_handle);
            if (ret != pdPASS) {
                ESP_LOGE(TAG, "Failed to create LLM task");
                snprintf(s_llm_reply, sizeof(s_llm_reply), "[AI回复失败: 内存不足]");
                s_llm_received = true;
            }
        }

        /* Safety timeout */
        if (s_state_ticks >= LLM_TIMEOUT_TICKS && !s_llm_received) {
            ESP_LOGW(TAG, "LLM safety timeout");
            snprintf(s_llm_reply, sizeof(s_llm_reply), "[AI回复超时]");
            s_llm_received = true;
        }

        if (s_llm_received) {
            history_push(s_asr_transcript, s_llm_reply);
            if (s_cb_llm) s_cb_llm(s_llm_reply);
            set_state(AI_STATE_DISPLAYING);
        }
        break;
    }

    /* =========================================================== *
     * DISPLAYING: hold on screen for 30 s                          *
     * =========================================================== */
    case AI_STATE_DISPLAYING: {
        if (s_state_ticks >= DISPLAY_HOLD_TICKS) {
            ESP_LOGI(TAG, "Display done → IDLE");
            force_cleanup();
            xSemaphoreGive(s_state_mtx);
            return;
        }
        break;
    }

    } /* switch */

    xSemaphoreGive(s_state_mtx);
}

/* ================================================================ *
 * Public API                                                        *
 * ================================================================ */

esp_err_t AiEngine_Init(ai_state_cb_t      state_cb,
                        ai_transcript_cb_t transcript_cb,
                        ai_llm_result_cb_t llm_cb)
{
    if (s_state_mtx) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    s_state_mtx = xSemaphoreCreateMutex();
    if (!s_state_mtx) return ESP_ERR_NO_MEM;

    s_ws_queue = xQueueCreate(WS_QUEUE_DEPTH, sizeof(ws_event_msg_t));
    if (!s_ws_queue) {
        vSemaphoreDelete(s_state_mtx);
        s_state_mtx = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_cb_state      = state_cb;
    s_cb_transcript = transcript_cb;
    s_cb_llm        = llm_cb;

    s_state          = AI_STATE_IDLE;
    s_state_ticks    = 0;
    s_session_ticks  = 0;
    s_asr_received   = false;
    s_llm_received   = false;
    s_llm_started    = false;
    s_ws_connected   = false;
    s_ws_config_sent = false;
    s_history_head   = 0;
    s_history_count  = 0;
    s_abort_flag     = false;
    s_audio_task_running = false;
    s_llm_task_running   = false;
    memset(s_history, 0, sizeof(s_history));

    /* Drain any stale queue messages */
    if (s_ws_queue) {
        ws_event_msg_t drop;
        while (xQueueReceive(s_ws_queue, &drop, 0) == pdTRUE) { }
    }

    load_keys();

    ESP_LOGI(TAG, "Init done");
    return ESP_OK;
}

esp_err_t AiEngine_StartRecording(void)
{
    if (!s_state_mtx) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_state_mtx, portMAX_DELAY);

    if (s_state != AI_STATE_IDLE) {
        ESP_LOGW(TAG, "StartRecording: state=%d (not IDLE)", (int)s_state);
        xSemaphoreGive(s_state_mtx);
        return ESP_ERR_INVALID_STATE;
    }

    /* Pre-condition checks (spec §3.5) */
    if (!WifiBsp_IsConnected()) {
        ESP_LOGW(TAG, "StartRecording: WiFi not connected");
        xSemaphoreGive(s_state_mtx);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_asr_key[0] == '\0') {
        ESP_LOGW(TAG, "StartRecording: ASR key missing (configure via /api/aikey)");
        xSemaphoreGive(s_state_mtx);
        return ESP_ERR_INVALID_STATE;
    }

    /* Connect WebSocket → ASR (non-blocking start) */
    s_abort_flag = false;
    esp_err_t err = ws_connect();
    if (err != ESP_OK) {
        xSemaphoreGive(s_state_mtx);
        return err;
    }

    /* Open record device */
    err = AudioBsp_InitRecord();
    if (err != ESP_OK) {
        ws_close();
        xSemaphoreGive(s_state_mtx);
        return err;
    }

    s_asr_received   = false;
    s_llm_received   = false;
    s_llm_started    = false;
    s_session_ticks  = 0;
    set_state(AI_STATE_STREAMING);

    /* Launch audio streaming task — it will wait for WS connected + config sent */
    BaseType_t ret = xTaskCreate(audio_stream_task, "ai_audio",
                                 AUDIO_STREAM_TASK_STACK, NULL, 5,
                                 &s_audio_task_handle);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create audio stream task");
        AudioBsp_DeinitRecord();
        ws_close();
        set_state(AI_STATE_IDLE);
        xSemaphoreGive(s_state_mtx);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Recording started (async)");
    xSemaphoreGive(s_state_mtx);
    return ESP_OK;
}

esp_err_t AiEngine_StopRecording(void)
{
    if (!s_state_mtx) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_state_mtx, portMAX_DELAY);

    if (s_state != AI_STATE_STREAMING) {
        ESP_LOGW(TAG, "StopRecording: state=%d (not STREAMING)", (int)s_state);
        xSemaphoreGive(s_state_mtx);
        return ESP_ERR_INVALID_STATE;
    }

    /* Stop audio task */
    s_audio_task_running = false;
    AudioBsp_DeinitRecord();
    ws_send_finish();
    set_state(AI_STATE_FINALIZING);

    ESP_LOGI(TAG, "Recording stopped, awaiting transcript");
    xSemaphoreGive(s_state_mtx);
    return ESP_OK;
}

void AiEngine_Abort(void)
{
    if (!s_state_mtx) return;

    xSemaphoreTake(s_state_mtx, portMAX_DELAY);

    if (s_state == AI_STATE_IDLE) {
        xSemaphoreGive(s_state_mtx);
        return;
    }

    ESP_LOGI(TAG, "Abort from state %d", (int)s_state);
    force_cleanup();
    xSemaphoreGive(s_state_mtx);
}

ai_state_t AiEngine_GetState(void)
{
    return s_state;
}

bool AiEngine_IsActive(void)
{
    return s_state != AI_STATE_IDLE;
}

esp_err_t AiEngine_SetAsrKey(const char *key)
{
    if (!key) return ESP_ERR_INVALID_ARG;
    esp_err_t err = save_key_to_nvs(NVS_KEY_ASR, key);
    if (err == ESP_OK) {
        xSemaphoreTake(s_state_mtx, portMAX_DELAY);
        strncpy(s_asr_key, key, sizeof(s_asr_key) - 1);
        s_asr_key[sizeof(s_asr_key) - 1] = '\0';
        xSemaphoreGive(s_state_mtx);
        ESP_LOGI(TAG, "ASR key stored");
    }
    return err;
}

esp_err_t AiEngine_SetLlmKey(const char *key)
{
    if (!key) return ESP_ERR_INVALID_ARG;
    esp_err_t err = save_key_to_nvs(NVS_KEY_LLM, key);
    if (err == ESP_OK) {
        xSemaphoreTake(s_state_mtx, portMAX_DELAY);
        strncpy(s_llm_key, key, sizeof(s_llm_key) - 1);
        s_llm_key[sizeof(s_llm_key) - 1] = '\0';
        xSemaphoreGive(s_state_mtx);
        ESP_LOGI(TAG, "LLM key stored");
    }
    return err;
}

void AiEngine_PrepareDeepSleep(void)
{
    ESP_LOGI(TAG, "PrepareDeepSleep, state=%d", (int)s_state);

    if (s_state_mtx) xSemaphoreTake(s_state_mtx, portMAX_DELAY);

    /* Force cleanup regardless of state (spec §10) */
    s_abort_flag = true;
    s_audio_task_running = false;
    vTaskDelay(pdMS_TO_TICKS(100));  /* Let tasks notice abort */

    if (AudioBsp_IsRecording()) AudioBsp_DeinitRecord();
    ws_close();

    s_state          = AI_STATE_IDLE;
    s_state_ticks    = 0;
    s_session_ticks  = 0;
    s_asr_received   = false;
    s_llm_received   = false;
    s_llm_started    = false;
    s_ws_connected   = false;
    s_ws_config_sent = false;
    s_history_head   = 0;
    s_history_count  = 0;
    s_abort_flag     = false;
    s_audio_task_running = false;
    s_llm_task_running   = false;
    memset(s_history, 0, sizeof(s_history));

    if (s_state_mtx) {
        xSemaphoreGive(s_state_mtx);
        vSemaphoreDelete(s_state_mtx);
        s_state_mtx = NULL;
    }
    if (s_ws_queue) {
        vQueueDelete(s_ws_queue);
        s_ws_queue = NULL;
    }

    ESP_LOGI(TAG, "Deep sleep prepare done");
    s_cb_state      = NULL;
    s_cb_transcript = NULL;
    s_cb_llm        = NULL;
}
