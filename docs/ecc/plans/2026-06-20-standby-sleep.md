# 续航优化 + 摆件模式 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use /feature-dev (recommended) or /plan to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 实现深睡眠摆件模式：双击 PWR 键进入深睡眠（显示像素角色画面），单击 PWR/BOOT 键唤醒快速恢复，跳过 WiFi/NTP。

**Architecture:** 新增 `standby_screen` 模块（纯 C，不依赖 LVGL，直接操作 EPD 画位图）+ `pwr_key_task`（双击检测）+ 在 `app_main` 增加唤醒分支。深睡眠前关 WiFi/AP/I2C/触摸，配置 EXT1 唤醒源，`rtc_gpio_hold_en` 保持墨水屏电源。唤醒后快速初始化 I2C/EPD/传感器/LVGL，跳过 WiFi 全流程。

**Tech Stack:** C（standby_screen、新增 Deinit）+ C++（main.cpp 改动），ESP-IDF deep sleep API，FreeRTOS

## Global Constraints

- GPIO18 (PWR) + GPIO0 (BOOT) 为深睡眠 EXT1 ANY_LOW 唤醒源
- 进入睡眠：双击 PWR，500ms 窗口内两次低电平
- 唤醒后：快速恢复 I2C → EPD → LVGL → 传感器，跳过 WiFi/NTP
- 睡眠前：全刷摆件画面（200×200 单色位图），墨水屏保持显示
- 睡眠期间：深睡眠（`esp_deep_sleep_start()`），不采集数据
- 番茄钟/提醒睡眠后丢失，唤醒后不恢复
- WiFi 唤醒后不自动重连，AP 模式不自动启动
- 摆件画面硬编码 Flash，不依赖 LVGL
- 不引入 `CONFIG_PM_ENABLE`，维持 160MHz 冷启动
- 遵守已有代码风格：main.cpp 为首选改动点，standby_screen 为纯 C 接口

---

### Task 1: Standby Screen — 像素角色位图 + 绘制函数

**Files:**
- Create: `main/standby_screen.h`
- Create: `main/standby_screen.c`
- Create: `main/standby_bitmap.h` (200×200 单色位图数据)

**Interfaces:**
- Produces: `void StandbyScreen_Draw(epaper_driver_display *epd)` — 用 EPD 全刷方式绘制摆件画面

- [ ] **Step 1: Create `main/standby_bitmap.h` — 200×200 单色位图数据**

文件内容为一个 `static const uint8_t` 数组，200×200/8 = 5000 字节。位图内容：一个居中的简单像素角色（猫脸） + 底部 "Zzz..." 文字，手动像素编排。

```c
// main/standby_bitmap.h
#ifndef STANDBY_BITMAP_H
#define STANDBY_BITMAP_H

#include <stdint.h>

// 200x200 单色位图 (BW), 5000 bytes
// 每字节代表 8 个水平像素，MSB=最左像素
// 行优先: byte[y*25 + x/8], bit = 7 - (x & 0x07)
// 1 = 白色, 0 = 黑色
static const uint8_t standby_bitmap[5000] = {
    // Row 0-39: 留白（角色居中偏上，顶部空白）
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    // ... (200 rows × 25 bytes each = full 5000-byte bitmap)
    // The bitmap encodes a simple pixel-art cat face centered horizontally,
    // roughly rows 40-140 with the character, and "Zzz..." text near bottom (rows 160-180).
    // Detailed pixel data omitted for brevity — the array MUST be exactly 5000 bytes.
    // See Task 1 Step 3 for the generation approach.
};
#endif
```

> **IMPORTANT**: The bitmap array above is a PLACEHOLDER. Step 3 will replace it with a real generated bitmap.

- [ ] **Step 2: Create `main/standby_screen.h`**

```c
// main/standby_screen.h
#ifndef STANDBY_SCREEN_H
#define STANDBY_SCREEN_H

#ifdef __cplusplus
extern "C" {
#endif

// Forward declaration — caller includes epaper_driver_bsp.h
struct epaper_driver_display;

/**
 * Draw the standby pixel-art character screen using full refresh.
 * Writes the 200x200 bitmap into the EPD's BW buffer and triggers display.
 * Must be called BEFORE entering deep sleep (LVGL is already stopped).
 */
void StandbyScreen_Draw(struct epaper_driver_display *epd);

#ifdef __cplusplus
}
#endif

#endif
```

