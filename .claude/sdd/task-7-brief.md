## Task 7: main.cpp 集成

**目标:** 组装所有组件，添加 BOOT 键长按检测

**Files:**
- Modify: `main/main.cpp`
- Modify: `sdkconfig.defaults`

---

### Step 1: 修改 sdkconfig.defaults

在文件末尾添加：

```
# WiFi AP Support
CONFIG_ESP_WIFI_SOFTAP_SUPPORT=y

# HTTP Server
CONFIG_HTTPD_MAX_REQ_HDR_LEN=512
CONFIG_HTTPD_MAX_URI_LEN=512
```

---

### Step 2: 修改 main.cpp 添加头文件

在现有 include 之后添加：

```cpp
#include "todo_manager.h"
#include "wifi_ap.h"
#include "todo_web.h"
#include "todo_ui.h"
```

---

### Step 3: 添加 BOOT 键检测任务

在 `data_task` 之前添加：

```cpp
/* BOOT key long-press detection */
static void boot_key_task(void *arg)
{
    uint32_t press_start = 0;

    for (;;) {
        if (gpio_get_level(0) == 0) {  // BOOT pressed (low active)
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
```

---

### Step 4: 修改 app_main 初始化新组件

在现有初始化之后（Touch + Battery 之后）添加：

```cpp
/* 12. TODO Manager */
TodoMgr_Init();
ESP_LOGI(TAG, "TODO Manager init done");

/* 13. TODO UI */
if (Lvgl_Lock(-1)) {
    TodoUi_Init(lv_screen_active());
    Lvgl_Unlock();
}
ESP_LOGI(TAG, "TODO UI init done");

/* 14. WiFi AP + Web Server */
WifiAp_Init();
TodoWeb_Init();
ESP_LOGI(TAG, "WiFi AP + Web Server init done");

/* 15. BOOT key detection task */
xTaskCreatePinnedToCore(boot_key_task, "boot_key", 4 * 1024, nullptr, 5, nullptr, 1);
ESP_LOGI(TAG, "BOOT key task started");
```

---

### Step 5: 修改 data_task 添加 TODO 刷新

在 Battery 更新之后添加：

```cpp
/* TODO UI refresh — every 30 s */
if (tick % 30 == 0) {
    if (Lvgl_Lock(500)) {
        TodoUi_Refresh();
        Lvgl_Unlock();
    }
}
```

---

### Step 6: 验证编译

```bash
cd E:/aiot
idf.py build
```

**预期:** 编译通过，无错误

---

### Step 7: 提交

```bash
git add main/main.cpp sdkconfig.defaults
git commit -m "feat: integrate todo components in main.cpp with BOOT key"
```
