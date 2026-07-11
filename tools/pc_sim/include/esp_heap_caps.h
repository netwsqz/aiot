#ifndef PC_ESP_HEAP_CAPS_H
#define PC_ESP_HEAP_CAPS_H

/* PC shim: heap_caps_malloc → malloc。PSRAM 标志在主机无意义，照常给堆。 */

#include <stdlib.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MALLOC_CAP_SPIRAM       (1 << 0)
#define MALLOC_CAP_INTERNAL     (1 << 1)
#define MALLOC_CAP_DMA          (1 << 2)
#define MALLOC_CAP_8BIT         (1 << 3)
#define MALLOC_CAP_DEFAULT      MALLOC_CAP_INTERNAL

static inline void *heap_caps_malloc(size_t size, uint32_t caps)
{
    (void)caps;
    return malloc(size);
}

static inline void *heap_caps_calloc(size_t n, size_t sz, uint32_t caps)
{
    (void)caps;
    return calloc(n, sz);
}

static inline void *heap_caps_realloc(void *p, size_t sz, uint32_t caps)
{
    (void)caps;
    return realloc(p, sz);
}

#ifdef __cplusplus
}
#endif
#endif /* PC_ESP_HEAP_CAPS_H */