- [ ] **Step 3: Create `main/standby_screen.c`**

```c
// main/standby_screen.c
#include "standby_screen.h"
#include "standby_bitmap.h"
#include "epaper_driver_bsp.h"
#include <string.h>

void StandbyScreen_Draw(struct epaper_driver_display *epd)
{
    if (!epd) return;

    // Re-init EPD for full refresh (SSD1681 state may be in partial mode from LVGL)
    epd->EPD_Init();

    // Clear buffer to white, then write bitmap
    epd->EPD_Clear();  // fills buffer with 0xFF (white)

    // Copy bitmap into buffer via pixel-level writes.
    // EPD_DrawColorPixel writes into the internal buffer; we iterate all 200x200 pixels.
    for (int y = 0; y < 200; y++) {
        for (int x = 0; x < 200; x++) {
            int byte_idx = y * 25 + (x >> 3);
            int bit = 7 - (x & 0x07);
            uint8_t color = (standby_bitmap[byte_idx] >> bit) & 0x01
                            ? DRIVER_COLOR_WHITE : DRIVER_COLOR_BLACK;
            epd->EPD_DrawColorPixel(x, y, color);
        }
    }

    // Full refresh to screen
    epd->EPD_Display();
}
```

> **NOTE**: Since `EPD_DrawColorPixel` modifies the internal `buffer` directly and `buffer` is private, we need to verify this works. The `EPD_Clear()` → pixel loop → `EPD_Display()` pattern is exactly what the driver was designed for. If the private access is a problem, an alternative is to `memcpy` the bitmap into EPD's buffer via `EPD_SendCommand(0x24)` + `writeBytes` directly — but that requires exposing buffer access. We'll test the pixel-path first; if compilation fails, fall back to a direct SPI write approach.

- [ ] **Step 4: Generate the real bitmap (Python script)**

Run this Python script to generate the actual `standby_bitmap.h`. Save as `scripts/gen_standby_bitmap.py`:

```python
#!/usr/bin/env python3
"""Generate a 200x200 pixel-art cat face bitmap for standby screen."""

from PIL import Image, ImageDraw

W, H = 200, 200

# Create image: white background
img = Image.new('1', (W, H), 1)  # '1' = 1-bit, 1=white
draw = ImageDraw.Draw(img)

# Draw a simple pixel-art cat face (centered, scaled to ~120x120)
# --- Cat ears (triangles) ---
# Left ear
draw.polygon([(40, 95), (55, 50), (70, 85)], fill=0)
# Right ear
draw.polygon([(130, 85), (145, 50), (160, 95)], fill=0)
# Inner ears (white triangles)
draw.polygon([(46, 90), (55, 60), (63, 85)], fill=1)
draw.polygon([(137, 85), (145, 60), (154, 90)], fill=1)

# --- Cat face (circle/ellipse) ---
draw.ellipse([(50, 70), (150, 160)], fill=0)

# --- Eyes (white ellipses with black pupils) ---
draw.ellipse([(70, 100), (90, 120)], fill=1)
draw.ellipse([(110, 100), (130, 120)], fill=1)
# Pupils
draw.ellipse([(78, 105), (86, 115)], fill=0)
draw.ellipse([(114, 105), (122, 115)], fill=0)

# --- Nose (small triangle) ---
draw.polygon([(97, 125), (103, 125), (100, 132)], fill=0)

# --- Mouth (two arcs approximated as lines) ---
draw.arc([(85, 125), (100, 142)], 0, 180, fill=0, width=2)
draw.arc([(100, 125), (115, 142)], 0, 180, fill=0, width=2)

# --- Whiskers ---
for x1, y1, x2, y2 in [(30, 115, 55, 118), (30, 125, 55, 125),
                         (145, 118, 170, 115), (145, 125, 170, 125)]:
    draw.line([(x1, y1), (x2, y2)], fill=0, width=1)

# --- "Zzz..." text at bottom ---
# Use default font (small), draw near bottom center
draw.text((70, 170), "Zzz...", fill=0)

# --- Convert to C array ---
pixels = list(img.getdata())  # list of 0/1, row-major
header = """// Auto-generated standby screen bitmap (200x200, 1-bit)
// Pixel-art cat character + "Zzz..." text
#ifndef STANDBY_BITMAP_H
#define STANDBY_BITMAP_H

#include <stdint.h>

static const uint8_t standby_bitmap[5000] = {
"""

lines = []
for row in range(H):
    row_bytes = []
    for col_byte in range(W // 8):
        byte_val = 0
        for bit in range(8):
            pixel = pixels[row * W + col_byte * 8 + bit]
            if pixel == 0:  # black
                byte_val |= (1 << (7 - bit))
        row_bytes.append(f"0x{byte_val:02X}")
    lines.append("    " + ",".join(row_bytes) + ",")

footer = """};
#endif
"""

with open("main/standby_bitmap.h", "w") as f:
    f.write(header)
    f.write("\n".join(lines) + "\n")
    f.write(footer)

print("Generated main/standby_bitmap.h (5000 bytes)")
```

