#ifndef PC_ESP_RANDOM_H
#define PC_ESP_RANDOM_H

/* PC shim: esp_random → 用 rand()。不是密码学随机，UI 选句够用。 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t esp_random(void);

#ifdef __cplusplus
}
#endif
#endif /* PC_ESP_RANDOM_H */
