#ifndef TIMER_ENGINE_H
#define TIMER_ENGINE_H

#include <stdint.h>
#include <stdbool.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TIMER_MAX_SLOTS  4   /* 1 番茄钟 + 3 定时提醒 */

typedef enum {
    TIMER_KIND_POMODORO = 0,
    TIMER_KIND_REMINDER = 1,
} timer_kind_t;

typedef struct {
    uint8_t       id;             /* 单调递增，1..255；0 表示空槽 */
    timer_kind_t  kind;
    uint32_t      total_seconds;
    uint32_t      remaining_sec;
    bool          active;
} timer_instance_t;

typedef void (*timer_fired_cb_t)(const timer_instance_t *t);

esp_err_t TimerEngine_Init(void);

/* 启动番茄钟；全局仅 1 个，已在跑返回 ESP_ERR_NO_MEM */
esp_err_t TimerEngine_StartPomodoro(uint32_t minutes, uint8_t *out_id);

/* 启动定时提醒；最多 3 个并发，满返回 ESP_ERR_NO_MEM */
esp_err_t TimerEngine_StartReminder(uint32_t minutes, uint8_t *out_id);

/* 按 id 取消；不存在返回 ESP_ERR_NOT_FOUND */
esp_err_t TimerEngine_Cancel(uint8_t id);

/* 拷贝所有活跃计时器到 out[]，*count 为拷贝数量。out 至少 TIMER_MAX_SLOTS 个元素 */
esp_err_t TimerEngine_GetAll(timer_instance_t *out, uint8_t *count);

/* 返回活跃番茄钟的静态快照指针，无则 NULL。指针在下次调用前有效 */
const timer_instance_t *TimerEngine_GetPomodoro(void);

void TimerEngine_SetFiredCallback(timer_fired_cb_t cb);

#ifdef __cplusplus
}
#endif

#endif /* TIMER_ENGINE_H */