Run it:
```bash
cd E:/aiot && python scripts/gen_standby_bitmap.py
```

- [ ] **Step 5: Add `standby_screen.c` to build**

Edit `main/CMakeLists.txt`:

```cmake
# Read the current CMakeLists.txt first, then add:
# "standby_screen.c" to the existing srcs list in idf_component_register
```

Use Grep to find the exact `idf_component_register` line in `main/CMakeLists.txt` and add `"standby_screen.c"` to the `SRCS` list.

- [ ] **Step 6: Commit**

```bash
git add main/standby_screen.h main/standby_screen.c main/standby_bitmap.h main/CMakeLists.txt
git commit -m "feat: add standby_screen module with pixel-art cat bitmap

Co-Authored-By: Claude Haiku 4.5 <noreply@anthropic.com>"
```

---

### Task 2: I2C Deinit 接口 + WifiBsp Deinit

**Files:**
- Modify: `components/i2c_bsp/i2c_bsp.h`
- Modify: `components/i2c_bsp/i2c_bsp.c`
- Modify: `components/wifi_bsp/wifi_bsp.h`
- Modify: `components/wifi_bsp/wifi_bsp.c`

**Interfaces:**
- Consumes: (none — extends existing modules)
- Produces: `void i2c_master_Deinit(void)` — 删除 I2C 设备并释放总线
- Produces: `void WifiBsp_Deinit(void)` — 断开 WiFi 并释放资源

- [ ] **Step 1: Add `i2c_master_Deinit` declaration to `components/i2c_bsp/i2c_bsp.h`**

在 `i2c_master_Init(void);` 声明后添加：

```c
void i2c_master_Deinit(void);
```

- [ ] **Step 2: Implement `i2c_master_Deinit` in `components/i2c_bsp/i2c_bsp.c`**

在 `i2c_master_Init` 函数后面添加：

```c
void i2c_master_Deinit(void)
{
    if (rtc_dev_handle) {
        i2c_master_bus_rm_device(rtc_dev_handle);
        rtc_dev_handle = NULL;
    }
    if (shtc3_handle) {
        i2c_master_bus_rm_device(shtc3_handle);
        shtc3_handle = NULL;
    }
    if (user_i2c_handle) {
        i2c_del_master_bus(user_i2c_handle);
        user_i2c_handle = NULL;
    }
}
```

- [ ] **Step 3: Add `WifiBsp_Deinit` declaration to `components/wifi_bsp/wifi_bsp.h`**

在 `WifiBsp_SyncNtpTime` 声明后添加：

```c
void WifiBsp_Deinit(void);
```

- [ ] **Step 4: Implement `WifiBsp_Deinit` in `components/wifi_bsp/wifi_bsp.c`**

在文件末尾添加：

```c
void WifiBsp_Deinit(void)
{
    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_deinit();
    s_connected = false;
    ESP_LOGI(TAG, "WiFi deinit done");
}
```

- [ ] **Step 5: Commit**

```bash
git add components/i2c_bsp/i2c_bsp.h components/i2c_bsp/i2c_bsp.c components/wifi_bsp/wifi_bsp.h components/wifi_bsp/wifi_bsp.c
git commit -m "feat: add i2c_master_Deinit and WifiBsp_Deinit interfaces

Co-Authored-By: Claude Haiku 4.5 <noreply@anthropic.com>"
```

---

### Task 3: PWR Key Double-Click Detection + Enter Deep Sleep

