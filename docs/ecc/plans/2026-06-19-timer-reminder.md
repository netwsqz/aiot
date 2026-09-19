# 番茄钟 + 定时提醒 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use /feature-dev (recommended) or /plan to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 ESP32-S3 1.54" 墨水屏固件上新增番茄钟（单次，15/25/50 分钟）与定时提醒（5/10/15/30/45/60 分钟，最多 3 个并发），到点满屏提示，点屏幕任意处解除。

**Architecture:** 新增 `timer_engine`（纯 C 计时引擎，无 LVGL 依赖，FreeRTOS 软件定时器每秒 tick）与 `timer_ui`（4 个页面 + 满屏提醒 + 队列）。`ui` 模块加左上入口与右下迷你显示。`main.cpp` 负责接线与每秒驱动。切分与现有 `todo_manager`/`todo_ui` 一致。

**Tech Stack:** ESP-IDF v5.5.4、LVGL v9（`lv_screen_load`/`lv_screen_active` API）、FreeRTOS（xTimer + xQueue + 互斥锁）、PCF85063 RTC（仅满屏提醒页读当前时间）。

## Global Constraints

- 构建命令（Claude Code Bash 工具，必须去 MSYSTEM）：
  ```bash
  env -u MSYSTEM powershell.exe -ExecutionPolicy Bypass -Command \
    "[Environment]::SetEnvironmentVariable('MSYSTEM', \$null, 'Process'); \
     . E:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1; \
     cd E:\aiot; idf.py build"
  ```
- 烧录+监视：把末尾 `idf.py build` 换成 `idf.py -p COM5 flash monitor`（COM 口号按实际）。
- 用户在 Windows Terminal 直接用 PowerShell：`. E:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1` 后 `idf.py build`。
- **字体约束**：项目仅 `lv_font_chinese_14` 一个中文字体（含 ASCII 数字/字母/括号，见 `todo_ui.cpp` 已用），无 emoji 字体。设计稿的 🍅/⏰ emoji 全部改用 `LV_SYMBOL_BELL` + 中文文字。
- **LVGL v9 API**：用 `lv_screen_load`/`lv_screen_active`/`lv_obj_clean`/`lv_obj_add_flag`。避免 `lv_obj_delete`（v9.0 vs 9.1 命名歧义）——确认面板用"创建一次 + 显隐"而非删除。
- 计时精度由 `timer_engine` 的 1s xTimer 保证；UI 显示每分钟刷新一次（省屏）。到点提醒每秒轮询队列，最迟 ~1s 内弹出。
- 不持久化、不睡眠、不用 LED/声音（仅墨水屏视觉）。
- 工作树当前有未提交改动（`ui.h`/`ui_screen.c`/`ui/CMakeLists.txt`/`main.cpp`/`todo_ui.cpp`）。开始前若需干净基线可自行 commit/stash；本计划在这些文件上的修改基于其当前内容增量进行。
- 测试策略：无主机单测基础设施（设计文档已决定跳过）。每个 Task 的验证 = `idf.py build` 通过 + 真机手动清单。到点路径测试用"临时把预设改为 1 分钟"避免长等。
- commit message 末尾必须带 `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`。

## File Structure

**新建：**
- `components/timer_engine/timer_engine.h` — 引擎公开接口（枚举/结构/函数）
- `components/timer_engine/timer_engine.c` — 计时逻辑（xTimer tick + 互斥 + fired 回调）
- `components/timer_engine/CMakeLists.txt`
- `components/timer_ui/timer_ui.h` — UI 接口（页面枚举/函数）
- `components/timer_ui/timer_ui.c` — 4 页面 + 满屏提醒 + 队列 + 取消确认
- `components/timer_ui/CMakeLists.txt`

**修改：**
- `components/ui/ui.h` — `lv_ui` 加迷你显示控件、`ui_page_t` 加 3 个枚举、声明 `ui_update_timer_mini`
- `components/ui/ui_screen.c` — 左上点击入口、右下迷你显示控件、`ui_update_timer_mini`、`ui_switch_page` 加 timer 分支
- `components/ui/CMakeLists.txt` — `PRIV_REQUIRES` 加 `timer_ui timer_engine`
- `main/main.cpp` — 初始化、fired 回调、`data_task` 每秒驱动
- `main/CMakeLists.txt` — `PRIV_REQUIRES` 加 `timer_engine timer_ui`

**职责边界：**
- `timer_engine`：不知道 LVGL/UI。管理 4 个槽位（1 番茄钟 + 3 提醒），tick 推进，到点回调。
- `timer_ui`：不知道时间怎么走。接受 tick 通知与 fired 通知，渲染页面。导航自洽（用 `lv_screen_active()` 快照恢复，不依赖 `ui` 的页面枚举）。
- `ui`：仅加入口与迷你显示，把页面路由到 `timer_ui`。
- 依赖方向无环：`timer_ui → timer_engine`；`ui → timer_ui, timer_engine`；`main → 全部`。

---

## Task 1: timer_engine 计时引擎

**Files:**
- Create: `components/timer_engine/timer_engine.h`
- Create: `components/timer_engine/timer_engine.c`
- Create: `components/timer_engine/CMakeLists.txt`
- Modify: `main/main.cpp`（仅加 `TimerEngine_Init()` 调用 + 临时测试代码，测试后移除）
- Modify: `main/CMakeLists.txt`（加 `timer_engine`）

**Interfaces:**
- Produces（后续 Task 依赖的精确签名）:
  ```c
  typedef enum { TIMER_KIND_POMODORO = 0, TIMER_KIND_REMINDER = 1 } timer_kind_t;
  typedef struct { uint8_t id; timer_kind_t kind; uint32_t total_seconds; uint32_t remaining_sec; bool active; } timer_instance_t;
  typedef void (*timer_fired_cb_t)(const timer_instance_t *t);
  #define TIMER_MAX_SLOTS 4
  esp_err_t TimerEngine_Init(void);
  esp_err_t TimerEngine_StartPomodoro(uint32_t minutes, uint8_t *out_id);
  esp_err_t TimerEngine_StartReminder(uint32_t minutes, uint8_t *out_id);
  esp_err_t TimerEngine_Cancel(uint8_t id);
  esp_err_t TimerEngine_GetAll(timer_instance_t *out, uint8_t *count);
  const timer_instance_t *TimerEngine_GetPomodoro(void);  // NULL=无活跃番茄钟；返回内部静态快照指针
  void TimerEngine_SetFiredCallback(timer_fired_cb_t cb);
  ```
  - 错误码：`ESP_ERR_INVALID_ARG`（minutes=0 或 >180）、`ESP_ERR_NO_MEM`（槽位满）、`ESP_ERR_NOT_FOUND`（Cancel id 不存在）。
  - `fired_cb` 在 FreeRTOS timer 服务任务中执行，**不可直接操作 LVGL**。

- [ ] **Step 1: 创建 `components/timer_engine/timer_engine.h`**

```c
#ifndef TIMER_ENGINE_H
#define TIMER_ENGINE_H

#include <stdint.h>
#include <stdbool.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TIMER_MAX_SLOTS  4   /* 1 番茄钟 + 3 定时提醒 */

typedef enum {
    TIMER_KIND_POMODORO = 0,
    TIMER_KIND_REMINDER = 1,
} timer_kind_t;

typedef struct {
    uint8_t       id;             /* 单调递增，1..255；0 表示空槽 */
    timer_kind_t  kind;
    uint32_t      total_seconds;
    uint32_t      remaining_sec;
    bool          active;
} timer_instance_t;

typedef void (*timer_fired_cb_t)(const timer_instance_t *t);

esp_err_t TimerEngine_Init(void);

/* 启动番茄钟；全局仅 1 个，已在跑返回 ESP_ERR_NO_MEM */
esp_err_t TimerEngine_StartPomodoro(uint32_t minutes, uint8_t *out_id);

/* 启动定时提醒；最多 3 个并发，满返回 ESP_ERR_NO_MEM */
esp_err_t TimerEngine_StartReminder(uint32_t minutes, uint8_t *out_id);

/* 按 id 取消；不存在返回 ESP_ERR_NOT_FOUND */
esp_err_t TimerEngine_Cancel(uint8_t id);

/* 拷贝所有活跃计时器到 out[]，*count 为拷贝数量。out 至少 TIMER_MAX_SLOTS 个元素 */
esp_err_t TimerEngine_GetAll(timer_instance_t *out, uint8_t *count);

/* 返回活跃番茄钟的静态快照指针，无则 NULL。指针在下次调用前有效 */
const timer_instance_t *TimerEngine_GetPomodoro(void);

void TimerEngine_SetFiredCallback(timer_fired_cb_t cb);

#ifdef __cplusplus
}
#endif

#endif /* TIMER_ENGINE_H */
```

- [ ] **Step 2: 创建 `components/timer_engine/timer_engine.c`**

