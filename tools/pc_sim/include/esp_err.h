#ifndef PC_ESP_ERR_H
#define PC_ESP_ERR_H

/* PC shim: 把 ESP-IDF 的 esp_err_t 搬到主机，让 UI 源码不改一行就能编。 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int esp_err_t;

#define ESP_OK          0
#define ESP_FAIL        -1
#define ESP_ERR_NO_MEM  0x101
#define ESP_ERR_NOT_FOUND 0x104
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103

#ifdef __cplusplus
}
#endif
#endif /* PC_ESP_ERR_H */