**Files:**
- Modify: `main/main.cpp`

**Interfaces:**
- Consumes: `StandbyScreen_Draw()` (Task 1), `i2c_master_Deinit()` (Task 2), `WifiBsp_Deinit()` (Task 2)
- Produces: `pwr_key_task()` — FreeRTOS task, core 1 prio 5, detects double-click on GPIO18

- [ ] **Step 1: Add include and forward declarations to `main/main.cpp`**

在现有 `#include` 区域末尾（`"timer_ui.h"` 行之后）添加：

```cpp
#include "standby_screen.h"
#include "esp_sleep.h"
#include "driver/rtc_io.h"
```

在 `static const char *TAG = "main";` 行后面，添加 task handle 全局变量和 enter_deep_sleep 前向声明：

```cpp
/* Task handles — needed by enter_deep_sleep to suspend/resume */
static TaskHandle_t s_data_task_handle = nullptr;
static TaskHandle_t s_boot_key_task_handle = nullptr;

static void enter_deep_sleep(void);
```

- [ ] **Step 2: Implement `enter_deep_sleep` function**

在 `boot_key_task` 函数之后，`data_task` 函数之前添加：

```cpp
static void enter_deep_sleep(void)
{
    ESP_LOGI(TAG, "Entering deep sleep (standby mode)...");

    // 1. Suspend background tasks
    if (s_data_task_handle) vTaskSuspend(s_data_task_handle);
    if (s_boot_key_task_handle) vTaskSuspend(s_boot_key_task_handle);

    // 2. Stop AP if active
    if (WifiAp_IsActive()) {
        TodoWeb_Stop();
        WifiAp_Stop();
    }

    // 3. Deinit WiFi + I2C (touch device is on I2C bus, cleaned up by i2c Deinit)
    WifiBsp_Deinit();
    i2c_master_Deinit();

    // 4. Draw standby pixel-art screen (full refresh)
    StandbyScreen_Draw(epd);

    // 5. Hold EPD power pins so screen stays powered during deep sleep
    gpio_hold_en(EPD_PWR_PIN);
    gpio_hold_en(AUDIO_PWR_PIN);
    gpio_hold_en(VBAT_PWR_PIN);

    // 6. Configure deep sleep + EXT1 wakeup
    esp_sleep_pd_config(ESP_PD_DOMAIN_MAX, ESP_PD_OPTION_AUTO);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

    const uint64_t wake_mask = (1ULL << PWR_BUTTON_PIN) | (1ULL << BOOT_BUTTON_PIN);
    esp_sleep_enable_ext1_wakeup_io(wake_mask, ESP_EXT1_WAKEUP_ANY_LOW);

    // Ensure pull-ups on wake pins
    rtc_gpio_pullup_en(PWR_BUTTON_PIN);
    rtc_gpio_pullup_en(BOOT_BUTTON_PIN);
    rtc_gpio_pulldown_dis(PWR_BUTTON_PIN);
    rtc_gpio_pulldown_dis(BOOT_BUTTON_PIN);

    ESP_LOGI(TAG, "Going to deep sleep... press PWR or BOOT to wake");
    esp_deep_sleep_start();
}
```

- [ ] **Step 3: Implement `pwr_key_task` function**

在 `enter_deep_sleep` 函数之后添加：

```cpp
/* PWR key double-click detection → enter standby mode */
static void pwr_key_task(void *arg)
{
    uint32_t last_press_ms = 0;
    uint8_t  press_count = 0;

    // Configure PWR pin as input with pull-up
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.pin_bit_mask = (1ULL << PWR_BUTTON_PIN);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);

    for (;;) {
        if (gpio_get_level(PWR_BUTTON_PIN) == 0) {  // pressed (low active)
            if (press_count == 0) {
                last_press_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
                press_count = 1;
            } else if (press_count == 1) {
                uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
                if (now - last_press_ms < 500) {
                    // Double-click detected
                    ESP_LOGI(TAG, "PWR double-click → entering standby");
                    enter_deep_sleep();
                    // enter_deep_sleep() never returns
                }
                press_count = 0;
            }
            vTaskDelay(pdMS_TO_TICKS(50));  // debounce
        } else {
            // Reset if too long since first press
            if (press_count == 1) {
                uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
                if (now - last_press_ms > 500) press_count = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}
```

