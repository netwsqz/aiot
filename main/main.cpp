#include <cstdio>
#include <cstring>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include "epaper_driver_bsp.h"
#include "lvgl_port.h"
#include "ui.h"
#include "i2c_bsp.h"
#include "port_shtc3.h"
#include "port_rtc.h"
#include "port_touch.h"
#include "port_battery.h"
#include "wifi_bsp.h"
#include "app_config.h"
#include "todo_manager.h"
#include "wifi_ap.h"
#include "todo_web.h"
#include "todo_ui.h"
#include "timer_engine.h"
#include "timer_ui.h"

#include <ctime>

static const char *TAG = "main";

/* EPD driver instance */
static epaper_driver_display *epd = nullptr;

/* Back callback: TODO lists page → dashboard.
 * ALWAYS invoked from an LVGL event callback (back_btn_cb / swipe_gesture_cb),
 * which already runs inside Lvgl_Lock. Re-acquiring the (non-recursive) mutex
 * here deadlocks and silently drops the navigation — do NOT lock. */
static void todo_back_cb(void)
{
    ui_switch_page(PAGE_DASHBOARD);
}

/* Timer tools back callback: tools page → dashboard. Runs inside LVGL lock. */
static void timer_back_cb(void)
{
    ui_switch_page(PAGE_DASHBOARD);
}

/* Timer fired: runs in FreeRTOS timer service task — MUST NOT touch LVGL.
 * Just enqueue; data_task consumes it under the LVGL lock. */
static void timer_fired_cb(const timer_instance_t *t)
{
    TimerUi_PushFiredEvent(t->id, t->kind, t->total_seconds);
}

