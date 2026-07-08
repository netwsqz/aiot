#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "lvgl_port.h"
#include "epaper_driver_bsp.h"
#include "port_touch.h"

static const char *TAG = "lvgl_port";
/* Recursive mutex: LVGL event callbacks execute inside lvgl_handler_task,
 * which already holds lvgl_mux. If a callback (or code it calls) invokes
 * Lvgl_Lock() again, a plain mutex would self-deadlock. The recursive
 * variant makes re-locking from the same task a no-op-ish increment. */
static SemaphoreHandle_t lvgl_mux = NULL;
static epaper_driver_display *s_epd = NULL;
static esp_timer_handle_t s_tick_timer = NULL;
static TaskHandle_t s_lvgl_task_handle = NULL;

/* ── EPD flush (RGB565 → 1bpp) ────────────────────── */
static void epd_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *color_p)
{
    if (!s_epd) {
        lv_display_flush_ready(disp);
        return;
    }

    uint16_t *buf = (uint16_t *)color_p;
    s_epd->EPD_Clear();
    for (int y = area->y1; y <= area->y2; y++) {
        for (int x = area->x1; x <= area->x2; x++) {
            uint8_t c = (*buf < 0x7fff) ? DRIVER_COLOR_BLACK : DRIVER_COLOR_WHITE;
            s_epd->EPD_DrawColorPixel(x, y, c);
            buf++;
        }
    }
    s_epd->EPD_DisplayPart();
    lv_display_flush_ready(disp);
}

/* ── Tick timer ────────────────────────────────────── */
static void tick_timer_cb(void *arg)
{
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

/* ── Touch read callback for LVGL ─────────────────── */
static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    uint16_t x, y;
    if (PortTouch_GetPoint(&x, &y)) {
        /* Clamp to display bounds */
        if (x >= EPD_WIDTH)  x = EPD_WIDTH - 1;
        if (y >= EPD_HEIGHT) y = EPD_HEIGHT - 1;
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

/* ── Handler task ──────────────────────────────────── */
static void lvgl_handler_task(void *arg)
{
    uint32_t delay_ms = LVGL_TASK_MAX_DELAY_MS;
    for (;;) {
        if (Lvgl_Lock(-1)) {
            delay_ms = lv_timer_handler();
            Lvgl_Unlock();
        }
        if (delay_ms > LVGL_TASK_MAX_DELAY_MS)
            delay_ms = LVGL_TASK_MAX_DELAY_MS;
        else if (delay_ms < LVGL_TASK_MIN_DELAY_MS)
            delay_ms = LVGL_TASK_MIN_DELAY_MS;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

/* ── Public API ────────────────────────────────────── */
void Lvgl_SetEpd(epaper_driver_display *epd)
{
    s_epd = epd;
}

void Lvgl_PortInit(void)
{
    lvgl_mux = xSemaphoreCreateRecursiveMutex();
    assert(lvgl_mux);

    lv_init();

    /* Display 200×200 */
    lv_display_t *disp = lv_display_create(EPD_WIDTH, EPD_HEIGHT);
    lv_display_set_flush_cb(disp, epd_flush_cb);

    /* Double buffer in PSRAM: 200 × 200 × 2 bytes = 80 KB each */
    size_t buf_size = EPD_WIDTH * EPD_HEIGHT
                    * LV_COLOR_FORMAT_GET_SIZE(LV_COLOR_FORMAT_RGB565);
    uint8_t *buf1 = (uint8_t *)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    uint8_t *buf2 = (uint8_t *)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    assert(buf1 && buf2);

    lv_display_set_buffers(disp, buf1, buf2, buf_size, LV_DISPLAY_RENDER_MODE_FULL);
    ESP_LOGI(TAG, "Display %dx%d, buf %u B x2 (PSRAM)", EPD_WIDTH, EPD_HEIGHT, (unsigned)buf_size);

    /* Touch input device */
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);
    ESP_LOGI(TAG, "Touch indev registered");

    /* Tick timer (5 ms) */
    esp_timer_create_args_t tick_args = {};
    tick_args.callback = tick_timer_cb;
    tick_args.name = "lvgl_tick";
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &s_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_tick_timer, LVGL_TICK_PERIOD_MS * 1000));

    /* Handler task: core 1, 8 KB stack, priority 4 */
    xTaskCreatePinnedToCore(lvgl_handler_task, "lvgl_handler",
                            8 * 1024, NULL, 4, &s_lvgl_task_handle, 1);

    ESP_LOGI(TAG, "LVGL port ready (tick + handler running)");
}

bool Lvgl_Lock(int timeout_ms)
{
    const TickType_t ticks =
        (timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    /* Recursive take: the LVGL handler task already holds this mutex when
     * it dispatches event callbacks, so callbacks (and anything they call)
     * can re-enter Lvgl_Lock without deadlocking. */
    return xSemaphoreTakeRecursive(lvgl_mux, ticks) == pdTRUE;
}

void Lvgl_Unlock(void)
{
    assert(lvgl_mux);
    xSemaphoreGiveRecursive(lvgl_mux);
}

void Lvgl_PortDeinit(void)
{
    /* Stop tick timer first */
    if (s_tick_timer) {
        esp_timer_stop(s_tick_timer);
        esp_timer_delete(s_tick_timer);
        s_tick_timer = NULL;
    }

    /* Delete LVGL handler task */
    if (s_lvgl_task_handle) {
        vTaskDelete(s_lvgl_task_handle);
        s_lvgl_task_handle = NULL;
    }

    /* Delete mutex */
    if (lvgl_mux) {
        vSemaphoreDelete(lvgl_mux);
        lvgl_mux = NULL;
    }

    s_epd = NULL;
    ESP_LOGI(TAG, "LVGL port deinit done");
}