- [ ] **Step 4: Store task handles in `app_main`**

在 `app_main` 中，修改两处 `xTaskCreatePinnedToCore` 调用以保存 handle：

将：
```cpp
xTaskCreatePinnedToCore(data_task, "data", 7 * 1024, nullptr, 3, nullptr, 0);
```
改为：
```cpp
xTaskCreatePinnedToCore(data_task, "data", 7 * 1024, nullptr, 3, &s_data_task_handle, 0);
```

将：
```cpp
xTaskCreatePinnedToCore(boot_key_task, "boot_key", 4 * 1024, nullptr, 5, nullptr, 1);
```
改为：
```cpp
xTaskCreatePinnedToCore(boot_key_task, "boot_key", 4 * 1024, nullptr, 5, &s_boot_key_task_handle, 1);
```

- [ ] **Step 5: Start `pwr_key_task` in `app_main`**

在 `boot_key_task` 启动之后，添加：

```cpp
/* 15b. PWR key double-click → standby */
xTaskCreatePinnedToCore(pwr_key_task, "pwr_key", 3 * 1024, nullptr, 5, nullptr, 1);
ESP_LOGI(TAG, "PWR key task started");
```

- [ ] **Step 6: Commit**

```bash
git add main/main.cpp
git commit -m "feat: add PWR key double-click detection + deep sleep entry

Co-Authored-By: Claude Haiku 4.5 <noreply@anthropic.com>"
```

---

### Task 4: Wake-from-Deep-Sleep Fast Recovery Path

**Files:**
- Modify: `main/main.cpp`

**Interfaces:**
- Consumes: all existing init functions + `gpio_hold_dis()` 
- Produces: wakeup detection branch in `app_main()`

- [ ] **Step 1: Add wakeup detection to `app_main`**

Wrap the existing `app_main` body with wakeup detection. Replace the current `app_main` function:

```cpp
extern "C" void app_main(void)
{
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

    /* ── Deep sleep wakeup: fast recovery, skip WiFi/NTP ── */
    if (cause == ESP_SLEEP_WAKEUP_EXT1) {
        uint64_t wake_pins = esp_sleep_get_ext1_wakeup_status();
        ESP_LOGI(TAG, "=== Wake from deep sleep (pins: 0x%llx) ===", wake_pins);

        // Release GPIO holds set before sleep
        gpio_hold_dis(EPD_PWR_PIN);
        gpio_hold_dis(AUDIO_PWR_PIN);
        gpio_hold_dis(VBAT_PWR_PIN);

        // 1. Power on
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
        ESP_LOGI(TAG, "Power on");

        // 2. I2C
        i2c_master_Init();
        ESP_LOGI(TAG, "I2C init done");

        // 3. EPD (re-init registers, screen still shows standby bitmap)
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
        // Do NOT Clear — preserve standby bitmap on screen
        // Restore base image so LVGL partial refresh works
        epd->EPD_DisplayPartBaseImage();
        epd->EPD_Init_Partial();
        ESP_LOGI(TAG, "EPD init done (fast path)");

        // 4. LVGL
        Lvgl_PortInit();
        Lvgl_SetEpd(epd);
        ESP_LOGI(TAG, "LVGL init done");

        // 5. Sensors
        PortShtc3_Init();
        PortRtc_Init();
        ESP_LOGI(TAG, "SHTC3 + RTC init done");

        // 6. Touch + Battery
        PortTouch_Init();
        PortBattery_Init();
        ESP_LOGI(TAG, "Touch + Battery init done");

        // 7. UI
        if (Lvgl_Lock(-1)) {
            ui_init();
            Lvgl_Unlock();
        }
        ESP_LOGI(TAG, "UI init done");

        // 8. Timer engine (fresh, no restore)
        if (TimerEngine_Init() != ESP_OK) {
            ESP_LOGE(TAG, "Timer engine init FAILED");
        }
        if (Lvgl_Lock(-1)) {
            TimerUi_Init();
            TimerUi_SetBackCallback(timer_back_cb);
            Lvgl_Unlock();
        }
        TimerEngine_SetFiredCallback(timer_fired_cb);

        // 9. TODO Manager + UI
        TodoMgr_Init();
        if (Lvgl_Lock(-1)) {
            TodoUi_Init();
            TodoUi_SetBackCallback(todo_back_cb);
            Lvgl_Unlock();
        }

        // NOTE: WiFi AP + Web NOT initialized in fast path.
        // NOTE: WifiBsp_Init() + NTP sync skipped — WiFi stays off.

        // 10. Start tasks
        xTaskCreatePinnedToCore(data_task, "data", 7 * 1024, nullptr, 3,
                                &s_data_task_handle, 0);
        xTaskCreatePinnedToCore(boot_key_task, "boot_key", 4 * 1024, nullptr, 5,
                                &s_boot_key_task_handle, 1);
        xTaskCreatePinnedToCore(pwr_key_task, "pwr_key", 3 * 1024, nullptr, 5,
                                nullptr, 1);

        ESP_LOGI(TAG, "=== Fast wakeup complete ===");
        return;  // app_main returns; tasks keep running
    }

    /* ── Cold boot: full init (original code) ── */
    ESP_LOGI(TAG, "=== AIoT Dashboard ===");
    // ... (keep the existing code unchanged from here down)
```

