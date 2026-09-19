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
#include "weather_fetch.h"
#include "weather_cache.h"
#include "weather_ui.h"
#include "wifi_ui.h"
#include "pet_ui.h"
#include "draw_ui.h"
#include "snake_ui.h"
#include "app_list_ui.h"
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
static TaskHandle_t s_pwr_key_task_handle = nullptr;
/* Reentrancy guard: enter_deep_sleep() does a multi-step teardown
 * (delete tasks → deinit LVGL/WiFi/I2C → draw standby → sleep). If the
 * user keeps mashing PWR, pwr_key_task could fire a second teardown while
 * the first is mid-flight and half the peripherals are gone. This flag
 * makes the second call a no-op. */
static volatile bool s_entering_sleep = false;

/* Last tick when user was on a non-pet page (for idle return-to-pet). */
static volatile uint32_t s_last_activity_tick = 0;

/* TODO 数据被改过。TodoMgr 的 change 回调在 httpd / LVGL 事件上下文里触发，
 * 那里绝不能碰 LVGL；所以回调只置这个标志，真正的重绘留在 data_task 里、
 * 已经持有 LVGL 锁的地方做（和 WeatherFetch_RequestRefresh 同一个套路）。
 * volatile：写入者可能和读取者不在同一个上下文。 */
static volatile bool s_todo_dirty = false;

static void todo_changed_cb(void)
{
    s_todo_dirty = true;
}

/* WiFi lazy-init: cold boot sets this true; wakeup path defers until BOOT long-press */
static bool s_wifi_lazy_inited = false;

/* HTTP server "should be running" intent. Set true once we want the web API
 * available; the data_task monitors WiFi STA state and starts/stops the HTTP
 * server to match — server follows STA connectivity, NOT the AP hotspot.
 * (AP hotspot lifecycle is still owned by the BOOT long-press handler.) */
static bool s_web_want_running = false;
static bool s_web_is_running = false;

/* ── Weather state ──
 * s_weather_snapshot holds the most recent successful (or cached) snapshot.
 * s_weather_has_cached tracks whether we have any data to render.
 * s_indoor_temp_c caches the latest SHTC3 reading so the weather summary
 * can show indoor temp alongside outdoor data (SHTC3 is read inline in
 * data_task; without a cache we'd have no value to pass to the summary). */
static weather_snapshot_t s_weather_snapshot;
static bool s_weather_has_cached = false;
static int s_weather_fetch_tick = 0;          /* seconds since last fetch attempt */
static int s_weather_consecutive_fail = 0;    /* backoff multiplier source */
static float s_indoor_temp_c = 0.0f;          /* latest SHTC3 temperature */

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
    /* HTTP server now follows STA connectivity (see web_server_reconcile).
     * Mark intent so data_task starts the server once STA is up. */
    s_web_want_running = true;
    ESP_LOGI(TAG, "WiFi lazy-init done");
}

/* Reconcile HTTP server state with intent + STA connectivity.
 * Server follows STA (not the AP hotspot): start when STA is up and we want
 * it, stop when STA drops so we don't serve on a dead interface. Idempotent. */
static void web_server_reconcile(void)
{
    bool want = s_web_want_running && WifiBsp_IsConnected();
    if (want && !s_web_is_running) {
        if (TodoWeb_Start() == ESP_OK) {
            s_web_is_running = true;
            ESP_LOGI(TAG, "HTTP server started (STA up)");
        }
    } else if (!want && s_web_is_running) {
        TodoWeb_Stop();
        s_web_is_running = false;
        ESP_LOGI(TAG, "HTTP server stopped (STA down)");
    }
}

/* Weather refresh: wired to the WiFi management page's refresh button by
 * ui_init_all(). Runs inside LVGL lock (button click). Just sets the refresh
 * flag — the actual HTTPS fetch happens in data_task on the next 1s tick,
 * OUTSIDE the LVGL lock (TLS + JSON parse must not hold the UI mutex). */
