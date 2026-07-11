#ifndef PC_ESP_LOG_H
#define PC_ESP_LOG_H

/* PC shim: ESP_LOG* 宏落到 printf，TAG 可空。 */

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ESP_LOGE
#define ESP_LOGE(tag, fmt, ...)  fprintf(stderr, "E %s: " fmt "\n", (tag ? tag : "?"), ##__VA_ARGS__)
#endif
#ifndef ESP_LOGW
#define ESP_LOGW(tag, fmt, ...)  fprintf(stderr, "W %s: " fmt "\n", (tag ? tag : "?"), ##__VA_ARGS__)
#endif
#ifndef ESP_LOGI
#define ESP_LOGI(tag, fmt, ...)  fprintf(stderr, "I %s: " fmt "\n", (tag ? tag : "?"), ##__VA_ARGS__)
#endif
#ifndef ESP_LOGD
#define ESP_LOGD(tag, fmt, ...)
#endif
#ifndef ESP_LOGV
#define ESP_LOGV(tag, fmt, ...)
#endif

#ifdef __cplusplus
}
#endif
#endif /* PC_ESP_LOG_H */