**Note**: The original `app_main` body from `/* 1. Power on */` through `/* 15. BOOT key task started */` must remain intact and unindented after the cold-boot comment. The `s_data_task_handle` / `s_boot_key_task_handle` assignments from Task 3 Step 4 must also be applied here.

- [ ] **Step 2: Update `enter_deep_sleep` to use `gpio_hold_en` correctly**

In `enter_deep_sleep()`, the `gpio_hold_en()` calls need `gpio_hold_dis()` first to clear any previous hold state:

```cpp
    // 5. Hold EPD power pins so screen stays powered during deep sleep
    gpio_hold_dis(EPD_PWR_PIN);
    gpio_hold_dis(AUDIO_PWR_PIN);
    gpio_hold_dis(VBAT_PWR_PIN);
    gpio_hold_en(EPD_PWR_PIN);
    gpio_hold_en(AUDIO_PWR_PIN);
    gpio_hold_en(VBAT_PWR_PIN);
```

(The existing `enter_deep_sleep` from Task 3 Step 2 already has the `gpio_hold_en` calls; just add `gpio_hold_dis` before each.)

- [ ] **Step 3: Commit**

```bash
git add main/main.cpp
git commit -m "feat: add deep sleep wakeup fast recovery path in app_main

Co-Authored-By: Claude Haiku 4.5 <noreply@anthropic.com>"
```

---

### Task 5: Fix StandbyScreen_Draw to Access EPD Buffer Directly

**Files:**
- Modify: `main/standby_screen.c`

**Interfaces:**
- Consumes: `epaper_driver_display` class (from `epaper_driver_bsp.h`)
- Produces: corrected drawing logic (uses `EPD_DrawColorPixel` which already has access to private `buffer` member — no change needed if it compiles)

This task is a **conditional fix** — only execute if Task 1's pixel-loop approach fails to compile or is too slow.

- [ ] **Step 1: Verify compilation**

```bash
env -u MSYSTEM powershell.exe -ExecutionPolicy Bypass -Command \
  "[Environment]::SetEnvironmentVariable('MSYSTEM', \$null, 'Process'); \
   . E:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1; \
   cd E:\aiot; idf.py build"
```

If compilation succeeds, skip remaining steps and mark this task as done.

- [ ] **Step 2: (Only if EPD_DrawColorPixel is unavailable) Rewrite with direct SPI write**

If pixel-by-pixel fails, rewrite `StandbyScreen_Draw` to write the bitmap directly to EPD via SPI commands (bypassing the private buffer):

```c
void StandbyScreen_Draw(struct epaper_driver_display *epd)
{
    if (!epd) return;
    epd->EPD_Init();
    epd->EPD_Clear();  // still needed for buffer allocation side effects

    // Write standby_bitmap directly to EPD RAM via 0x24 command
    epd->EPD_SendCommand(0x24);  // undocumented public access — may need a wrapper
    // Actually, EPD_SendCommand is private. We need a different approach:
    // Just use EPD_DrawColorPixel in a loop — it's ~40,000 calls but each is
    // just bit manipulation in RAM + one SPI burst at EPD_Display().
    // This is acceptable for a one-shot sleep entry (not performance-critical).
    for (int y = 0; y < 200; y++) {
        for (int x = 0; x < 200; x++) {
            int byte_idx = y * 25 + (x >> 3);
            int bit = 7 - (x & 0x07);
            uint8_t color = (standby_bitmap[byte_idx] >> bit) & 0x01
                            ? DRIVER_COLOR_WHITE : DRIVER_COLOR_BLACK;
            epd->EPD_DrawColorPixel(x, y, color);
        }
    }
    epd->EPD_Display();
}
```