```c
#include "timer_engine.h"
#include <string.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/timers.h>

static const char *TAG = "timer_eng";

#define POMODORO_SLOT       0
#define REMINDER_SLOT_FIRST 1
#define MAX_REMINDERS       3
#define TICK_PERIOD_MS      1000

static timer_instance_t s_slots[TIMER_MAX_SLOTS];
static SemaphoreHandle_t s_mutex = NULL;
static TimerHandle_t     s_tick_timer = NULL;
static timer_fired_cb_t  s_fired_cb = NULL;
static uint8_t           s_next_id = 1;

/* tick 回调里收集到点的计时器，释放锁后再触发回调 */
static timer_instance_t s_fired_buf[TIMER_MAX_SLOTS];

static void tick_cb(TimerHandle_t tmr)
{
    (void)tmr;
    int fired_count = 0;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < TIMER_MAX_SLOTS; i++) {
        if (!s_slots[i].active) continue;
        if (s_slots[i].remaining_sec > 0) s_slots[i].remaining_sec--;
        if (s_slots[i].remaining_sec == 0) {
            s_slots[i].active = false;
            s_fired_buf[fired_count++] = s_slots[i];
        }
    }
    xSemaphoreGive(s_mutex);

    for (int i = 0; i < fired_count; i++) {
        if (s_fired_cb) s_fired_cb(&s_fired_buf[i]);
    }
}

esp_err_t TimerEngine_Init(void)
{
    if (s_mutex) return ESP_OK;  /* 已初始化 */

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_FAIL;

    memset(s_slots, 0, sizeof(s_slots));
    s_next_id = 1;

    s_tick_timer = xTimerCreate("timer_tick", pdMS_TO_TICKS(TICK_PERIOD_MS),
                                pdTRUE, NULL, tick_cb);
    if (!s_tick_timer) return ESP_FAIL;
    xTimerStart(s_tick_timer, 0);

    ESP_LOGI(TAG, "Init done, tick 1s");
    return ESP_OK;
}

static void fill_slot(int slot, timer_kind_t kind, uint32_t minutes, uint8_t *out_id)
{
    s_slots[slot].id            = s_next_id++;
    if (s_next_id == 0) s_next_id = 1;
    s_slots[slot].kind          = kind;
    s_slots[slot].total_seconds = minutes * 60;
    s_slots[slot].remaining_sec = minutes * 60;
    s_slots[slot].active        = true;
    if (out_id) *out_id = s_slots[slot].id;
}

esp_err_t TimerEngine_StartPomodoro(uint32_t minutes, uint8_t *out_id)
{
    if (minutes == 0 || minutes > 180) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_slots[POMODORO_SLOT].active) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }
    fill_slot(POMODORO_SLOT, TIMER_KIND_POMODORO, minutes, out_id);
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Pomodoro %lu min started (id=%u)",
             (unsigned long)minutes, out_id ? *out_id : 0);
    return ESP_OK;
}

esp_err_t TimerEngine_StartReminder(uint32_t minutes, uint8_t *out_id)
{
    if (minutes == 0 || minutes > 180) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int slot = -1;
    for (int i = REMINDER_SLOT_FIRST; i < REMINDER_SLOT_FIRST + MAX_REMINDERS; i++) {
        if (!s_slots[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }
    fill_slot(slot, TIMER_KIND_REMINDER, minutes, out_id);
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Reminder %lu min started (id=%u)",
             (unsigned long)minutes, out_id ? *out_id : 0);
    return ESP_OK;
}

esp_err_t TimerEngine_Cancel(uint8_t id)
{
    if (id == 0) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < TIMER_MAX_SLOTS; i++) {
        if (s_slots[i].active && s_slots[i].id == id) {
            s_slots[i].active = false;
            s_slots[i].id     = 0;
            xSemaphoreGive(s_mutex);
            ESP_LOGI(TAG, "Cancelled id=%u", id);
            return ESP_OK;
        }
    }
    xSemaphoreGive(s_mutex);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t TimerEngine_GetAll(timer_instance_t *out, uint8_t *count)
{
    if (!out || !count) return ESP_ERR_INVALID_ARG;
    uint8_t n = 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < TIMER_MAX_SLOTS; i++) {
        if (s_slots[i].active) out[n++] = s_slots[i];
    }
    xSemaphoreGive(s_mutex);
    *count = n;
    return ESP_OK;
}

const timer_instance_t *TimerEngine_GetPomodoro(void)
{
    static timer_instance_t snapshot;
    bool active;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    active = s_slots[POMODORO_SLOT].active;
    if (active) snapshot = s_slots[POMODORO_SLOT];
    xSemaphoreGive(s_mutex);
    return active ? &snapshot : NULL;
}
```

- [ ] **Step 3: 创建 `components/timer_engine/CMakeLists.txt`**

```cmake
idf_component_register(
    SRCS "timer_engine.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES freertos)
```

- [ ] **Step 4: 修改 `main/CMakeLists.txt`**，在 `PRIV_REQUIRES` 末尾加 `timer_engine`：

```cmake
idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES driver
    PRIV_REQUIRES epaper_driver_bsp lvgl_port ui i2c_bsp port_shtc3 port_rtc port_touch port_battery wifi_bsp todo_manager wifi_ap todo_web todo_ui timer_engine)
```

- [ ] **Step 5: 修改 `main/main.cpp`**，顶部 include 区加：

```cpp
#include "timer_engine.h"
```

在 `app_main` 中 `TodoWeb_Init();` 之后、`xTaskCreatePinnedToCore(boot_key_task...)` 之前加初始化：

```cpp
    /* 16. Timer engine */
    TimerEngine_Init();
    ESP_LOGI(TAG, "Timer engine init done");
```

- [ ] **Step 6: 临时验证到点路径（验证后移除）**

在 `app_main` 末尾（`=== All tasks started ===` 之后）临时加测试代码：

```cpp
    /* TEMP: verify timer fire path — remove before commit */
    TimerEngine_SetFiredCallback([](const timer_instance_t *t) {
        ESP_LOGI(TAG, "TEMP fired: kind=%d total=%lu", t->kind, (unsigned long)t->total_seconds);
    });
    TimerEngine_StartPomodoro(1, nullptr);
```

- [ ] **Step 7: 构建并烧录验证**

Run（构建）:
```bash
env -u MSYSTEM powershell.exe -ExecutionPolicy Bypass -Command "[Environment]::SetEnvironmentVariable('MSYSTEM', \$null, 'Process'); . E:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1; cd E:\aiot; idf.py build"
```
Expected: 构建成功，无错误。

烧录+监视（换实际 COM 口）:
```bash
env -u MSYSTEM powershell.exe -ExecutionPolicy Bypass -Command "[Environment]::SetEnvironmentVariable('MSYSTEM', \$null, 'Process'); . E:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1; cd E:\aiot; idf.py -p COM5 flash monitor"
```
Expected: 启动日志含 `timer_eng: Init done`、`Timer engine init done`、`Pomodoro 1 min started`；约 60 秒后出现 `TEMP fired: kind=0 total=60`。确认后按 Ctrl+] 退出 monitor。

- [ ] **Step 8: 移除临时测试代码**

删除 Step 6 加入的 `TEMP` 代码块（`TimerEngine_SetFiredCallback(...)` 与 `TimerEngine_StartPomodoro(1, nullptr)`）。保留 `TimerEngine_Init()` 与 include 与日志行。

- [ ] **Step 9: 重新构建确认干净**

Run:
```bash
env -u MSYSTEM powershell.exe -ExecutionPolicy Bypass -Command "[Environment]::SetEnvironmentVariable('MSYSTEM', \$null, 'Process'); . E:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1; cd E:\aiot; idf.py build"
```
Expected: 构建成功。

- [ ] **Step 10: Commit**

```bash
git add components/timer_engine main/main.cpp main/CMakeLists.txt
git commit -m "feat(timer): add timer_engine with pomodoro + reminder logic" -m "Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: timer_ui 骨架 + 时间工具页 + 左上入口

**Files:**
- Create: `components/timer_ui/timer_ui.h`
- Create: `components/timer_ui/timer_ui.c`（本 Task 只实现 tools 页 + 骨架，pomodoro/reminder 页留空函数）
- Create: `components/timer_ui/CMakeLists.txt`
- Modify: `components/ui/ui.h`
- Modify: `components/ui/ui_screen.c`
- Modify: `components/ui/CMakeLists.txt`
- Modify: `main/main.cpp`（加 `TimerUi_Init` + 返回回调）
- Modify: `main/CMakeLists.txt`（加 `timer_ui`）

**Interfaces:**
- Consumes: `TimerEngine_*`（本 Task 暂不调用，留到 Task 3）
- Produces（后续 Task 依赖）:
  ```c
  typedef enum { TIMER_PAGE_TOOLS = 0, TIMER_PAGE_POMODORO, TIMER_PAGE_REMINDER } timer_page_t;
  esp_err_t TimerUi_Init(void);
  lv_obj_t *TimerUi_GetScreen(void);
  void TimerUi_ShowPage(timer_page_t page);
  void TimerUi_SetBackCallback(void (*cb)(void));
  void TimerUi_PushFiredEvent(uint8_t id, timer_kind_t kind, uint32_t total_seconds);  /* Task 4 实现 */
  void TimerUi_TickUpdate(void);                                                       /* Task 4 实现 */
  ```

- [ ] **Step 1: 创建 `components/timer_ui/timer_ui.h`**

```c
#ifndef TIMER_UI_H
#define TIMER_UI_H

#include <lvgl.h>
#include <esp_err.h>
#include "timer_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TIMER_PAGE_TOOLS = 0,
    TIMER_PAGE_POMODORO,
    TIMER_PAGE_REMINDER,
} timer_page_t;

esp_err_t TimerUi_Init(void);

/* 获取 timer 屏幕（含 tools/pomodoro/reminder 子页面） */
lv_obj_t *TimerUi_GetScreen(void);

/* 切换 timer 屏幕内的子页面。须在 LVGL 锁内调用 */
void TimerUi_ShowPage(timer_page_t page);

/* 在 tools 页按返回时调用（→ 回主屏） */
void TimerUi_SetBackCallback(void (*cb)(void));

/* 由 engine fired_cb（timer 任务上下文）调用，仅入队 */
void TimerUi_PushFiredEvent(uint8_t id, timer_kind_t kind, uint32_t total_seconds);