static void weather_refresh_cb(void)
{
    if (WifiBsp_IsConnected()) {
        WeatherFetch_RequestRefresh();
        ESP_LOGI(TAG, "Weather refresh requested (on-screen button)");
    } else {
        ESP_LOGW(TAG, "Weather refresh requested but WiFi down");
    }
}

/* ── WiFi 管理页面回调（均在 LVGL 锁内触发，只做轻量动作） ── */

/* AP 启停按钮：WiFi 管理页里的显式入口（长按 BOOT 不再切 AP，只进界面）。
 * 这里仍在 LVGL 锁内，但 WifiAp_Start/Stop 内部只配 wifi 模式 + 起停定时器，
 * 不持有 LVGL 锁，安全。WifiAp_Stop 切回 WIFI_MODE_STA，不影响 STA 连接。 */
static void wifi_toggle_ap_cb(void)
{
    wifi_lazy_init();
    if (WifiAp_IsActive()) {
        WifiAp_Stop();
        ESP_LOGI(TAG, "AP stopped (wifi_ui button)");
    } else {
        if (WifiAp_Start() == ESP_OK) {
            ESP_LOGI(TAG, "AP started (wifi_ui button)");
        }
    }
}

/* STA 重连：触发 WifiBsp_Reconnect（非阻塞，仅发起重连）。
 * 在 LVGL 锁内调用安全——Reconnect 不持有任何 UI 锁。 */
static void wifi_reconnect_cb(void)
{
    if (!s_wifi_lazy_inited) {
        wifi_lazy_init();
    }
    if (WifiBsp_Reconnect() == ESP_OK) {
        ESP_LOGI(TAG, "STA reconnect requested (wifi_ui button)");
    } else {
        ESP_LOGW(TAG, "STA reconnect failed (wifi_ui button)");
    }
}

/* Timer fired: runs in FreeRTOS timer service task — MUST NOT touch LVGL.
 * Just enqueue; data_task consumes it under the LVGL lock. */
static void timer_fired_cb(const timer_instance_t *t)
{
    TimerUi_PushFiredEvent(t->id, t->kind, t->total_seconds);
}

/* BOOT key:
 * - short press (< 300 ms): cycle pet ↔ dashboard ↔ todo; other screens → pet
 * - long press (2 s): enter WiFi management page (AP toggle moved to UI button). */