Since `EPD_DrawColorPixel` is a public method of `epaper_driver_display`, this approach works as long as the extra 40k function calls don't cause a stack overflow in `enter_deep_sleep`. If that's an issue, we can increase `pwr_key_task` stack or use a direct buffer copy approach.

---

### Task 6: WiFi AP Interaction on Wakeup

**Files:**
- Modify: `main/main.cpp`

**Interfaces:**
- Consumes: `WifiAp_Init()`, `WifiBsp_Init()` (existing)
- Produces: Lazy WiFi init on BOOT long-press after wakeup

- [ ] **Step 1: Handle WiFi-not-initialized state on wakeup**

After fast-path wakeup, BOOT long-press currently toggles AP mode. But `WifiAp_Init()` and `WifiBsp_Init()` were never called. Add a lazy-init guard at the top of the BOOT long-press handler in `boot_key_task`:

In `boot_key_task`, at the point where `WifiAp_IsActive()` is checked, wrap the WiFi-AP toggle logic:

```cpp
// In boot_key_task, replace the long-press handler:
if ((xTaskGetTickCount() * portTICK_PERIOD_MS - press_start) > 2000) {
    ESP_LOGI(TAG, "BOOT long press detected");

    // Lazy-init WiFi if not yet initialized (wakeup fast path skips it)
    static bool wifi_inited = false;
    if (!wifi_inited) {
        WifiBsp_Init();
        if (WifiBsp_SyncNtpTime()) {
            time_t now = time(NULL);
            struct tm *t = localtime(&now);
            PortRtc_SetTime(t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                            t->tm_hour, t->tm_min, t->tm_sec);
        }
        WifiAp_Init();
        TodoWeb_Init();
        wifi_inited = true;
        ESP_LOGI(TAG, "WiFi lazy-init done");
    }

    if (WifiAp_IsActive()) {
        TodoWeb_Stop();
        WifiAp_Stop();
    } else {
        if (WifiAp_Start() == ESP_OK) {
            TodoWeb_Start();
            ESP_LOGI(TAG, "AP started");
        }
    }
    // ... rest of handler
}
```