/* 由 data_task 每秒在 LVGL 锁内调用：轮询到点队列 + 每分钟刷新可见页 */
void TimerUi_TickUpdate(void);

#ifdef __cplusplus
}
#endif

#endif /* TIMER_UI_H */
```

- [ ] **Step 2: 创建 `components/timer_ui/timer_ui.c`（骨架 + tools 页）**

```c
#include "timer_ui.h"
#include "timer_engine.h"
#include "port_rtc.h"
#include "font_cn.h"
#include <stdio.h>
#include <string.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

static const char *TAG = "timer_ui";

static lv_obj_t *s_timer_screen = NULL;
static lv_obj_t *s_tools_page   = NULL;
static lv_obj_t *s_pomo_page    = NULL;
static lv_obj_t *s_rem_page     = NULL;
static timer_page_t s_current_page = TIMER_PAGE_TOOLS;
static void (*s_back_cb)(void) = NULL;

/* ---- Pomodoro 页控件（Task 3 填充） ---- */
static lv_obj_t *s_pomo_idle_cont = NULL;
static lv_obj_t *s_pomo_run_cont  = NULL;
static lv_obj_t *s_pomo_min_label = NULL;
static lv_obj_t *s_pomo_bar       = NULL;

/* ---- Reminder 页控件（Task 5 填充） ---- */
static lv_obj_t *s_rem_title      = NULL;
static lv_obj_t *s_rem_btns[6]    = {0};
static lv_obj_t *s_rem_list_cont  = NULL;
static lv_obj_t *s_confirm_panel  = NULL;
static uint8_t   s_confirm_id     = 0;

/* ---- 满屏提醒（Task 4 填充） ---- */
typedef struct {
    uint8_t      id;
    timer_kind_t kind;
    uint32_t     total_seconds;
} fired_item_t;
static QueueHandle_t s_fired_queue        = NULL;
static lv_obj_t     *s_alert_screen       = NULL;
static lv_obj_t     *s_alert_type_lbl     = NULL;
static lv_obj_t     *s_alert_dur_lbl      = NULL;
static lv_obj_t     *s_alert_time_lbl     = NULL;
static lv_obj_t     *s_screen_before_alert = NULL;
static bool          s_alert_showing      = false;
static timer_kind_t  s_last_fired_kind    = TIMER_KIND_POMODORO;

static const uint32_t POMO_PRESETS[3] = {15, 25, 50};
static const uint32_t REM_PRESETS[6]  = {5, 10, 15, 30, 45, 60};

/* ============ 通用：返回按钮 + 手势（仿 todo_ui） ============ */
static void back_btn_cb(lv_event_t *e)
{
    (void)e;
    if (s_current_page == TIMER_PAGE_POMODORO || s_current_page == TIMER_PAGE_REMINDER)
        TimerUi_ShowPage(TIMER_PAGE_TOOLS);
    else if (s_back_cb) s_back_cb();
}

static void swipe_cb(lv_event_t *e)
{
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
    if (dir == LV_DIR_RIGHT) back_btn_cb(e);
}

static lv_obj_t *create_back_btn(lv_obj_t *parent)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 40, 32);
    lv_obj_set_pos(btn, 4, 4);
    lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_add_event_cb(btn, back_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(lbl);
    return btn;
}

static lv_obj_t *make_page(lv_obj_t *parent)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_size(p, 200, 200);
    lv_obj_set_pos(p, 0, 0);
    lv_obj_set_style_bg_color(p, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_radius(p, 0, 0);
    lv_obj_set_style_pad_all(p, 0, 0);
    lv_obj_set_scrollbar_mode(p, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(p, LV_OBJ_FLAG_GESTURE_BUBBLE);
    return p;
}

/* ============ tools 页 ============ */
static void tools_pomo_cb(lv_event_t *e) { (void)e; TimerUi_ShowPage(TIMER_PAGE_POMODORO); }
static void tools_rem_cb(lv_event_t *e)  { (void)e; TimerUi_ShowPage(TIMER_PAGE_REMINDER); }

static void create_tools_page(lv_obj_t *parent)
{
    s_tools_page = make_page(parent);
    create_back_btn(s_tools_page);

    lv_obj_t *title = lv_label_create(s_tools_page);
    lv_obj_set_pos(title, 48, 10);
    lv_label_set_text(title, "时间工具");
    lv_obj_set_style_text_font(title, &lv_font_chinese_14, 0);
    lv_obj_set_style_text_decor(title, LV_TEXT_DECOR_UNDERLINE, 0);

    const char *names[2] = {"番茄钟", "定时提醒"};
    lv_obj_t (*cbs[2])(lv_event_t *) = {tools_pomo_cb, tools_rem_cb};  /* 占位类型，实际用函数指针 */
    (void)cbs;
    void (*cb_arr[2])(lv_event_t *) = {tools_pomo_cb, tools_rem_cb};
    for (int i = 0; i < 2; i++) {
        lv_obj_t *btn = lv_btn_create(s_tools_page);
        lv_obj_set_size(btn, 180, 56);
        lv_obj_set_pos(btn, 10, 50 + i * 66);
        lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_radius(btn, 4, 0);
        lv_obj_add_event_cb(btn, cb_arr[i], LV_EVENT_CLICKED, NULL);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, names[i]);
        lv_obj_set_style_text_font(lbl, &lv_font_chinese_14, 0);
        lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
        lv_obj_center(lbl);
    }
}

/* ============ pomodoro / reminder 页（本 Task 占位，Task 3/5 实现） ============ */
static void create_pomo_page(lv_obj_t *parent)
{
    s_pomo_page = make_page(parent);
    create_back_btn(s_pomo_page);
    lv_obj_t *t = lv_label_create(s_pomo_page);
    lv_obj_set_pos(t, 48, 10);
    lv_label_set_text(t, "番茄钟");
    lv_obj_set_style_text_font(t, &lv_font_chinese_14, 0);
}

static void create_rem_page(lv_obj_t *parent)
{
    s_rem_page = make_page(parent);
    create_back_btn(s_rem_page);
    s_rem_title = lv_label_create(s_rem_page);
    lv_obj_set_pos(s_rem_title, 48, 10);
    lv_label_set_text(s_rem_title, "定时提醒");
    lv_obj_set_style_text_font(s_rem_title, &lv_font_chinese_14, 0);
}

static void refresh_pomodoro_page(void) { /* Task 3 */ }
static void refresh_reminder_page(void) { /* Task 5 */ }