/* BOOT key long-press detection */
static void boot_key_task(void *arg)
{
    uint32_t press_start = 0;

    for (;;) {
        if (gpio_get_level((gpio_num_t)0) == 0) {  // BOOT pressed (low active)
            if (press_start == 0) {
                press_start = xTaskGetTickCount() * portTICK_PERIOD_MS;
            } else if ((xTaskGetTickCount() * portTICK_PERIOD_MS - press_start) > 2000) {
                // Long press 2 seconds
                ESP_LOGI(TAG, "BOOT long press detected");

                if (WifiAp_IsActive()) {
                    TodoWeb_Stop();
                    WifiAp_Stop();
                    ESP_LOGI(TAG, "AP stopped");
                } else {
                    if (WifiAp_Start() == ESP_OK) {
                        TodoWeb_Start();
                        ESP_LOGI(TAG, "AP started, connect to 'AIoT-TODO'");
                    }
                }

                press_start = 0;
                vTaskDelay(pdMS_TO_TICKS(500));  // Debounce
            }
        } else {
            press_start = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* ---- Data collection task ---- */
static void data_task(void *arg)
{
    char time_buf[16], date_buf[32];
    static const char *month_en[] = {"","Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    static const char *weekday_en[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    uint32_t tick = 0;

    for (;;) {
        /* RTC — every 10 s */
        if (tick % 10 == 0) {
            rtc_datetime_t dt = PortRtc_GetTime();
            snprintf(time_buf, sizeof(time_buf), "%02d:%02d",
                     dt.hour, dt.minute);
            snprintf(date_buf, sizeof(date_buf), "%s %d %s",
                     (dt.month >= 1 && dt.month <= 12) ? month_en[dt.month] : "",
                     dt.day,
                     (dt.week <= 6) ? weekday_en[dt.week] : "");

            if (Lvgl_Lock(500)) {
                ui_update_time(time_buf, date_buf);
                Lvgl_Unlock();
            }
        }

        /* SHTC3 — every 30 s */
        if (tick % 30 == 0) {
            shtc3_data_t sht = PortShtc3_Read();
            if (Lvgl_Lock(500)) {
                ui_update_temperature((int)(sht.Temp + 0.5f));
                ui_update_humidity((int)(sht.RH + 0.5f));
                Lvgl_Unlock();
            }
            ESP_LOGI(TAG, "SHTC3: %.1f°C  %.1f%%", sht.Temp, sht.RH);
        }

        /* Battery — every 60 s */
        if (tick % 60 == 0) {
            uint8_t level = PortBattery_GetLevel();
            if (Lvgl_Lock(500)) {
                ui_update_battery(level);
                Lvgl_Unlock();
            }
        }

        /* TODO UI refresh — every 30 s */
        if (tick % 30 == 0) {
            if (Lvgl_Lock(500)) {
                TodoUi_Refresh();
                ui_update_todo_summary();
                Lvgl_Unlock();
            }
        }

        /* WiFi status — every 10 s */
        if (tick % 10 == 0) {
            bool connected = WifiBsp_IsConnected();
            if (Lvgl_Lock(500)) {
                ui_update_wifi_status(connected);
                Lvgl_Unlock();
            }
        }

        /* Touch — every 200 ms */
        if (tick % 1 == 0) {
            /* Timer UI: poll fired queue + per-minute refresh (every 1 s) */
            if (Lvgl_Lock(500)) {
                TimerUi_TickUpdate();
                Lvgl_Unlock();
            }

            uint16_t tx, ty;
            if (PortTouch_GetPoint(&tx, &ty)) {
                ESP_LOGI(TAG, "Touch: (%d, %d)", tx, ty);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
        tick++;
    }
}

/* ---- app_main ---- */
extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=== AIoT Dashboard ===");

    /* 1. Power on — EPD + Audio (match V2 power sequence) */
    gpio_config_t pwr_conf = {};
    pwr_conf.intr_type = GPIO_INTR_DISABLE;
    pwr_conf.mode = GPIO_MODE_OUTPUT;
    pwr_conf.pin_bit_mask = (1ULL << EPD_PWR_PIN) |
                             (1ULL << AUDIO_PWR_PIN) |
                             (1ULL << VBAT_PWR_PIN);
    pwr_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    pwr_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&pwr_conf);
    gpio_set_level(EPD_PWR_PIN, 0);     /* Low  = ON */
    gpio_set_level(AUDIO_PWR_PIN, 0);   /* Low  = ON */
    gpio_set_level(VBAT_PWR_PIN, 1);    /* High = ON */
    ESP_LOGI(TAG, "Power on");

    /* 2. I2C bus */
    i2c_master_Init();
    ESP_LOGI(TAG, "I2C init done");

    /* 3. EPD driver */
    custom_lcd_spi_t lcd_cfg = {
        .cs = EPD_CS_PIN,
        .dc = EPD_DC_PIN,
        .rst = EPD_RST_PIN,
        .busy = EPD_BUSY_PIN,
        .mosi = EPD_MOSI_PIN,
        .scl = EPD_SCK_PIN,
        .spi_host = EPD_SPI_NUM,
        .buffer_len = 5000,
    };
    epd = new epaper_driver_display(EPD_WIDTH, EPD_HEIGHT, lcd_cfg);
    epd->EPD_Init();
    epd->EPD_Clear();
    epd->EPD_DisplayPartBaseImage();
    epd->EPD_Init_Partial();
    ESP_LOGI(TAG, "EPD init done");

    /* 4. LVGL */
    Lvgl_PortInit();
    Lvgl_SetEpd(epd);
    ESP_LOGI(TAG, "LVGL init done");

    /* 5-6. Sensors */
    PortShtc3_Init();
    PortRtc_Init();
    ESP_LOGI(TAG, "SHTC3 + RTC init done");

    /* 7. WiFi + NTP */
    WifiBsp_Init();
    if (WifiBsp_SyncNtpTime()) {
        /* NTP synced → write system time to RTC chip */
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        PortRtc_SetTime(t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                        t->tm_hour, t->tm_min, t->tm_sec);
        ESP_LOGI(TAG, "NTP sync OK, RTC updated");
    } else {
        ESP_LOGW(TAG, "NTP sync failed");
    }

    /* 8-9. Touch + Battery */
    PortTouch_Init();
    PortBattery_Init();
    ESP_LOGI(TAG, "Touch + Battery init done");

    /* 10. UI */
    if (Lvgl_Lock(-1)) {
        ui_init();
        Lvgl_Unlock();
    }
    ESP_LOGI(TAG, "UI init done");

    /* 11. Data collection task (Core 0, priority 3) */
    xTaskCreatePinnedToCore(data_task, "data", 7 * 1024, nullptr, 3, nullptr, 0);

    /* 12. TODO Manager */
    TodoMgr_Init();
    ESP_LOGI(TAG, "TODO Manager init done");

    /* 13. TODO UI (separate screen, navigated from dashboard) */
    if (Lvgl_Lock(-1)) {
        TodoUi_Init();
        TodoUi_SetBackCallback(todo_back_cb);
        Lvgl_Unlock();
    }
    ESP_LOGI(TAG, "TODO UI init done");
    /* Note: ui_update_todo_summary() lives on data_task's larger stack
     * (~3.7KB of locals) — calling it from app_main here would overflow
     * the main task stack and corrupt FreeRTOS internals. */

    /* 14. WiFi AP + Web Server */
    WifiAp_Init();
    TodoWeb_Init();
    ESP_LOGI(TAG, "WiFi AP + Web Server init done");

    /* 16. Timer engine */
    TimerEngine_Init();
    ESP_LOGI(TAG, "Timer engine init done");

    /* 17. Timer UI */
    if (Lvgl_Lock(-1)) {
        TimerUi_Init();
        TimerUi_SetBackCallback(timer_back_cb);
        Lvgl_Unlock();
    }
    ESP_LOGI(TAG, "Timer UI init done");

    TimerEngine_SetFiredCallback(timer_fired_cb);
    ESP_LOGI(TAG, "Timer fired callback registered");

    /* 15. BOOT key detection task */
    xTaskCreatePinnedToCore(boot_key_task, "boot_key", 4 * 1024, nullptr, 5, nullptr, 1);
    ESP_LOGI(TAG, "BOOT key task started");

    ESP_LOGI(TAG, "=== All tasks started ===");
}