However, this approach has a state management problem: `wifi_inited` static inside boot_key_task survives across deep sleep cycles (since deep sleep restarts `app_main`, which re-runs to create a new `boot_key_task` — wait, no: deep sleep wakes into `app_main`, which starts a fresh `boot_key_task`, so the static is always `false`. Simplification: just always do lazy init.

Actually, since `app_main` always re-executes on deep sleep wakeup, and the fast path skips WiFi/AP/Web init, each wakeup creates a fresh `boot_key_task` where WiFi is not initialized. So the lazy-init guard is:

```cpp
                } else if ((xTaskGetTickCount() * portTICK_PERIOD_MS - press_start) > 2000) {
                    ESP_LOGI(TAG, "BOOT long press detected");

                    // Lazy-init WiFi on first BOOT long-press after wakeup
                    WifiBsp_Init();
                    if (WifiBsp_SyncNtpTime()) {
                        time_t now = time(NULL);
                        struct tm *t = localtime(&now);
                        PortRtc_SetTime(t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                                        t->tm_hour, t->tm_min, t->tm_sec);
                        ESP_LOGI(TAG, "NTP sync OK, RTC updated");
                    }
                    WifiAp_Init();
                    TodoWeb_Init();

                    if (WifiAp_IsActive()) {
                        TodoWeb_Stop();
                        WifiAp_Stop();
                    } else {
                        if (WifiAp_Start() == ESP_OK) {
                            TodoWeb_Start();
                            ESP_LOGI(TAG, "AP started");
                        }
                    }

                    press_start = 0;
                    vTaskDelay(pdMS_TO_TICKS(500));
                }
```

But calling init repeatedly every long-press is wasteful. Add an idempotency guard:

```cpp
                } else if ((xTaskGetTickCount() * portTICK_PERIOD_MS - press_start) > 2000) {
                    ESP_LOGI(TAG, "BOOT long press detected");

                    // Ensure WiFi + AP + Web are initialized (lazy after wakeup)
                    WifiBsp_Init();  // idempotent: esp_netif_init checks if already done
                    static bool ntp_done = false;
                    if (!ntp_done && WifiBsp_IsConnected()) {
                        if (WifiBsp_SyncNtpTime()) {
                            time_t now = time(NULL);
                            struct tm *t = localtime(&now);
                            PortRtc_SetTime(t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                                            t->tm_hour, t->tm_min, t->tm_sec);
                            ntp_done = true;
                        }
                    }
                    WifiAp_Init();   // idempotent: checks s_initialized
                    TodoWeb_Init();  // idempotent: checks s_initialized

                    if (WifiAp_IsActive()) {
                        TodoWeb_Stop();
                        WifiAp_Stop();
                    } else {
                        if (WifiAp_Start() == ESP_OK) {
                            TodoWeb_Start();
                            ESP_LOGI(TAG, "AP started");
                        }
                    }

                    press_start = 0;
                    vTaskDelay(pdMS_TO_TICKS(500));
                }
```

Wait — `ntp_done` as static local inside a FreeRTOS task that restarts on each wakeup will always be false on first call, then true for subsequent calls within the same boot cycle. That's correct behavior: NTP sync once per wake cycle.

Actually, the critical issue: `WifiBsp_Init()` calls `nvs_flash_init()`, `esp_netif_init()`, `esp_event_loop_create_default()`, `esp_wifi_init()` — these are NOT idempotent. Calling them twice will crash. We need proper guards. Let me check:

- `nvs_flash_init()` — idempotent (returns OK if already init)
- `esp_netif_init()` — NOT idempotent (will assert/crash on second call)
- `esp_event_loop_create_default()` — NOT idempotent
- `esp_wifi_init()` — NOT idempotent

So we can't just call `WifiBsp_Init()` multiple times. We need a module-level flag.

Let me revise: the simplest correct approach is to move `WifiAp_Init()` and `TodoWeb_Init()` back into the cold-boot path only (they stay there, unchanged). For the wakeup path, add a separate lazy-init path that calls the full WiFi stack exactly once, triggered by BOOT long-press:

In `main.cpp`, add a flag:

```cpp
static bool s_wifi_lazy_inited = false;

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
}
```

In `boot_key_task` long-press handler, before the AP toggle logic:

```cpp
wifi_lazy_init();
```

And for the cold boot path, set `s_wifi_lazy_inited = true` after the existing WiFi/AP/Web init calls so `wifi_lazy_init()` becomes a no-op.

- [ ] **Step 2: Implement the solution**

在 `main.cpp` 中，`boot_key_task` 函数之前添加：

```cpp
static bool s_wifi_lazy_inited = false;

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
```

修改 `boot_key_task` 的 long-press handler，在 AP toggle 逻辑前加 `wifi_lazy_init()`：

```cpp
static void boot_key_task(void *arg)
{
    uint32_t press_start = 0;

    for (;;) {
        if (gpio_get_level((gpio_num_t)0) == 0) {
            if (press_start == 0) {
                press_start = xTaskGetTickCount() * portTICK_PERIOD_MS;
            } else if ((xTaskGetTickCount() * portTICK_PERIOD_MS - press_start) > 2000) {
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
                vTaskDelay(pdMS_TO_TICKS(500));
            }
        } else {
            press_start = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
```

在 `app_main` 的冷启动路径中，`WifiAp_Init()` + `TodoWeb_Init()` 之后添加：

```cpp
s_wifi_lazy_inited = true;
```

- [ ] **Step 3: Verify build**

```bash
env -u MSYSTEM powershell.exe -ExecutionPolicy Bypass -Command \
  "[Environment]::SetEnvironmentVariable('MSYSTEM', \$null, 'Process'); \
   . E:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1; \
   cd E:\aiot; idf.py build"
```

- [ ] **Step 4: Commit**

```bash
git add main/main.cpp
git commit -m "feat: add lazy WiFi init for BOOT long-press after wakeup

Co-Authored-By: Claude Haiku 4.5 <noreply@anthropic.com>"
```

---