/* ============ 公开 API ============ */
esp_err_t TimerUi_Init(void)
{
    s_fired_queue = xQueueCreate(4, sizeof(fired_item_t));
    if (!s_fired_queue) return ESP_FAIL;

    s_timer_screen = lv_obj_create(NULL);
    lv_obj_set_size(s_timer_screen, 200, 200);
    lv_obj_set_scrollbar_mode(s_timer_screen, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(s_timer_screen, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_timer_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_timer_screen, 0, 0);
    lv_obj_set_style_radius(s_timer_screen, 0, 0);
    lv_obj_set_style_pad_all(s_timer_screen, 0, 0);

    create_tools_page(s_timer_screen);
    create_pomo_page(s_timer_screen);
    create_rem_page(s_timer_screen);

    lv_obj_clear_flag(s_tools_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_pomo_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_rem_page, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_event_cb(s_timer_screen, swipe_cb, LV_EVENT_GESTURE, NULL);
    return ESP_OK;
}

lv_obj_t *TimerUi_GetScreen(void) { return s_timer_screen; }

void TimerUi_ShowPage(timer_page_t page)
{
    s_current_page = page;
    lv_obj_add_flag(s_tools_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_pomo_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_rem_page, LV_OBJ_FLAG_HIDDEN);
    if (page == TIMER_PAGE_TOOLS) {
        lv_obj_clear_flag(s_tools_page, LV_OBJ_FLAG_HIDDEN);
    } else if (page == TIMER_PAGE_POMODORO) {
        lv_obj_clear_flag(s_pomo_page, LV_OBJ_FLAG_HIDDEN);
        refresh_pomodoro_page();
    } else {
        lv_obj_clear_flag(s_rem_page, LV_OBJ_FLAG_HIDDEN);
        refresh_reminder_page();
    }
}

void TimerUi_SetBackCallback(void (*cb)(void)) { s_back_cb = cb; }

void TimerUi_PushFiredEvent(uint8_t id, timer_kind_t kind, uint32_t total_seconds)
{
    (void)id; (void)kind; (void)total_seconds;  /* Task 4 实现 */
}

void TimerUi_TickUpdate(void) { /* Task 4 实现 */ }
```

> 说明：`tools_pomo_cb`/`tools_rem_cb` 上方的 `cbs` 行是无用的占位（早期草稿残留），保留会被编译器警告。实现时**删掉** `const char *names...` 与 `lv_obj_t (*cbs[2])...; (void)cbs;` 这两行，只保留 `void (*cb_arr[2])...` 那行与下方循环。最终 create_tools_page 应只含 `names`、`cb_arr` 两个数组。下面 Step 3 给出干净的最终版本——以 Step 3 为准。

- [ ] **Step 3: 用干净版本替换 `create_tools_page`**

把 Step 2 中的 `create_tools_page` 整个函数体替换为（删除占位的 `cbs` 行）：

```c
static void create_tools_page(lv_obj_t *parent)
{
    s_tools_page = make_page(parent);
    create_back_btn(s_tools_page);

    lv_obj_t *title = lv_label_create(s_tools_page);
    lv_obj_set_pos(title, 48, 10);
    lv_label_set_text(title, "时间工具");
    lv_obj_set_style_text_font(title, &lv_font_chinese_14, 0);
    lv_obj_set_style_text_decor(title, LV_TEXT_DECOR_UNDERLINE, 0);

    const char *names[2] = {"番茄钟", "定时提醒"};
    void (*cb_arr[2])(lv_event_t *) = {tools_pomo_cb, tools_rem_cb};
    for (int i = 0; i < 2; i++) {
        lv_obj_t *btn = lv_btn_create(s_tools_page);
        lv_obj_set_size(btn, 180, 56);
        lv_obj_set_pos(btn, 10, 50 + i * 66);
        lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_radius(btn, 4, 0);
        lv_obj_add_event_cb(btn, cb_arr[i], LV_EVENT_CLICKED, NULL);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, names[i]);
        lv_obj_set_style_text_font(lbl, &lv_font_chinese_14, 0);
        lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
        lv_obj_center(lbl);
    }
}
```

- [ ] **Step 4: 创建 `components/timer_ui/CMakeLists.txt`**

```cmake
idf_component_register(
    SRCS "timer_ui.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES timer_engine lvgl font_cn port_rtc)
```

- [ ] **Step 5: 修改 `components/ui/CMakeLists.txt`**，加 `timer_ui timer_engine`：

```cmake
idf_component_register(
    SRCS "ui_screen.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES lvgl lvgl_port todo_ui todo_manager font_cn timer_ui timer_engine)
```

- [ ] **Step 6: 修改 `components/ui/ui.h`**

在 `ui_page_t` 枚举中加 3 个值（`PAGE_TODO_TASKS` 之后）：

```c
typedef enum {
    PAGE_DASHBOARD,
    PAGE_TODO_LISTS,
    PAGE_TODO_TASKS,
    PAGE_TIMER_TOOLS,
    PAGE_POMODORO,
    PAGE_REMINDER,
} ui_page_t;
```

在 `lv_ui` 结构体 `lbl_image_placeholder;` 之后加迷你显示控件：

```c
    /* Bottom-right: image placeholder */
    lv_obj_t *cont_bottom_right;
    lv_obj_t *lbl_image_placeholder;

    /* Bottom-right: pomodoro mini display (shown when a pomodoro is running) */
    lv_obj_t *cont_timer_mini;
    lv_obj_t *lbl_timer_mini_title;
    lv_obj_t *lbl_timer_mini_min;
    lv_obj_t *bar_timer_mini;
} lv_ui;
```

在 `ui_update_todo_summary` 声明之后、`/* ── Page management ── */` 之前加：

```c
void ui_update_timer_mini(bool running, uint32_t remaining_sec, uint32_t total_sec);
```

- [ ] **Step 7: 修改 `components/ui/ui_screen.c`**

顶部 include 区加：

```c
#include "timer_ui.h"
#include "timer_engine.h"
```

在 `static lv_obj_t *s_todo_screen = NULL;` 之后加：

```c
static lv_obj_t *s_timer_screen = NULL;
```

加左上点击回调（放在 `todo_quadrant_click_cb` 附近）：

```c
/* ── Top-left click → enter timer tools ── */
static void timer_entry_click_cb(lv_event_t *e)
{
    (void)e;
    ui_switch_page(PAGE_TIMER_TOOLS);
}
```

修改 `setup_top_left`，在函数末尾（`lbl_date` 设置之后、return 前）加可点击与事件：

```c
    /* Whole top-left quadrant is the entry to timer tools */
    lv_obj_add_flag(ui->cont_top_left, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ui->cont_top_left, timer_entry_click_cb, LV_EVENT_CLICKED, NULL);
```

修改 `setup_bottom_right`，在 `lbl_image_placeholder` 创建之后加迷你显示控件（初始隐藏）：

```c
    /* Pomodoro mini display — hidden until a pomodoro runs (Task 6 drives it) */
    ui->cont_timer_mini = lv_obj_create(ui->cont_bottom_right);
    lv_obj_set_size(ui->cont_timer_mini, 100, 100);
    lv_obj_set_pos(ui->cont_timer_mini, 0, 0);
    lv_obj_set_style_bg_opa(ui->cont_timer_mini, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ui->cont_timer_mini, 0, 0);
    lv_obj_set_style_pad_all(ui->cont_timer_mini, 0, 0);
    lv_obj_set_style_radius(ui->cont_timer_mini, 0, 0);
    lv_obj_set_scrollbar_mode(ui->cont_timer_mini, LV_SCROLLBAR_MODE_OFF);

    ui->lbl_timer_mini_title = lv_label_create(ui->cont_timer_mini);
    lv_obj_set_pos(ui->lbl_timer_mini_title, 0, 8);
    lv_obj_set_size(ui->lbl_timer_mini_title, 100, 16);
    lv_label_set_text(ui->lbl_timer_mini_title, "番茄");
    lv_obj_set_style_text_font(ui->lbl_timer_mini_title, &lv_font_chinese_14, 0);
    lv_obj_set_style_text_color(ui->lbl_timer_mini_title, lv_color_black(), 0);
    lv_obj_set_style_text_align(ui->lbl_timer_mini_title, LV_TEXT_ALIGN_CENTER, 0);

    ui->lbl_timer_mini_min = lv_label_create(ui->cont_timer_mini);
    lv_obj_set_pos(ui->lbl_timer_mini_min, 0, 28);
    lv_obj_set_size(ui->lbl_timer_mini_min, 100, 24);
    lv_label_set_text(ui->lbl_timer_mini_min, "");
    lv_obj_set_style_text_font(ui->lbl_timer_mini_min, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(ui->lbl_timer_mini_min, lv_color_black(), 0);
    lv_obj_set_style_text_align(ui->lbl_timer_mini_min, LV_TEXT_ALIGN_CENTER, 0);

    ui->bar_timer_mini = lv_bar_create(ui->cont_timer_mini);
    lv_obj_set_size(ui->bar_timer_mini, 80, 8);
    lv_obj_set_pos(ui->bar_timer_mini, 10, 64);
    lv_bar_set_range(ui->bar_timer_mini, 0, 100);
    lv_obj_set_style_bg_color(ui->bar_timer_mini, lv_color_white(), 0);
    lv_obj_set_style_border_width(ui->bar_timer_mini, 1, 0);
    lv_obj_set_style_border_color(ui->bar_timer_mini, lv_color_black(), 0);
    lv_obj_set_style_bg_color(ui->bar_timer_mini, lv_color_black(), LV_PART_INDICATOR);
    lv_obj_set_style_radius(ui->bar_timer_mini, 0, 0);
    lv_obj_set_style_radius(ui->bar_timer_mini, 0, LV_PART_INDICATOR);

    lv_obj_add_flag(ui->cont_timer_mini, LV_OBJ_FLAG_HIDDEN);
```

在 `ui_update_wifi_status` 函数之后加 `ui_update_timer_mini`（带自门控，避免每秒触发刷新）：

```c
/* ── Dashboard right-bottom pomodoro mini display ── */
void ui_update_timer_mini(bool running, uint32_t remaining_sec, uint32_t total_sec)
{
    static bool     last_running = false;
    static uint32_t last_min     = 0;
    static int      last_pct     = -1;

    if (running != last_running) {
        if (running) {
            lv_obj_clear_flag(ui.cont_timer_mini, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(ui.lbl_image_placeholder, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(ui.cont_timer_mini, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(ui.lbl_image_placeholder, LV_OBJ_FLAG_HIDDEN);
        }
        last_running = running;
        last_min = 0;
        last_pct = -1;
    }

    if (!running) return;

    uint32_t min = (remaining_sec + 59) / 60;
    int pct = (total_sec > 0) ? (int)(100 * (total_sec - remaining_sec) / total_sec) : 0;

    if (min != last_min) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%u", (unsigned)min);
        lv_label_set_text(ui.lbl_timer_mini_min, buf);
        last_min = min;
    }
    if (pct != last_pct) {
        lv_bar_set_value(ui.bar_timer_mini, pct, LV_ANIM_OFF);
        last_pct = pct;
    }
}
```

修改 `ui_switch_page`，在 `else if (page == PAGE_TODO_TASKS)` 块之后、函数结束前加 timer 分支：

```c
    else if (page == PAGE_TIMER_TOOLS || page == PAGE_POMODORO || page == PAGE_REMINDER) {
        if (!s_timer_screen) s_timer_screen = TimerUi_GetScreen();
        if (s_timer_screen) {
            timer_page_t tp = (page == PAGE_POMODORO) ? TIMER_PAGE_POMODORO :
                              (page == PAGE_REMINDER)  ? TIMER_PAGE_REMINDER :
                                                        TIMER_PAGE_TOOLS;
            TimerUi_ShowPage(tp);
            lv_screen_load(s_timer_screen);
        }
    }
```

- [ ] **Step 8: 修改 `main/main.cpp`**

include 区加：

```cpp
#include "timer_ui.h"
```

在 `todo_back_cb` 函数之后加 timer 返回回调：

```cpp
/* Timer tools back callback: tools page → dashboard. Runs inside LVGL lock. */
static void timer_back_cb(void)
{
    ui_switch_page(PAGE_DASHBOARD);
}
```

在 `app_main` 中 TODO UI 初始化块之后（`TODO UI init done` 注释之后）加：

```cpp
    /* 17. Timer UI */
    if (Lvgl_Lock(-1)) {
        TimerUi_Init();
        TimerUi_SetBackCallback(timer_back_cb);
        Lvgl_Unlock();
    }
    ESP_LOGI(TAG, "Timer UI init done");
```

（`TimerEngine_Init()` 已在 Task 1 加入，保持在其前即可；顺序无强依赖。）

- [ ] **Step 9: 修改 `main/CMakeLists.txt`**，`PRIV_REQUIRES` 末尾加 `timer_ui`：

```cmake
    PRIV_REQUIRES epaper_driver_bsp lvgl_port ui i2c_bsp port_shtc3 port_rtc port_touch port_battery wifi_bsp todo_manager wifi_ap todo_web todo_ui timer_engine timer_ui)
```

- [ ] **Step 10: 构建并烧录验证**

Run:
```bash
env -u MSYSTEM powershell.exe -ExecutionPolicy Bypass -Command "[Environment]::SetEnvironmentVariable('MSYSTEM', \$null, 'Process'); . E:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1; cd E:\aiot; idf.py -p COM5 flash monitor"
```
Expected: 构建成功；启动日志含 `Timer UI init done`；主屏正常显示。手动验证清单：
```
□ 点主屏左上时间区域 → 进入"时间工具"页（两个按钮：番茄钟/定时提醒）
□ 点"番茄钟" → 进入番茄钟页（仅标题，本 Task 占位）
□ 点左上返回箭头 → 回到时间工具页
□ 在时间工具页点返回箭头 → 回主屏
□ 在时间工具页右滑 → 回主屏
□ 主屏时钟/温湿度/TODO 入口仍正常
```

- [ ] **Step 11: Commit**

```bash
git add components/timer_ui components/ui main/main.cpp main/CMakeLists.txt
git commit -m "feat(timer): timer_ui skeleton + tools page + top-left entry" -m "Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: 番茄钟页（未运行 / 运行中两态 + 启动/取消）

**Files:**
- Modify: `components/timer_ui/timer_ui.c`（实现 `create_pomo_page`、`refresh_pomodoro_page`、启动/取消回调）

**Interfaces:**
- Consumes: `TimerEngine_StartPomodoro`、`TimerEngine_Cancel`、`TimerEngine_GetPomodoro`（来自 Task 1）
- Produces: 番茄钟页可用；`refresh_pomodoro_page` 由 `TimerUi_ShowPage(POMODORO)` 调用。**注意**：本 Task 不含每分钟自动刷新（倒计时数字不会自己变），自动刷新在 Task 4 的 `TimerUi_TickUpdate` 接入。

- [ ] **Step 1: 替换 `create_pomo_page` 为完整实现**

把 Task 2 的占位 `create_pomo_page` 替换为：

```c
/* ============ pomodoro 页 ============ */
static void pomo_start_cb(lv_event_t *e)
{
    uint32_t min = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    if (TimerEngine_StartPomodoro(min, NULL) == ESP_OK) refresh_pomodoro_page();
}

static void pomo_cancel_cb(lv_event_t *e)
{
    (void)e;
    const timer_instance_t *p = TimerEngine_GetPomodoro();
    if (p) TimerEngine_Cancel(p->id);
    refresh_pomodoro_page();
}

static void create_pomo_page(lv_obj_t *parent)
{
    s_pomo_page = make_page(parent);
    create_back_btn(s_pomo_page);

    lv_obj_t *title = lv_label_create(s_pomo_page);
    lv_obj_set_pos(title, 48, 10);
    lv_label_set_text(title, "番茄钟");
    lv_obj_set_style_text_font(title, &lv_font_chinese_14, 0);
    lv_obj_set_style_text_decor(title, LV_TEXT_DECOR_UNDERLINE, 0);

    /* ---- 未运行：选时长 ---- */
    s_pomo_idle_cont = lv_obj_create(s_pomo_page);
    lv_obj_set_size(s_pomo_idle_cont, 200, 160);
    lv_obj_set_pos(s_pomo_idle_cont, 0, 40);
    lv_obj_set_style_bg_opa(s_pomo_idle_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_pomo_idle_cont, 0, 0);
    lv_obj_set_style_pad_all(s_pomo_idle_cont, 0, 0);

    lv_obj_t *hint = lv_label_create(s_pomo_idle_cont);
    lv_obj_set_pos(hint, 0, 10);
    lv_obj_set_size(hint, 200, 16);
    lv_label_set_text(hint, "选择时长");
    lv_obj_set_style_text_font(hint, &lv_font_chinese_14, 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);

    for (int i = 0; i < 3; i++) {
        lv_obj_t *btn = lv_btn_create(s_pomo_idle_cont);
        lv_obj_set_size(btn, 50, 36);
        lv_obj_set_pos(btn, 20 + i * 55, 36);
        lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_radius(btn, 4, 0);
        lv_obj_add_event_cb(btn, pomo_start_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)POMO_PRESETS[i]);
        lv_obj_t *lbl = lv_label_create(btn);
        char buf[8];
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)POMO_PRESETS[i]);
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
        lv_obj_center(lbl);
    }

    lv_obj_t *unit = lv_label_create(s_pomo_idle_cont);
    lv_obj_set_pos(unit, 0, 80);
    lv_obj_set_size(unit, 200, 16);
    lv_label_set_text(unit, "分钟");
    lv_obj_set_style_text_font(unit, &lv_font_chinese_14, 0);
    lv_obj_set_style_text_align(unit, LV_TEXT_ALIGN_CENTER, 0);

    /* ---- 运行中：倒计时 + 进度条 + 取消 ---- */
    s_pomo_run_cont = lv_obj_create(s_pomo_page);
    lv_obj_set_size(s_pomo_run_cont, 200, 160);
    lv_obj_set_pos(s_pomo_run_cont, 0, 40);
    lv_obj_set_style_bg_opa(s_pomo_run_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_pomo_run_cont, 0, 0);
    lv_obj_set_style_pad_all(s_pomo_run_cont, 0, 0);

    s_pomo_min_label = lv_label_create(s_pomo_run_cont);
    lv_obj_set_pos(s_pomo_min_label, 0, 6);
    lv_obj_set_size(s_pomo_min_label, 200, 32);
    lv_label_set_text(s_pomo_min_label, "25");
    lv_obj_set_style_text_font(s_pomo_min_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_pomo_min_label, lv_color_black(), 0);
    lv_obj_set_style_text_align(s_pomo_min_label, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *minu = lv_label_create(s_pomo_run_cont);
    lv_obj_set_pos(minu, 0, 40);
    lv_obj_set_size(minu, 200, 16);
    lv_label_set_text(minu, "min");
    lv_obj_set_style_text_font(minu, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(minu, LV_TEXT_ALIGN_CENTER, 0);

    s_pomo_bar = lv_bar_create(s_pomo_run_cont);
    lv_obj_set_size(s_pomo_bar, 160, 10);
    lv_obj_set_pos(s_pomo_bar, 20, 66);
    lv_bar_set_range(s_pomo_bar, 0, 100);
    lv_obj_set_style_bg_color(s_pomo_bar, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_pomo_bar, 1, 0);
    lv_obj_set_style_border_color(s_pomo_bar, lv_color_black(), 0);
    lv_obj_set_style_bg_color(s_pomo_bar, lv_color_black(), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_pomo_bar, 0, 0);
    lv_obj_set_style_radius(s_pomo_bar, 0, LV_PART_INDICATOR);

    lv_obj_t *cancel = lv_btn_create(s_pomo_run_cont);
    lv_obj_set_size(cancel, 100, 30);
    lv_obj_set_pos(cancel, 50, 92);
    lv_obj_set_style_bg_color(cancel, lv_color_white(), 0);
    lv_obj_set_style_border_width(cancel, 1, 0);
    lv_obj_set_style_radius(cancel, 4, 0);
    lv_obj_add_event_cb(cancel, pomo_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *clbl = lv_label_create(cancel);
    lv_label_set_text(clbl, "取消");
    lv_obj_set_style_text_font(clbl, &lv_font_chinese_14, 0);
    lv_obj_set_style_text_color(clbl, lv_color_black(), 0);
    lv_obj_center(clbl);

    lv_obj_add_flag(s_pomo_run_cont, LV_OBJ_FLAG_HIDDEN);
}
```

- [ ] **Step 2: 替换 `refresh_pomodoro_page` 为完整实现**

```c
static void refresh_pomodoro_page(void)
{
    const timer_instance_t *p = TimerEngine_GetPomodoro();
    if (p) {
        lv_obj_add_flag(s_pomo_idle_cont, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_pomo_run_cont, LV_OBJ_FLAG_HIDDEN);
        char buf[8];
        snprintf(buf, sizeof(buf), "%u", (unsigned)((p->remaining_sec + 59) / 60));
        lv_label_set_text(s_pomo_min_label, buf);
        int pct = (int)(100 * (p->total_seconds - p->remaining_sec) / p->total_seconds);
        lv_bar_set_value(s_pomo_bar, pct, LV_ANIM_OFF);
    } else {
        lv_obj_clear_flag(s_pomo_idle_cont, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_pomo_run_cont, LV_OBJ_FLAG_HIDDEN);
    }
}
```

- [ ] **Step 3: 构建并烧录验证**

Run:
```bash
env -u MSYSTEM powershell.exe -ExecutionPolicy Bypass -Command "[Environment]::SetEnvironmentVariable('MSYSTEM', \$null, 'Process'); . E:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1; cd E:\aiot; idf.py -p COM5 flash monitor"
```
Expected: 构建成功。手动验证清单：
```
□ 时间工具 → 番茄钟 → 显示"选择时长" + 三个按钮 15/25/50
□ 点 25 → 切到运行中状态：大字"25" + min + 进度条(空) + 取消按钮
□ 点取消 → 回到"选择时长"状态
□ 点 15 → 显示"15"
□ 返回 → 回时间工具页 → 返回 → 回主屏
□ 串口日志含 "Pomodoro 25 min started" 与 "Cancelled id=..."
```
（注：运行中数字不会自动递减——那是 Task 4 的能力。本 Task 只验证启动/取消/初始显示。）

- [ ] **Step 4: Commit**

```bash
git add components/timer_ui/timer_ui.c
git commit -m "feat(timer): pomodoro page with start/cancel and two states" -m "Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: 满屏到点提醒 + 队列 + FIFO + 每分钟刷新

**Files:**
- Modify: `components/timer_ui/timer_ui.c`（实现 `TimerUi_PushFiredEvent`、`TimerUi_TickUpdate`、alert 屏幕、`refresh_visible_page`）
- Modify: `main/main.cpp`（注册 `fired_cb` + `data_task` 每秒调 `TimerUi_TickUpdate`）

**Interfaces:**
- Consumes: `TimerEngine_SetFiredCallback`（Task 1）、`PortRtc_GetTime`（`port_rtc.h`）
- Produces: 到点满屏提醒可用；番茄钟运行中数字每分钟自动更新。

- [ ] **Step 1: 在 `timer_ui.c` 实现 alert 屏幕与队列消费**

在 `refresh_reminder_page` 占位函数之后、`/* ============ 公开 API ============ */` 之前加：

```c
/* ============ 满屏到点提醒 ============ */
static void alert_click_cb(lv_event_t *e)
{
    (void)e;
    s_alert_showing = false;
    if (s_last_fired_kind == TIMER_KIND_POMODORO) {
        /* 番茄钟到点 → 回番茄钟页（已变未运行） */
        TimerUi_ShowPage(TIMER_PAGE_POMODORO);
        lv_screen_load(s_timer_screen);
    } else {
        /* 提醒到点 → 回到弹提醒前的屏幕 */
        lv_screen_load(s_screen_before_alert);
    }
    /* 下一秒 TickUpdate 会自动弹队列里下一个 */
}

static void build_alert_screen(void)
{
    s_alert_screen = lv_obj_create(NULL);
    lv_obj_set_size(s_alert_screen, 200, 200);
    lv_obj_set_scrollbar_mode(s_alert_screen, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(s_alert_screen, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_alert_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_alert_screen, 0, 0);
    lv_obj_set_style_radius(s_alert_screen, 0, 0);
    lv_obj_set_style_pad_all(s_alert_screen, 0, 0);
    lv_obj_add_flag(s_alert_screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_alert_screen, alert_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *bell = lv_label_create(s_alert_screen);
    lv_obj_set_pos(bell, 0, 18);
    lv_obj_set_size(bell, 200, 32);
    lv_label_set_text(bell, LV_SYMBOL_BELL);
    lv_obj_set_style_text_font(bell, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_align(bell, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *title = lv_label_create(s_alert_screen);
    lv_obj_set_pos(title, 0, 56);
    lv_obj_set_size(title, 200, 18);
    lv_label_set_text(title, "时间到");
    lv_obj_set_style_text_font(title, &lv_font_chinese_14, 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

    s_alert_type_lbl = lv_label_create(s_alert_screen);
    lv_obj_set_pos(s_alert_type_lbl, 0, 80);
    lv_obj_set_size(s_alert_type_lbl, 200, 16);
    lv_obj_set_style_text_font(s_alert_type_lbl, &lv_font_chinese_14, 0);
    lv_obj_set_style_text_align(s_alert_type_lbl, LV_TEXT_ALIGN_CENTER, 0);

    s_alert_dur_lbl = lv_label_create(s_alert_screen);
    lv_obj_set_pos(s_alert_dur_lbl, 0, 100);
    lv_obj_set_size(s_alert_dur_lbl, 200, 16);
    lv_obj_set_style_text_font(s_alert_dur_lbl, &lv_font_chinese_14, 0);
    lv_obj_set_style_text_align(s_alert_dur_lbl, LV_TEXT_ALIGN_CENTER, 0);

    s_alert_time_lbl = lv_label_create(s_alert_screen);
    lv_obj_set_pos(s_alert_time_lbl, 0, 124);
    lv_obj_set_size(s_alert_time_lbl, 200, 32);
    lv_obj_set_style_text_font(s_alert_time_lbl, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_align(s_alert_time_lbl, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *hint = lv_label_create(s_alert_screen);
    lv_obj_set_pos(hint, 0, 172);
    lv_obj_set_size(hint, 200, 16);
    lv_label_set_text(hint, "点屏幕任意处关闭");
    lv_obj_set_style_text_font(hint, &lv_font_chinese_14, 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
}

static void fill_alert_content(const fired_item_t *item)
{
    char buf[32];
    if (item->kind == TIMER_KIND_POMODORO) {
        lv_label_set_text(s_alert_type_lbl, "番茄钟");
        snprintf(buf, sizeof(buf), "%u 分钟已完成", (unsigned)(item->total_seconds / 60));
    } else {
        lv_label_set_text(s_alert_type_lbl, "定时提醒");
        snprintf(buf, sizeof(buf), "%u 分钟已到", (unsigned)(item->total_seconds / 60));
    }
    lv_label_set_text(s_alert_dur_lbl, buf);

    rtc_datetime_t dt = PortRtc_GetTime();
    snprintf(buf, sizeof(buf), "%02u:%02u", dt.hour, dt.minute);
    lv_label_set_text(s_alert_time_lbl, buf);
}

static void show_alert(const fired_item_t *item)
{
    s_alert_showing = true;
    s_last_fired_kind = item->kind;
    s_screen_before_alert = lv_screen_active();
    if (!s_alert_screen) build_alert_screen();
    fill_alert_content(item);
    lv_screen_load(s_alert_screen);
    ESP_LOGI(TAG, "Alert shown: kind=%d", item->kind);
}

static void refresh_visible_page(void)
{
    if (s_current_page == TIMER_PAGE_POMODORO) refresh_pomodoro_page();
    else if (s_current_page == TIMER_PAGE_REMINDER) refresh_reminder_page();
}
```

- [ ] **Step 2: 替换 `TimerUi_PushFiredEvent` 与 `TimerUi_TickUpdate` 的占位实现**

```c
void TimerUi_PushFiredEvent(uint8_t id, timer_kind_t kind, uint32_t total_seconds)
{
    if (!s_fired_queue) return;
    fired_item_t item = { id, kind, total_seconds };
    xQueueSend(s_fired_queue, &item, 0);
}

void TimerUi_TickUpdate(void)
{
    if (!s_fired_queue) return;

    /* 1. 每秒轮询到点队列 */
    if (!s_alert_showing) {
        fired_item_t item;
        if (xQueueReceive(s_fired_queue, &item, 0) == pdTRUE) {
            show_alert(&item);
        }
    }
    if (s_alert_showing) return;

    /* 2. 每分钟刷新可见的 timer 子页面（倒计时数字/进度条/提醒列表） */
    static uint32_t sec = 0;
    sec++;
    if (lv_screen_active() == s_timer_screen && (sec % 60 == 0)) {
        refresh_visible_page();
    }
}
```

- [ ] **Step 3: 修改 `main/main.cpp`，注册 fired 回调**

在 `timer_back_cb` 之后加：

```cpp
/* Timer fired: runs in FreeRTOS timer service task — MUST NOT touch LVGL.
 * Just enqueue; data_task consumes it under the LVGL lock. */
static void timer_fired_cb(const timer_instance_t *t)
{
    TimerUi_PushFiredEvent(t->id, t->kind, t->total_seconds);
}
```

在 `app_main` 中 Timer UI 初始化块之后加：

```cpp
    TimerEngine_SetFiredCallback(timer_fired_cb);
    ESP_LOGI(TAG, "Timer fired callback registered");
```

- [ ] **Step 4: 修改 `main/main.cpp` 的 `data_task`，每秒驱动 TickUpdate**

找到 `data_task` 中 `if (tick % 1 == 0)` 块（触摸轮询那段）。在该块内、`PortTouch_GetPoint` 之前加 timer 驱动：

```c
        /* Timer UI: poll fired queue + per-minute refresh (every 1 s) */
        if (Lvgl_Lock(500)) {
            TimerUi_TickUpdate();
            Lvgl_Unlock();
        }
```

（保留原有 `PortTouch_GetPoint` 逻辑不动。）

- [ ] **Step 5: 临时改 1 分钟预设以便测试到点**

在 `components/timer_ui/timer_ui.c` 顶部把番茄钟预设临时改短：
```c
static const uint32_t POMO_PRESETS[3] = {1, 25, 50};   /* TEMP: 1 min for fire test */
```

- [ ] **Step 6: 构建并烧录验证**

Run:
```bash
env -u MSYSTEM powershell.exe -ExecutionPolicy Bypass -Command "[Environment]::SetEnvironmentVariable('MSYSTEM', \$null, 'Process'); . E:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1; cd E:\aiot; idf.py -p COM5 flash monitor"
```
Expected: 构建成功。手动验证清单：
```
□ 番茄钟页点"1"（临时预设）→ 运行中显示"1"
□ 等约 60 秒 → 满屏提醒弹出：🔔 + 时间到 + 番茄钟 + 1 分钟已完成 + 当前时间 HH:MM + 点屏幕任意处关闭
□ 点屏幕任意处 → 解除，回到番茄钟页（未运行状态，显示 1/25/50）
□ 串口日志含 "Alert shown: kind=0"
□ 再点"1"启动，运行中等待时每分钟数字递减（1→到点）—— 因预设是 1 分钟，跳过递减直接到点，正常
□ 改回 25 预设后（下一步）再测：点 25 → 显示 25 → 等 1 分钟 → 显示 24（验证每分钟刷新）
```

- [ ] **Step 7: 恢复预设并用 25 分钟验证每分钟刷新**

恢复：
```c
static const uint32_t POMO_PRESETS[3] = {15, 25, 50};
```

构建烧录后验证：点 25 → 显示 "25" → 等 60 秒 → 显示 "24"（确认每分钟刷新生效）→ 点取消回未运行。不必等满 25 分钟到点（到点路径已在 Step 6 验证）。

- [ ] **Step 8: Commit**

```bash
git add components/timer_ui/timer_ui.c main/main.cpp
git commit -m "feat(timer): full-screen fired alert + FIFO queue + per-minute refresh" -m "Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 5: 定时提醒页（6 预设 + 已挂列表 + 取消确认 + 满 3 置灰）

**Files:**
- Modify: `components/timer_ui/timer_ui.c`（实现 `create_rem_page`、`refresh_reminder_page`、确认面板、`rem_start_cb`、`rem_item_click_cb`）

**Interfaces:**
- Consumes: `TimerEngine_StartReminder`、`TimerEngine_Cancel`、`TimerEngine_GetAll`（Task 1）
- Produces: 定时提醒页可用；`refresh_reminder_page` 由 `ShowPage(REMINDER)` 与 `TickUpdate` 每分钟调用。

- [ ] **Step 1: 替换 `create_rem_page` 占位为完整实现**

```c
/* ============ reminder 页 ============ */
static void rem_start_cb(lv_event_t *e)
{
    uint32_t min = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    if (TimerEngine_StartReminder(min, NULL) == ESP_OK) refresh_reminder_page();
}

static void confirm_yes_cb(lv_event_t *e)
{
    (void)e;
    TimerEngine_Cancel(s_confirm_id);
    lv_obj_add_flag(s_confirm_panel, LV_OBJ_FLAG_HIDDEN);
    refresh_reminder_page();
}

static void confirm_no_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_add_flag(s_confirm_panel, LV_OBJ_FLAG_HIDDEN);
}

static void rem_item_click_cb(lv_event_t *e)
{
    uint8_t id = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    s_confirm_id = id;
    lv_obj_clear_flag(s_confirm_panel, LV_OBJ_FLAG_HIDDEN);
}

static void create_rem_page(lv_obj_t *parent)
{
    s_rem_page = make_page(parent);
    create_back_btn(s_rem_page);

    s_rem_title = lv_label_create(s_rem_page);
    lv_obj_set_pos(s_rem_title, 48, 10);
    lv_label_set_text(s_rem_title, "定时提醒 (0/3)");
    lv_obj_set_style_text_font(s_rem_title, &lv_font_chinese_14, 0);
    lv_obj_set_style_text_decor(s_rem_title, LV_TEXT_DECOR_UNDERLINE, 0);

    /* 6 预设按钮 2x3 */
    for (int i = 0; i < 6; i++) {
        lv_obj_t *btn = lv_btn_create(s_rem_page);
        lv_obj_set_size(btn, 55, 30);
        lv_obj_set_pos(btn, 15 + (i % 3) * 60, 34 + (i / 3) * 36);
        lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_radius(btn, 4, 0);
        lv_obj_add_event_cb(btn, rem_start_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)REM_PRESETS[i]);
        lv_obj_t *lbl = lv_label_create(btn);
        char buf[8];
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)REM_PRESETS[i]);
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
        lv_obj_center(lbl);
        s_rem_btns[i] = btn;
    }

    lv_obj_t *hint = lv_label_create(s_rem_page);
    lv_obj_set_pos(hint, 0, 110);
    lv_obj_set_size(hint, 200, 14);
    lv_label_set_text(hint, "分钟后提醒");
    lv_obj_set_style_text_font(hint, &lv_font_chinese_14, 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);

    /* 已挂列表 */
    s_rem_list_cont = lv_obj_create(s_rem_page);
    lv_obj_set_size(s_rem_list_cont, 190, 66);
    lv_obj_set_pos(s_rem_list_cont, 5, 128);
    lv_obj_set_style_bg_opa(s_rem_list_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_rem_list_cont, 0, 0);
    lv_obj_set_style_pad_all(s_rem_list_cont, 0, 0);
    lv_obj_set_style_pad_row(s_rem_list_cont, 2, 0);
    lv_obj_set_flex_flow(s_rem_list_cont, LV_FLEX_FLOW_COLUMN);

    /* 取消确认面板（隐藏，覆盖在 reminder 页上） */
    s_confirm_panel = lv_obj_create(s_rem_page);
    lv_obj_set_size(s_confirm_panel, 130, 70);
    lv_obj_set_pos(s_confirm_panel, 35, 65);
    lv_obj_set_style_bg_color(s_confirm_panel, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_confirm_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_confirm_panel, 1, 0);
    lv_obj_set_style_radius(s_confirm_panel, 4, 0);
    lv_obj_set_style_pad_all(s_confirm_panel, 4, 0);

    lv_obj_t *q = lv_label_create(s_confirm_panel);
    lv_obj_set_pos(q, 0, 6);
    lv_obj_set_size(q, 122, 16);
    lv_label_set_text(q, "确认取消？");
    lv_obj_set_style_text_font(q, &lv_font_chinese_14, 0);
    lv_obj_set_style_text_align(q, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *no_btn = lv_btn_create(s_confirm_panel);
    lv_obj_set_size(no_btn, 50, 24);
    lv_obj_set_pos(no_btn, 8, 34);
    lv_obj_set_style_bg_color(no_btn, lv_color_white(), 0);
    lv_obj_set_style_border_width(no_btn, 1, 0);
    lv_obj_set_style_radius(no_btn, 4, 0);
    lv_obj_add_event_cb(no_btn, confirm_no_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *nlbl = lv_label_create(no_btn);
    lv_label_set_text(nlbl, "否");
    lv_obj_set_style_text_font(nlbl, &lv_font_chinese_14, 0);
    lv_obj_center(nlbl);

    lv_obj_t *yes_btn = lv_btn_create(s_confirm_panel);
    lv_obj_set_size(yes_btn, 50, 24);
    lv_obj_set_pos(yes_btn, 72, 34);
    lv_obj_set_style_bg_color(yes_btn, lv_color_white(), 0);
    lv_obj_set_style_border_width(yes_btn, 1, 0);
    lv_obj_set_style_radius(yes_btn, 4, 0);
    lv_obj_add_event_cb(yes_btn, confirm_yes_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ylbl = lv_label_create(yes_btn);
    lv_label_set_text(ylbl, "是");
    lv_obj_set_style_text_font(ylbl, &lv_font_chinese_14, 0);
    lv_obj_center(ylbl);

    lv_obj_add_flag(s_confirm_panel, LV_OBJ_FLAG_HIDDEN);
}
```

- [ ] **Step 2: 替换 `refresh_reminder_page` 占位为完整实现**

```c
static void refresh_reminder_page(void)
{
    lv_obj_clean(s_rem_list_cont);

    timer_instance_t all[TIMER_MAX_SLOTS];
    uint8_t n = 0;
    TimerEngine_GetAll(all, &n);

    uint8_t rem_count = 0;
    for (int i = 0; i < n; i++) {
        if (all[i].kind != TIMER_KIND_REMINDER) continue;
        uint32_t min = (all[i].remaining_sec + 59) / 60;
        char buf[24];
        snprintf(buf, sizeof(buf), "还剩 %u min", (unsigned)min);

        lv_obj_t *btn = lv_btn_create(s_rem_list_cont);
        lv_obj_set_size(btn, 188, 20);
        lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_radius(btn, 2, 0);
        lv_obj_add_event_cb(btn, rem_item_click_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)all[i].id);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_font(lbl, &lv_font_chinese_14, 0);
        lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
        lv_obj_center(lbl);
        rem_count++;
    }

    /* 满 3 个 → 预设按钮置灰 */
    bool full = (rem_count >= 3);
    for (int i = 0; i < 6; i++) {
        if (full) lv_obj_add_state(s_rem_btns[i], LV_STATE_DISABLED);
        else      lv_obj_clear_state(s_rem_btns[i], LV_STATE_DISABLED);
    }

    char t[32];
    snprintf(t, sizeof(t), "定时提醒 (%u/3)", rem_count);
    lv_label_set_text(s_rem_title, t);
}
```

- [ ] **Step 3: 临时改 1 分钟预设测试到点**

```c
static const uint32_t REM_PRESETS[6] = {1, 10, 15, 30, 45, 60};  /* TEMP: 5→1 for fire test */
```

- [ ] **Step 4: 构建并烧录验证**

Run:
```bash
env -u MSYSTEM powershell.exe -ExecutionPolicy Bypass -Command "[Environment]::SetEnvironmentVariable('MSYSTEM', \$null, 'Process'); . E:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1; cd E:\aiot; idf.py -p COM5 flash monitor"
```
Expected: 构建成功。手动验证清单：
```
□ 时间工具 → 定时提醒 → 显示 6 个按钮 + "定时提醒 (0/3)"
□ 点"1" → 列表出现"还剩 1 min"，标题变"(1/3)"
□ 再点"1"两次 → 列表 3 项，标题"(3/3)"，6 个按钮置灰
□ 点列表中间一项 → 弹出"确认取消？"面板（否/是）
□ 点"是" → 该项消失，标题"(2/3)"，按钮恢复可点
□ 点"否" → 面板消失，列表不变
□ 等 ~60 秒 → 满屏提醒弹出：🔔 时间到 定时提醒 1 分钟已到 HH:MM
□ 点屏幕解除 → 回到提醒页
□ 串口日志含 "Reminder 1 min started" 与 "Alert shown: kind=1"
```

- [ ] **Step 5: 恢复预设**

```c
static const uint32_t REM_PRESETS[6] = {5, 10, 15, 30, 45, 60};
```

- [ ] **Step 6: Commit**

```bash
git add components/timer_ui/timer_ui.c
git commit -m "feat(timer): reminder page with presets, list, cancel confirm" -m "Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 6: 主屏右下角番茄钟迷你显示

**Files:**
- Modify: `main/main.cpp`（`data_task` 每秒读引擎并调 `ui_update_timer_mini`）

**Interfaces:**
- Consumes: `TimerEngine_GetPomodoro`（Task 1）、`ui_update_timer_mini`（Task 2 已声明+实现，控件已建）

> 说明：`ui_update_timer_mini` 与右下控件已在 Task 2 创建并实现，本 Task 只需在 `data_task` 每秒调用它。

- [ ] **Step 1: 修改 `main/main.cpp` 的 `data_task`，每秒更新迷你显示**

在 Task 4 加入的 `TimerUi_TickUpdate()` 调用块内，紧随其后、`Lvgl_Unlock()` 之前加：

```c
            const timer_instance_t *pomo = TimerEngine_GetPomodoro();
            if (pomo)
                ui_update_timer_mini(true, pomo->remaining_sec, pomo->total_seconds);
            else
                ui_update_timer_mini(false, 0, 0);
```

使该块最终为：

```c
        /* Timer UI: poll fired queue + per-minute refresh + mini display (every 1 s) */
        if (Lvgl_Lock(500)) {
            TimerUi_TickUpdate();
            const timer_instance_t *pomo = TimerEngine_GetPomodoro();
            if (pomo)
                ui_update_timer_mini(true, pomo->remaining_sec, pomo->total_seconds);
            else
                ui_update_timer_mini(false, 0, 0);
            Lvgl_Unlock();
        }
```

- [ ] **Step 2: 构建并烧录验证**

Run:
```bash
env -u MSYSTEM powershell.exe -ExecutionPolicy Bypass -Command "[Environment]::SetEnvironmentVariable('MSYSTEM', \$null, 'Process'); . E:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1; cd E:\aiot; idf.py -p COM5 flash monitor"
```
Expected: 构建成功。手动验证清单：
```
□ 主屏右下角默认显示 "Image" 占位
□ 进番茄钟页 → 点 25 → 返回主屏 → 右下角显示 "番茄" + "25" + 空进度条（Image 占位隐藏）
□ 等 60 秒 → 右下数字变 "24"，进度条前进一格
□ 进番茄钟页点取消 → 返回主屏 → 右下角变回 "Image" 占位
□ 番茄钟运行中主屏时钟/温湿度仍正常更新
```

- [ ] **Step 3: Commit**

```bash
git add main/main.cpp
git commit -m "feat(timer): dashboard right-bottom pomodoro mini display" -m "Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 7: 多提醒 FIFO + 全量集成验证

**Files:**
- 无新代码改动；本 Task 是设计文档 8.2 节集成清单的完整真机走查 + 必要的微调。

**Interfaces:**
- Consumes: 全部前序 Task 成果。

- [ ] **Step 1: 临时改两个短预设测 FIFO**

```c
static const uint32_t REM_PRESETS[6] = {1, 2, 15, 30, 45, 60};  /* TEMP: 5→1, 10→2 for FIFO test */
```

- [ ] **Step 2: 构建烧录，跑 FIFO 验证**

Run:
```bash
env -u MSYSTEM powershell.exe -ExecutionPolicy Bypass -Command "[Environment]::SetEnvironmentVariable('MSYSTEM', \$null, 'Process'); . E:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1; cd E:\aiot; idf.py -p COM5 flash monitor"
```
手动验证：
```
□ 定时提醒页连点"1"和"2" → 列表 2 项
□ 约 60 秒后"1"到点 → 满屏提醒（定时提醒 1 分钟已到）
□ 点屏幕解除 → 立即弹出第二个提醒（2 分钟那个，因 60s 已过其 remaining≈0，几乎同时到）
   （若第二个尚未到，等几秒它会自动弹出，验证 FIFO 排队）
□ 解除第二个 → 回提醒页，列表空
```

- [ ] **Step 3: 恢复预设**

```c
static const uint32_t REM_PRESETS[6] = {5, 10, 15, 30, 45, 60};
```

- [ ] **Step 4: 构建烧录，跑完整集成清单**

Run:
```bash
env -u MSYSTEM powershell.exe -ExecutionPolicy Bypass -Command "[Environment]::SetEnvironmentVariable('MSYSTEM', \$null, 'Process'); . E:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1; cd E:\aiot; idf.py -p COM5 flash monitor"
```
完整清单（设计文档 8.2）：
```
□ 启动番茄钟 25 → 主屏右下出现 番茄+25+进度条
□ 切回主屏 → 右下倒计时每分钟更新
□ 进番茄钟页 → 显示运行中状态 + 取消按钮
□ 取消番茄钟 → 回到选时长状态，右下变回 Image
□ 启动番茄钟 → 返回主屏 → 右下倒计时持续走，不中断
□ 设 3 个提醒（5/10/15）→ 提醒页列表显示 3 项
□ 第 4 个提醒预设按钮全部置灰
□ 取消中间一个提醒 → 列表更新为 2/3
□ 番茄钟到点 → 满屏弹页（番茄钟 + N 分钟已完成 + 当前时间）
□ 点屏幕任意处 → 解除，回到番茄钟页未运行状态
□ 设两个提醒接近同时到点 → 第一个弹 → 解除 → 立即弹第二个（FIFO）
□ 满屏提醒时番茄钟仍在后台跑（若同时设过）
□ 主屏左上点击 → 时间工具页 → 番茄钟/提醒入口
□ 各页面右滑手势返回（与 todo_ui 一致）
□ 主屏时钟/温湿度/TODO/WiFi 状态不受影响
□ 反复启动/取消/到点 10 次 → 串口无异常、不崩溃
```

- [ ] **Step 5: 内存稳定性观察**

在 monitor 中观察启动日志的堆内存，反复操作 10 次后重启一次，确认无内存不足告警。若需精确数值，可在 `data_task` 临时加一行 `ESP_LOGI(TAG, "heap free=%u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT));` 跑一轮后移除。

- [ ] **Step 6: 最终 Commit**

```bash
git add -A
git commit -m "test(timer): integration pass — FIFO + full checklist verified" -m "Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Self-Review

**1. Spec coverage（对照设计文档各节）:**
- §1 目标与范围：番茄钟(15/25/50 单次) + 提醒(6 预设/最多3并发) → Task 1/3/5 ✓
- §2 关键决策表：入口左上→工具页(Task 2)、软件定时器(Task 1)、纯常亮不持久化(Global Constraints)、单次番茄钟(Task 1/3)、并发3(Task 1/5)、右下迷你显示(Task 2/6)、每分钟刷新(Task 4)、满屏提醒点任意处解除(Task 4)、FIFO(Task 4/7) ✓
- §3 架构：timer_engine(Task 1) + timer_ui(Task 2-5) + ui 改动(Task 2) + main 接线(Task 2/4/6) ✓
- §4 数据流/状态机/线程模型：fired_cb→队列→data_task 消费(Task 4)、锁顺序(Global Constraints/Task 1) ✓
- §5 UI 布局：tools/pomodoro/reminder/alert/mini 全覆盖(Task 2-6)，emoji→文字+LV_SYMBOL_BELL(Global Constraints 已声明偏离) ✓
- §6 错误处理：槽位满置灰(Task 5)、Cancel 不存在返回 NOT_FOUND(Task 1)、到点瞬间取消(Task 1 active 检查) ✓
- §7 资源：软件定时器+4 槽+队列(Task 1) ✓
- §8 测试：跳过单测(Global Constraints)、真机清单(Task 7) ✓
- §9 实现顺序：8 阶段 → 7 Task 映射（1+2 合 Task1，3 拆 Task2/3，4→Task4，5→Task5，6 拆 Task2/6，7 贯穿，8→Task7）✓

**2. Placeholder scan:** 无 TBD/TODO；每个代码步骤含完整代码。临时测试代码均标注 `TEMP` 并有恢复步骤。

**3. Type consistency:**
- `timer_instance_t` 字段（id/kind/total_seconds/remaining_sec/active）跨 Task 1/3/4/5/6 一致 ✓
- `timer_page_t` 三值在 Task 2 定义、Task 2 的 `ui_switch_page` 与 Task 4 的 `refresh_visible_page` 使用一致 ✓
- `TimerUi_PushFiredEvent(id, kind, total_seconds)` 三参签名：Task 1 fired_cb 传 `t->total_seconds`（Task 4 Step 3）、Task 2 声明、Task 4 实现一致 ✓
- `ui_update_timer_mini(bool, uint32_t, uint32_t)`：Task 2 声明+实现、Task 6 调用一致 ✓
- `TIMER_MAX_SLOTS=4`：Task 1 定义、Task 5 `GetAll` 缓冲区使用一致 ✓

**4. 已知偏离设计文档（有意为之，已记录）:**
- emoji 🍅/⏰ → `LV_SYMBOL_BELL` + 中文文字（无 emoji 字体）。
- `ui_page_t` 未加 `PAGE_FIRED_ALERT`：alert 由 `timer_ui` 自载 `lv_screen_load`，不经 `ui_switch_page`，加该枚举会是死代码。
- 番茄钟到点解除后回番茄钟页（符合设计）；提醒到点解除后回"提醒前屏幕"（用 `lv_screen_active()` 快照，比设计举例的"DASHBOARD 或 REMINDER"更通用）。
- 确认面板用自建 `lv_obj` 而非 `lv_msgbox`（避免 LV_USE_MSGBOX 与 v9 API 不确定性）。

**Plan complete and saved to `docs/ecc/plans/2026-06-19-timer-reminder.md`.**
