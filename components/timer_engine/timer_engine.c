#include "timer_engine.h"
#include <string.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/timers.h>

static const char *TAG = "timer_eng";

#define POMODORO_SLOT       0
#define REMINDER_SLOT_FIRST 1
#define MAX_REMINDERS       3
#define TICK_PERIOD_MS      1000

static timer_instance_t s_slots[TIMER_MAX_SLOTS];
static SemaphoreHandle_t s_mutex = NULL;
static TimerHandle_t     s_tick_timer = NULL;
static volatile timer_fired_cb_t  s_fired_cb = NULL;
static uint8_t           s_next_id = 1;

/* tick 回调里收集到点的计时器，释放锁后再触发回调 */
static timer_instance_t s_fired_buf[TIMER_MAX_SLOTS];

static void tick_cb(TimerHandle_t tmr)
{
    (void)tmr;
    int fired_count = 0;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < TIMER_MAX_SLOTS; i++) {
        if (!s_slots[i].active) continue;
        if (s_slots[i].remaining_sec > 0) s_slots[i].remaining_sec--;
        if (s_slots[i].remaining_sec == 0) {
            s_slots[i].active = false;
            s_fired_buf[fired_count++] = s_slots[i];
        }
    }
    xSemaphoreGive(s_mutex);

    for (int i = 0; i < fired_count; i++) {
        if (s_fired_cb) s_fired_cb(&s_fired_buf[i]);
    }
}

esp_err_t TimerEngine_Init(void)
{
    if (s_mutex) return ESP_OK;  /* 已初始化 */

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_FAIL;

    memset(s_slots, 0, sizeof(s_slots));
    s_next_id = 1;

    s_tick_timer = xTimerCreate("timer_tick", pdMS_TO_TICKS(TICK_PERIOD_MS),
                                pdTRUE, NULL, tick_cb);
    if (!s_tick_timer) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return ESP_FAIL;
    }
    xTimerStart(s_tick_timer, 0);

    ESP_LOGI(TAG, "Init done, tick 1s");
    return ESP_OK;
}

static void fill_slot(int slot, timer_kind_t kind, uint32_t minutes, uint8_t *out_id)
{
    s_slots[slot].id            = s_next_id++;
    if (s_next_id == 0) s_next_id = 1;
    s_slots[slot].kind          = kind;
    s_slots[slot].total_seconds = minutes * 60;
    s_slots[slot].remaining_sec = minutes * 60;
    s_slots[slot].active        = true;
    if (out_id) *out_id = s_slots[slot].id;
}

esp_err_t TimerEngine_StartPomodoro(uint32_t minutes, uint8_t *out_id)
{
    if (minutes == 0 || minutes > 180) return ESP_ERR_INVALID_ARG;
    if (!s_mutex) return ESP_ERR_INVALID_STATE;  /* Init not called / failed */

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_slots[POMODORO_SLOT].active) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }
    fill_slot(POMODORO_SLOT, TIMER_KIND_POMODORO, minutes, out_id);
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Pomodoro %lu min started (id=%u)",
             (unsigned long)minutes, out_id ? *out_id : 0);
    return ESP_OK;
}

esp_err_t TimerEngine_StartReminder(uint32_t minutes, uint8_t *out_id)
{
    if (minutes == 0 || minutes > 180) return ESP_ERR_INVALID_ARG;
    if (!s_mutex) return ESP_ERR_INVALID_STATE;  /* Init not called / failed */

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int slot = -1;
    for (int i = REMINDER_SLOT_FIRST; i < REMINDER_SLOT_FIRST + MAX_REMINDERS; i++) {
        if (!s_slots[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }
    fill_slot(slot, TIMER_KIND_REMINDER, minutes, out_id);
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Reminder %lu min started (id=%u)",
             (unsigned long)minutes, out_id ? *out_id : 0);
    return ESP_OK;
}

esp_err_t TimerEngine_Cancel(uint8_t id)
{
    if (id == 0) return ESP_ERR_INVALID_ARG;
    if (!s_mutex) return ESP_ERR_INVALID_STATE;  /* Init not called / failed */

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < TIMER_MAX_SLOTS; i++) {
        if (s_slots[i].active && s_slots[i].id == id) {
            s_slots[i].active = false;
            s_slots[i].id     = 0;
            xSemaphoreGive(s_mutex);
            ESP_LOGI(TAG, "Cancelled id=%u", id);
            return ESP_OK;
        }
    }
    xSemaphoreGive(s_mutex);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t TimerEngine_GetAll(timer_instance_t *out, uint8_t *count)
{
    if (!out || !count) return ESP_ERR_INVALID_ARG;
    if (!s_mutex) return ESP_ERR_INVALID_STATE;  /* Init not called / failed */
    uint8_t n = 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < TIMER_MAX_SLOTS; i++) {
        if (s_slots[i].active) out[n++] = s_slots[i];
    }
    xSemaphoreGive(s_mutex);
    *count = n;
    return ESP_OK;
}

const timer_instance_t *TimerEngine_GetPomodoro(void)
{
    static timer_instance_t snapshot;
    bool active;
    if (!s_mutex) return NULL;  /* Init not called / failed — avoid xSemaphoreTake assert */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    active = s_slots[POMODORO_SLOT].active;
    if (active) snapshot = s_slots[POMODORO_SLOT];
    xSemaphoreGive(s_mutex);
    return active ? &snapshot : NULL;
}

void TimerEngine_SetFiredCallback(timer_fired_cb_t cb)
{
    s_fired_cb = cb;
}
