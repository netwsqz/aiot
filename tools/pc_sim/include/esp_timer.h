#ifndef PC_ESP_TIMER_H
#define PC_ESP_TIMER_H

/* PC shim: esp_timer_get_time → 用 SDL_GetTicks 换算微秒（够用，不追求单调精度）。 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void *esp_timer_handle_t;
typedef void (*esp_timer_cb_t)(void *arg);

int64_t pc_esp_timer_get_time_us(void);   /* 实现 mocks.c */

#define esp_timer_get_time()  pc_esp_timer_get_time_us()

#ifdef __cplusplus
}
#endif
#endif /* PC_ESP_TIMER_H */