static void boot_key_task(void *arg)
{
    TickType_t press_start = 0;
    bool long_handled = false;

    for (;;) {
        if (gpio_get_level((gpio_num_t)0) == 0) {  // BOOT pressed (low active)
            if (press_start == 0) {
                press_start = xTaskGetTickCount();
                long_handled = false;
            } else if (!long_handled &&
                       (xTaskGetTickCount() - press_start) > pdMS_TO_TICKS(2000)) {
                // Long press 2 seconds
                ESP_LOGI(TAG, "BOOT long press detected");
                long_handled = true;

                // Lazy-init WiFi stack if not already done (needed after wakeup)
                wifi_lazy_init();

                /* 长按 BOOT 仅进入 WiFi 管理界面，AP 启停改由界面内的按钮控制。
                 * HTTP server 仍跟随 STA 连通性（见 data_task 的 web_server_reconcile），
                 * 与 AP 热点生命周期解耦。
                 * boot_key_task 在 Core 1，跨核取 LVGL 锁安全（参考 data_task）。 */
                if (Lvgl_Lock(500)) {
                    ui_switch_page(PAGE_WIFI_MGMT);
                    Lvgl_Unlock();
                }
            }
        } else {
            /* Released */
            if (press_start != 0 && !long_handled) {
                TickType_t held = xTaskGetTickCount() - press_start;
                if (held < pdMS_TO_TICKS(300)) {
                    /* Short press: cycle screens — except when on the draw
                     * board, where BOOT short-press = save & exit to dashboard.
                     * (画板不用长按退出——LVGL LONG_PRESSED 与画线冲突，
                     * 见 DrawUi_SaveAndExit 注释。) */
                    if (Lvgl_Lock(500)) {
                        if (ui_get_current_page() == PAGE_DRAW) {
                            DrawUi_SaveAndExit();
                            DrawUi_RestoreIndevPeriod();
                        } else if (ui_get_current_page() == PAGE_SNAKE) {
                            /* 贪吃蛇页：BOOT 短按 = 停 timer + 退回 dashboard */
                            SnakeUi_Exit();
                            ui_back_to_dashboard();
                        } else {
                            ui_page_t cur = ui_get_current_page();
                            if (cur == PAGE_PET) {
                                ui_switch_page(PAGE_DASHBOARD);
                            } else if (cur == PAGE_DASHBOARD) {
                                ui_switch_page(PAGE_TODO_LISTS);
                            } else {
                                ui_switch_page(PAGE_PET);
                            }
                        }
                        Lvgl_Unlock();
                    }
                }
            }
            press_start = 0;
            long_handled = false;
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
    /* Reentrancy guard: a second PWR double-click while the first call is
     * tearing down peripherals would deinit I2C/LVGL a second time and
     * panic. Once we're committed, ignore further triggers. */
    if (s_entering_sleep) return;
    s_entering_sleep = true;

    ESP_LOGI(TAG, "Preparing deep sleep...");

    /* We do NOT delete pwr_key_task here, even though this function runs
     * inside it. FreeRTOS marks a self-deleting task for deletion at the
     * next yield (vTaskDelay/vTaskSuspend); after that yield the scheduler
     * never returns to this task, so every line below (LVGL/WiFi/I2C
     * teardown, standby draw, esp_deep_sleep_start) would be dead code and
     * the device would hang awake draining battery. The s_entering_sleep
     * guard above already makes a second PWR double-click a no-op, so
     * deleting the task is unnecessary. Run the full teardown in this task
     * and let esp_deep_sleep_start() end it. */
    (void)s_pwr_key_task_handle;

    /* 1. Delete application tasks — data_task may hold LVGL/I2C.
     *    Must be gone before we touch LVGL or I2C. The 50 ms delay lets
     *    the idle task reclaim their stacks; it's not a guarantee that
     *    data_task released every mutex, but vTaskDelete can't wait for
     *    that. The deepest hazard (data_task mid-I2C-transfer) is covered
     *    by i2c_master_Deinit recreating the bus on wake. */
    if (s_data_task_handle) {
        vTaskDelete(s_data_task_handle);
        s_data_task_handle = nullptr;
    }
    if (s_boot_key_task_handle) {
        vTaskDelete(s_boot_key_task_handle);
        s_boot_key_task_handle = nullptr;
    }
    vTaskDelay(pdMS_TO_TICKS(50));

    /* 2. Stop LVGL — its handler task reads touch via I2C,
     *    and its tick timer keeps firing. */
    Lvgl_PortDeinit();

    /* 3. Stop HTTP server if running (decoupled from AP) */
    if (s_web_is_running) {
        TodoWeb_Stop();
        s_web_is_running = false;
    }
    s_web_want_running = false;

    /* 4. Tear down the AP first if it's up — WifiBsp_Deinit calls
     *    esp_wifi_stop()/deinit() and APSTA mode must be collapsed to STA
     *    cleanly before the driver goes away. WifiAp_Stop is a no-op if
     *    the AP is already idle. */
    if (s_wifi_lazy_inited) {
        WifiAp_Stop();
    }

    /* 5. Deinit WiFi (unregister handlers + disconnect + stop + deinit) */
    WifiBsp_Deinit();

    /* 6. Persist timers BEFORE I2C deinit (Persist reads RTC via I2C) */
    TimerEngine_Persist();

    /* 7. Detach the FT6336 touch device, THEN tear down the shared I2C bus.
     *    The touch device handle lives in port_touch.c and i2c_bsp can't
     *    see it; calling i2c_del_master_bus with the device still attached
     *    is undefined and leaks the slot across sleep/wake cycles. */
    PortTouch_Deinit();
    i2c_master_Deinit();

    /* 8. Draw standby screen (reads SPIFFS for custom image — MUST be before unmount) */
    StandbyScreen_Draw(epd);

    /* 9. Unmount SPIFFS cleanly before sleep */
    esp_vfs_spiffs_unregister(NULL);

    /* 10. Hold power pins state during deep sleep */
    gpio_hold_en(EPD_PWR_PIN);     // Keep EPD powered (LOW = ON)
    gpio_hold_en(AUDIO_PWR_PIN);   // Keep audio powered (LOW = ON)
    gpio_hold_en(VBAT_PWR_PIN);    // Keep battery powered (HIGH = ON)
    gpio_deep_sleep_hold_en();     // Latch digital GPIO hold through deep sleep

    /* 11. Configure EXT1 wakeup: PWR (IO18) or BOOT (IO0), any low */
    esp_sleep_enable_ext1_wakeup(
        (1ULL << PWR_BUTTON_PIN) | (1ULL << BOOT_BUTTON_PIN),
        ESP_EXT1_WAKEUP_ANY_LOW);

    /* 12. Power domain config */
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
    /* 中文短格式日期，适配首页左上 100×20 区域（lv_font_chinese_14）。
     * 周几用"周X"，避免与英文 month_en/weekday_en 覆盖中文显示。 */
    static const char *weekday_cn[] = {"周日","周一","周二","周三","周四","周五","周六"};
    uint32_t tick = 0;

    for (;;) {
        /* RTC — every 10 s */
        if (tick % 10 == 0) {
            rtc_datetime_t dt = PortRtc_GetTime();
            snprintf(time_buf, sizeof(time_buf), "%02d:%02d",
                     dt.hour, dt.minute);
            snprintf(date_buf, sizeof(date_buf), "%d月%d日 %s",
                     dt.month, dt.day,
                     (dt.week <= 6) ? weekday_cn[dt.week] : "");

            if (Lvgl_Lock(500)) {
                ui_update_time(time_buf, date_buf);
                Lvgl_Unlock();
            }
        }

        /* SHTC3 — every 30 s */
        if (tick % 30 == 0) {
            shtc3_data_t sht = PortShtc3_Read();
            s_indoor_temp_c = sht.Temp;   /* cache for weather summary */
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

        /* TODO UI refresh — 有改动就立刻刷（s_todo_dirty 由 httpd/UI 事件里的
         * TodoMgr change 回调置位），否则每 30 s 兜底一次。 */
        if (s_todo_dirty || tick % 30 == 0) {
            if (Lvgl_Lock(500)) {
                TodoUi_Refresh();
                ui_update_todo_summary();
                s_todo_dirty = false;
                Lvgl_Unlock();
            }
        }

        /* Pet UI refresh — every 30 s (recompute expression) */
        if (tick % 30 == 0) {
            if (Lvgl_Lock(500)) {
                PetUi_Refresh();
                Lvgl_Unlock();
            }
        }

        /* Idle return to pet page: if user stays on a non-pet, non-dashboard
         * tool page for 60 s, auto-switch back to the pet (screen-saver
         * behavior). Dashboard is the user's mixed companion summary — it is
         * NOT auto-left, so they can read todo/pomodoro/sensors at leisure.
         * Todo/timer/weather/wifi/draw tool pages still time out.
         * PAGE_SNAKE 例外——游戏中不让闲置逻辑把人踢回 pet 屏；退出靠 BOOT 短按。*/
        {
            ui_page_t cur = ui_get_current_page();
            if (cur != PAGE_PET && cur != PAGE_DASHBOARD && cur != PAGE_SNAKE) {
                if (s_last_activity_tick == 0) s_last_activity_tick = tick;
                if ((tick - s_last_activity_tick) >= 60) {
                    if (Lvgl_Lock(500)) {
                        ui_switch_page(PAGE_PET);
                        Lvgl_Unlock();
                    }
                    s_last_activity_tick = 0;
                }
            } else {
                s_last_activity_tick = 0;
            }
        }

        /* WiFi status — every 10 s.
         * Also reconciles the HTTP server: it follows STA connectivity, so
         * start it the moment STA comes up (no need to long-press BOOT just
         * to reach the web API), and stop it if STA drops. */
        if (tick % 10 == 0) {
            bool connected = WifiBsp_IsConnected();
            bool ap_active = s_wifi_lazy_inited ? WifiAp_IsActive() : false;
            if (Lvgl_Lock(500)) {
                ui_update_wifi_status(connected);
                /* 若当前在 WiFi 管理页，同步刷新该页的 AP/STA 状态显示 */
                if (ui_get_current_page() == PAGE_WIFI_MGMT) {
                    char ssid_buf[40] = "";
                    char ip_buf[20] = "";
                    if (connected) {
                        WifiBsp_GetSsid(ssid_buf, sizeof(ssid_buf));
                        WifiBsp_GetIp(ip_buf, sizeof(ip_buf));
                    }
                    int stas = ap_active ? WifiAp_GetStaCount() : 0;
                    WifiUi_UpdateStatus(connected, ap_active,
                                        ssid_buf, ip_buf, NULL, stas);
                }
                Lvgl_Unlock();
            }
            web_server_reconcile();
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
            /* AI engine tick removed (AI headers not in this checkout). */
            Lvgl_Unlock();
        }

        uint16_t tx, ty;
        if (PortTouch_GetPoint(&tx, &ty)) {
            ESP_LOGI(TAG, "Touch: (%d, %d)", tx, ty);
        }

        /* ── Weather refresh (1 s tick, throttled internally) ──
         * HTTP fetch + NVS save run OUTSIDE Lvgl_Lock; only the UI
         * render/summary update acquires the lock. Backoff doubles the
         * interval (up to 2 h) after 3 consecutive failures.
         * Web API can request an immediate refresh via WeatherFetch_RequestRefresh(). */
        if (WeatherFetch_IsReady() && WifiBsp_IsConnected()) {
            int interval = CONFIG_WEATHER_FETCH_INTERVAL_MIN * 60;  /* seconds */
            if (s_weather_consecutive_fail >= 3) {
                int shift = s_weather_consecutive_fail - 2;  /* 3→×2, 4→×4, ... */
                if (shift > 6) shift = 6;                    /* cap ×64 */
                interval = interval << shift;
                if (interval > 7200) interval = 7200;
            }
            s_weather_fetch_tick++;
            bool due = (s_weather_fetch_tick >= interval);
            bool manual = WeatherFetch_ConsumeRefreshRequest();
            if (due || manual) {
                s_weather_fetch_tick = 0;
                if (manual) ESP_LOGI(TAG, "Weather: manual refresh triggered");
                weather_snapshot_t snap;
                memset(&snap, 0, sizeof(snap));
                bool ok = WeatherFetch_Update(&snap);  /* NOT under Lvgl_Lock */
                if (ok) {
                    WeatherCache_Save(&snap);           /* NOT under Lvgl_Lock */
                    /* Snapshot copy moved INSIDE the Lvgl_Lock below: the
                     * ~300-byte struct write was racing with the Lvgl_Lock'd
                     * read in ui_update_weather_summary. Doing the copy under
                     * the same lock that guards the read closes the race.
                     * s_weather_has_cached is also set under the lock so a
                     * reader can't see cached==true with a still-stale
                     * snapshot (the flag is the contract that the snapshot
                     * is valid). */
                    s_weather_consecutive_fail = 0;
                    if (Lvgl_Lock(500)) {
                        s_weather_snapshot = snap;
                        s_weather_has_cached = true;
                        /* Home screen shows indoor sensors only now; the
                         * summary call repaints the weather page if it's
                         * currently open. */
                        ui_update_weather_summary(&s_weather_snapshot,
                                                  s_indoor_temp_c, true);
                        Lvgl_Unlock();
                    }
                } else {
                    ESP_LOGW(TAG, "Weather fetch failed: %s",
                             WeatherFetch_LastError());
                    s_weather_consecutive_fail++;
                }
            }
        } else {
            /* Drain a stale manual request so it doesn't fire the moment
             * WiFi/config becomes ready later. */
            (void)WeatherFetch_ConsumeRefreshRequest();
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
        /* Free the previous EPD object before re-allocating. On wake we
         * re-run app_main, so the global `epd` still points at the
         * pre-sleep object whose SPI context is now invalid. Deleting it
         * runs the destructor that frees the PSRAM framebuffer — without
         * this, every wake leaked ~5KB of PSRAM plus a dangling object. */
        if (epd) {
            delete epd;
            epd = nullptr;
        }
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

        /* UI: dashboard + every sub-page. One call — see ui_init_all(). */
        if (Lvgl_Lock(-1)) {
            ui_init();
            Lvgl_Unlock();
        }
        ESP_LOGI(TAG, "UI re-init done");

        /* Timer engine. MUST init before data_task starts (tick=0 calls
         * TimerEngine_GetPomodoro, which takes s_mutex). UI comes after. */
        if (TimerEngine_Init() != ESP_OK) {
            ESP_LOGE(TAG, "Timer engine init FAILED");
        }
        TimerEngine_Restore();  /* Recover timers from NVS, adjust for elapsed sleep */

        /* AI engine + UI re-init removed (AI headers not in this checkout). */

        /* TODO Manager (no UI — ui_init_all below covers it) */
        TodoMgr_Init();
        TodoMgr_SetChangeCallback(todo_changed_cb);

        /* Weather: restore cache from NVS (no WiFi on wake path), render it.
         * WiFi stays deferred until BOOT long-press; weather won't refresh
         * until then — by design, per the spec.
         * The cache must be loaded BEFORE ui_init_all(), which builds the
         * weather page. */
        WeatherCache_Init();
        s_weather_has_cached = WeatherCache_Load(&s_weather_snapshot);
        WeatherFetch_Init();

        /* Every sub-page: one row each in ui_init_all()'s table — init under
         * the LVGL lock + back callback wired to ui_back_to_dashboard(). */
        ui_init_all();
        TimerEngine_SetFiredCallback(timer_fired_cb);

        /* Page-specific wiring that isn't a back button. */
        if (Lvgl_Lock(-1)) {
            WifiUi_SetToggleApCallback(wifi_toggle_ap_cb);
            WifiUi_SetReconnectCallback(wifi_reconnect_cb);
            WifiUi_SetRefreshWeatherCallback(weather_refresh_cb);
            if (s_weather_has_cached) {
                WeatherUI_Render(&s_weather_snapshot, true);
            }
            Lvgl_Unlock();
        }
        ESP_LOGI(TAG, "Sub-pages init done (weather cache: %s)",
                 s_weather_has_cached ? "loaded" : "empty");

        /* WiFi AP + Web deferred — lazy-init on BOOT long-press */

        /* Start data task */
        /* data_task stack must fit mbedTLS HTTPS handshake (~30KB peak
         * during TLS connect) plus the 4KB JSON recv buffers in
         * WeatherFetch_Update, on top of the regular sensor/UI work.
         * 7KB was too small — caused stack overflow panic on weather fetch. */
        xTaskCreatePinnedToCore(data_task, "data", 12 * 1024, nullptr, 3, &s_data_task_handle, 0);

        /* Start BOOT key task */
        xTaskCreatePinnedToCore(boot_key_task, "boot_key", 4 * 1024, nullptr, 5, &s_boot_key_task_handle, 1);

        /* Start PWR key task */
        xTaskCreatePinnedToCore(pwr_key_task, "pwr_key", 3 * 1024, nullptr, 5, &s_pwr_key_task_handle, 1);

        /* Default screen on wake too — match cold-boot behavior so the
         * device always lands on the pet screen, not whichever page
         * ui_init happened to load last. */
        if (Lvgl_Lock(-1)) {
            ui_switch_page(PAGE_PET);
            Lvgl_Unlock();
        }

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

    /* Timer engine — MUST init before data_task starts. data_task's very
     * first loop (tick=0) calls TimerEngine_GetPomodoro(); if the engine isn't
     * initialized yet s_mutex is NULL and xSemaphoreTake asserts. */
    if (TimerEngine_Init() != ESP_OK) {
        ESP_LOGE(TAG, "Timer engine init FAILED — timer features will be no-ops");
    }
    ESP_LOGI(TAG, "Timer engine init done");

    /* AI engine + UI init removed (AI headers not in this checkout). */

    /* 12. TODO Manager */
    TodoMgr_Init();
    TodoMgr_SetChangeCallback(todo_changed_cb);
    ESP_LOGI(TAG, "TODO Manager init done");

    /* 12b. Weather cache — must be loaded BEFORE ui_init_all(), which builds
     * the weather page. Cold boot has no WiFi yet, so only cached data. */
    WeatherCache_Init();
    s_weather_has_cached = WeatherCache_Load(&s_weather_snapshot);
    WeatherFetch_Init();
    ESP_LOGI(TAG, "Weather cache: %s", s_weather_has_cached ? "loaded" : "empty");

    /* Every sub-page: one row each in ui_init_all()'s table — init under the
     * LVGL lock + back callback wired to ui_back_to_dashboard(). No WiFi on
     * this path, so sys params are right. */
    ui_init_all();
    TimerEngine_SetFiredCallback(timer_fired_cb);
    ESP_LOGI(TAG, "Sub-pages init done");

    /* Note: ui_update_todo_summary() lives on data_task's larger stack
     * (~3.7KB of locals) — calling it from app_main here would overflow
     * the main task stack and corrupt FreeRTOS internals. */

    /* 12c. Page-specific wiring that isn't a back button. */
    if (Lvgl_Lock(-1)) {
        WifiUi_SetToggleApCallback(wifi_toggle_ap_cb);
        WifiUi_SetReconnectCallback(wifi_reconnect_cb);
        WifiUi_SetRefreshWeatherCallback(weather_refresh_cb);
        if (s_weather_has_cached) {
            WeatherUI_Render(&s_weather_snapshot, true);
        }
        Lvgl_Unlock();
    }

    /* 13d/13e. Snake UI + App list UI —— 已并入 ui_init_all() 的页面注册表
     * （见 ui_screen.c），此处不再重复 init。 */

    /* 14. WiFi AP + Web Server
     * STA WiFi is initialized inline above (step 7). The HTTP server now
     * follows STA connectivity — data_task starts it once STA is up, so the
     * web API is reachable without long-pressing BOOT. The AP hotspot
     * remains BOOT-long-press-gated for on-device provisioning. */
    WifiAp_Init();
    TodoWeb_Init();
    s_wifi_lazy_inited = true;   /* STA already inited inline; AP/Web init done */
    s_web_want_running = true;   /* data_task will start the server when STA connects */
    ESP_LOGI(TAG, "WiFi AP + Web Server init done");

    /* 11. Data collection task (Core 0, priority 3).
     * MUST start after TimerEngine_Init — its tick=0 loop calls
     * TimerEngine_GetPomodoro(), which takes s_mutex; if the engine
     * isn't up yet s_mutex is NULL and xSemaphoreTake asserts.
     * Stack sized for mbedTLS HTTPS handshake (~30KB) inside WeatherFetch_Update. */
    xTaskCreatePinnedToCore(data_task, "data", 12 * 1024, nullptr, 3, &s_data_task_handle, 0);

    /* 15. BOOT key detection task */
    xTaskCreatePinnedToCore(boot_key_task, "boot_key", 4 * 1024, nullptr, 5, &s_boot_key_task_handle, 1);
    ESP_LOGI(TAG, "BOOT key task started");

    /* 16. PWR key double-click detection task */
    xTaskCreatePinnedToCore(pwr_key_task, "pwr_key", 3 * 1024, nullptr, 5, &s_pwr_key_task_handle, 1);
    ESP_LOGI(TAG, "PWR key task started");

    /* 17. Default screen: pet page (screen-saver style home) */
    if (Lvgl_Lock(-1)) {
        ui_switch_page(PAGE_PET);
        Lvgl_Unlock();
    }

    ESP_LOGI(TAG, "=== All tasks started ===");
}
