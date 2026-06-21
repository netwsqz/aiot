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
#include "standby_screen.h"
#include "esp_sleep.h"
#include "driver/rtc_io.h"
#include "nvs_flash.h"
#include "esp_spiffs.h"

#include <ctime>

static const char *TAG = "main";

/* EPD driver instance */
static epaper_driver_display *epd = nullptr;

/* Task handles for deep sleep cleanup */
static TaskHandle_t s_data_task_handle = nullptr;
static TaskHandle_t s_boot_key_task_handle = nullptr;

/* WiFi lazy-init: cold boot sets this true; wakeup path defers until BOOT long-press */
static bool s_wifi_lazy_inited = false;

static void enter_deep_sleep(void);

static void spiffs_mount(void)
{
    esp_vfs_spiffs_conf_t conf = {};
    conf.base_path = "/spiffs";
    conf.partition_label = NULL;
    conf.max_files = 2;
    conf.format_if_mount_failed = true;
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS mounted");
    } else {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(err));
    }
}

static void wifi_lazy_init(void)
{
    if (s_wifi_lazy_inited) return;
    s_wifi_lazy_inited = true;

    WifiBsp_Init();
    if (WifiBsp_SyncNtpTime()) {
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        PortRtc_SetTime(t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                        t->tm_hour, t->tm_min, t->tm_sec);
    }
    WifiAp_Init();
    TodoWeb_Init();
    ESP_LOGI(TAG, "WiFi lazy-init done");
}

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
    TickType_t press_start = 0;

    for (;;) {
        if (gpio_get_level((gpio_num_t)0) == 0) {  // BOOT pressed (low active)
            if (press_start == 0) {
                press_start = xTaskGetTickCount();
            } else if ((xTaskGetTickCount() - press_start) > pdMS_TO_TICKS(2000)) {
                // Long press 2 seconds
                ESP_LOGI(TAG, "BOOT long press detected");

                // Lazy-init WiFi stack if not already done (needed after wakeup)
                wifi_lazy_init();

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

/* PWR key double-click detection → enter deep sleep
 * Correct sequence: press → release → press within 500 ms */
static void pwr_key_task(void *arg)
{
    for (;;) {
        /* Wait for first press (PWR is active-low) */
        while (gpio_get_level(PWR_BUTTON_PIN) == 1)
            vTaskDelay(pdMS_TO_TICKS(10));

        TickType_t t1 = xTaskGetTickCount();

        /* Wait for release */
        while (gpio_get_level(PWR_BUTTON_PIN) == 0)
            vTaskDelay(pdMS_TO_TICKS(10));

        /* Look for second press within 500 ms */
        TickType_t deadline = t1 + pdMS_TO_TICKS(500);
        while (xTaskGetTickCount() < deadline) {
            if (gpio_get_level(PWR_BUTTON_PIN) == 0) {
                ESP_LOGI(TAG, "PWR double-click → entering deep sleep");
                enter_deep_sleep();
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        /* Single press — ignore */
    }
}

static void enter_deep_sleep(void)
{
    ESP_LOGI(TAG, "Preparing deep sleep...");

    /* 1. Delete application tasks FIRST — data_task may hold LVGL/I2C.
     *    Must be gone before we touch LVGL or I2C. */
    if (s_data_task_handle) {
        vTaskDelete(s_data_task_handle);
        s_data_task_handle = nullptr;
    }
    if (s_boot_key_task_handle) {
        vTaskDelete(s_boot_key_task_handle);
        s_boot_key_task_handle = nullptr;
    }
    vTaskDelay(pdMS_TO_TICKS(50));  /* Let idle task clean up; ensures data_task
                                      * has released any mutex it may hold. */

    /* 2. Stop LVGL — its handler task reads touch via I2C,
     *    and its tick timer keeps firing. */
    Lvgl_PortDeinit();

    /* 3. Stop AP + Web server if active */
    if (WifiAp_IsActive()) {
        TodoWeb_Stop();
        WifiAp_Stop();
    }

    /* 4. Deinit WiFi (unregister handlers + disconnect + stop + deinit) */
    WifiBsp_Deinit();

    /* 5. Persist timers BEFORE I2C deinit (Persist reads RTC via I2C) */
    TimerEngine_Persist();

    /* 6. Deinit I2C (removes RTC, SHTC3, Touch — all on shared bus) */
    i2c_master_Deinit();

    /* 7. Draw standby screen (reads SPIFFS for custom image — MUST be before unmount) */
    StandbyScreen_Draw(epd);

    /* 8. Unmount SPIFFS cleanly before sleep */
    esp_vfs_spiffs_unregister(NULL);

    /* 9. Hold power pins state during deep sleep */
    gpio_hold_en(EPD_PWR_PIN);     // Keep EPD powered (LOW = ON)
    gpio_hold_en(AUDIO_PWR_PIN);   // Keep audio powered (LOW = ON)
    gpio_hold_en(VBAT_PWR_PIN);    // Keep battery powered (HIGH = ON)
    gpio_deep_sleep_hold_en();     // Latch digital GPIO hold through deep sleep

    /* 10. Configure EXT1 wakeup: PWR (IO18) or BOOT (IO0), any low */
    esp_sleep_enable_ext1_wakeup(
        (1ULL << PWR_BUTTON_PIN) | (1ULL << BOOT_BUTTON_PIN),
        ESP_EXT1_WAKEUP_ANY_LOW);

    /* 11. Power domain config */
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_AUTO);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RC_FAST, ESP_PD_OPTION_OFF);
    esp_sleep_pd_config(ESP_PD_DOMAIN_XTAL, ESP_PD_OPTION_OFF);

    ESP_LOGI(TAG, "Entering deep sleep... zzz");
    vTaskDelay(pdMS_TO_TICKS(100));  // Let log flush

    esp_deep_sleep_start();
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

        /* Every 1 s: timer UI tick (fired-queue poll + per-minute refresh),
         * pomodoro mini display, and touch poll. Loop delay is 1000 ms. */
        if (Lvgl_Lock(500)) {
            TimerUi_TickUpdate();
            const timer_instance_t *pomo = TimerEngine_GetPomodoro();
            if (pomo)
                ui_update_timer_mini(true, pomo->remaining_sec, pomo->total_seconds);
            else
                ui_update_timer_mini(false, 0, 0);
            Lvgl_Unlock();
        }

        uint16_t tx, ty;
        if (PortTouch_GetPoint(&tx, &ty)) {
            ESP_LOGI(TAG, "Touch: (%d, %d)", tx, ty);
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
        tick++;
    }
}

/* ---- app_main ---- */
extern "C" void app_main(void)
{
    /* Wake from deep sleep fast recovery */
    esp_sleep_wakeup_cause_t wake_cause = esp_sleep_get_wakeup_cause();
    if (wake_cause == ESP_SLEEP_WAKEUP_EXT1) {
        ESP_LOGI(TAG, "=== Wake from deep sleep (EXT1) — fast recovery ===");

        /* NVS must be initialized before TodoMgr / TimerEngine can open it.
         * nvs_flash_init() is idempotent — returns ESP_OK if already done. */
        esp_err_t nvs_err = nvs_flash_init();
        if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES ||
            nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            nvs_flash_erase();
            nvs_flash_init();
        }

        /* SPIFFS for custom standby image */
        spiffs_mount();

        /* Reconfigure power pins BEFORE releasing hold to avoid glitch.
         * gpio_hold_dis() resets pin to default (input), so we must set
         * the desired output mode + level first; the new level takes
         * effect the instant hold is released. */
        gpio_config_t pwr_conf = {};
        pwr_conf.intr_type = GPIO_INTR_DISABLE;
        pwr_conf.mode = GPIO_MODE_OUTPUT;
        pwr_conf.pin_bit_mask = (1ULL << EPD_PWR_PIN) |
                                 (1ULL << AUDIO_PWR_PIN) |
                                 (1ULL << VBAT_PWR_PIN);
        pwr_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        pwr_conf.pull_up_en = GPIO_PULLUP_ENABLE;
        gpio_config(&pwr_conf);
        gpio_set_level(EPD_PWR_PIN, 0);
        gpio_set_level(AUDIO_PWR_PIN, 0);
        gpio_set_level(VBAT_PWR_PIN, 1);

        gpio_hold_dis(EPD_PWR_PIN);
        gpio_hold_dis(AUDIO_PWR_PIN);
        gpio_hold_dis(VBAT_PWR_PIN);

        /* I2C re-init */
        i2c_master_Init();
        ESP_LOGI(TAG, "I2C re-init done");

        /* EPD re-init (registers lost, panel retains image) */
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
        epd->EPD_DisplayPartBaseImage();
        epd->EPD_Init_Partial();
        ESP_LOGI(TAG, "EPD re-init done");

        /* LVGL re-init */
        Lvgl_PortInit();
        Lvgl_SetEpd(epd);
        ESP_LOGI(TAG, "LVGL re-init done");

        /* Sensors re-init */
        PortShtc3_Init();
        PortRtc_Init();
        ESP_LOGI(TAG, "SHTC3 + RTC re-init done");

        /* Touch + Battery */
        PortTouch_Init();
        PortBattery_Init();
        ESP_LOGI(TAG, "Touch + Battery re-init done");

        /* UI re-init */
        if (Lvgl_Lock(-1)) {
            ui_init();
            Lvgl_Unlock();
        }
        ESP_LOGI(TAG, "UI re-init done");

        /* Timer engine + UI */
        if (TimerEngine_Init() != ESP_OK) {
            ESP_LOGE(TAG, "Timer engine init FAILED");
        }
        TimerEngine_Restore();  /* Recover timers from NVS, adjust for elapsed sleep */
        if (Lvgl_Lock(-1)) {
            TimerUi_Init();
            TimerUi_SetBackCallback(timer_back_cb);
            Lvgl_Unlock();
        }
        TimerEngine_SetFiredCallback(timer_fired_cb);

        /* TODO Manager + UI */
        TodoMgr_Init();
        if (Lvgl_Lock(-1)) {
            TodoUi_Init();
            TodoUi_SetBackCallback(todo_back_cb);
            Lvgl_Unlock();
        }

        /* WiFi AP + Web deferred — lazy-init on BOOT long-press */

        /* Start data task */
        xTaskCreatePinnedToCore(data_task, "data", 7 * 1024, nullptr, 3, &s_data_task_handle, 0);

        /* Start BOOT key task */
        xTaskCreatePinnedToCore(boot_key_task, "boot_key", 4 * 1024, nullptr, 5, &s_boot_key_task_handle, 1);

        /* Start PWR key task */
        xTaskCreatePinnedToCore(pwr_key_task, "pwr_key", 3 * 1024, nullptr, 5, nullptr, 1);

        ESP_LOGI(TAG, "=== Fast recovery complete ===");
        return;
    }

    ESP_LOGI(TAG, "=== AIoT Dashboard ===");

    /* SPIFFS for custom standby image */
    spiffs_mount();

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

    /* Timer engine + UI — MUST init before data_task starts. data_task's very
     * first loop (tick=0) calls TimerEngine_GetPomodoro(); if the engine isn't
     * initialized yet s_mutex is NULL and xSemaphoreTake asserts. */
    if (TimerEngine_Init() != ESP_OK) {
        ESP_LOGE(TAG, "Timer engine init FAILED — timer features will be no-ops");
    }
    ESP_LOGI(TAG, "Timer engine init done");

    if (Lvgl_Lock(-1)) {
        TimerUi_Init();
        TimerUi_SetBackCallback(timer_back_cb);
        Lvgl_Unlock();
    }
    ESP_LOGI(TAG, "Timer UI init done");

    TimerEngine_SetFiredCallback(timer_fired_cb);
    ESP_LOGI(TAG, "Timer fired callback registered");

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
    s_wifi_lazy_inited = true;
    ESP_LOGI(TAG, "WiFi AP + Web Server init done");

    /* 11. Data collection task (Core 0, priority 3).
     * MUST start after TimerEngine_Init — its tick=0 loop calls
     * TimerEngine_GetPomodoro(), which takes s_mutex; if the engine
     * isn't up yet s_mutex is NULL and xSemaphoreTake asserts. */
    xTaskCreatePinnedToCore(data_task, "data", 7 * 1024, nullptr, 3, &s_data_task_handle, 0);

    /* 15. BOOT key detection task */
    xTaskCreatePinnedToCore(boot_key_task, "boot_key", 4 * 1024, nullptr, 5, &s_boot_key_task_handle, 1);
    ESP_LOGI(TAG, "BOOT key task started");

    /* 16. PWR key double-click detection task */
    xTaskCreatePinnedToCore(pwr_key_task, "pwr_key", 3 * 1024, nullptr, 5, nullptr, 1);
    ESP_LOGI(TAG, "PWR key task started");

    ESP_LOGI(TAG, "=== All tasks started ===");
}
