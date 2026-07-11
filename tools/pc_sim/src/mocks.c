/*
 * mocks.c — PC 测试桩：把 ui_screen.c / pet_ui.c / draw_ui.c 依赖的业务层
 * （todo/timer/weather/wifi/draw/PortRtc/PortBattery）和几个 esp 工具函数
 * 在主机上用假数据实现，让 UI 源码不改一行就能链接运行。
 *
 * 这里的"假数据"刻意可调（见 mock setters），方便你跑不同场景看 UI。
 * 跑实机时这些函数由真正的 ESP-IDF 组件提供，本文件不参与固件编译。
 */
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_timer.h"
#include "esp_random.h"

#include "lvgl.h"
#include "todo_manager.h"
#include "todo_ui.h"
#include "timer_engine.h"
#include "timer_ui.h"
#include "weather_fetch.h"
#include "port_rtc.h"
#include "port_battery.h"
#include "font_cn.h"

/* lv_font_chinese_14 现在直接编进 PC 工程（font_cn 源码不依赖 ESP-IDF，
 * 见 CMakeLists 的 target_sources）。无需别名桩。 */


/* ─────────── esp 工具函数 PC 实现 ─────────── */

static int64_t pc_now_us = 1;   /* 由 main.c 每帧推进，单调递增 */

int64_t pc_esp_timer_get_time_us(void)
{
    return pc_now_us;
}

void pc_advance_tick_us(int64_t delta_us)
{
    /* 由 main.c 每帧调用，推进伪时间。单调累加。 */
    if (pc_now_us == 0) pc_now_us = 1;   /* 非 0 起点，避免兜底分支 */
    pc_now_us += delta_us;
}

uint32_t esp_random(void)
{
    static int seeded = 0;
    if (!seeded) { srand((unsigned)time(NULL)); seeded = 1; }
    return (uint32_t)rand();
}

/* ─────────── PortRtc — 假时间，可被 main 改 ─────────── */

static rtc_datetime_t s_rtc = { .year=2026, .month=7, .day=11,
                                 .hour=14, .minute=32, .second=0, .week=5 };

void PortRtc_Init(void) {}
void PortRtc_SetTime(uint16_t y, uint8_t mo, uint8_t d,
                     uint8_t h, uint8_t mi, uint8_t s)
{
    s_rtc.year=y; s_rtc.month=mo; s_rtc.day=d;
    s_rtc.hour=h; s_rtc.minute=mi; s_rtc.second=s;
}
rtc_datetime_t PortRtc_GetTime(void) { return s_rtc; }

void mock_rtc_set_hour(uint8_t h) { s_rtc.hour = h; }

/* ─────────── PortBattery — 假电量 ─────────── */

static uint8_t s_batt = 80;
void PortBattery_Init(void) {}
float PortBattery_GetVoltage(void) { return 4.0f; }
uint8_t PortBattery_GetLevel(void) { return s_batt; }
void mock_battery_set(uint8_t pct) { s_batt = pct; }

/* ─────────── TodoMgr — 内存假数据 ─────────── */
/* 给 dashboard 的 ui_update_todo_summary 喂几条 TODO，能看出优先级排序。 */

static todo_list_t s_lists[TODO_MAX_LISTS];
static todo_task_t s_tasks[TODO_MAX_TASKS];
static uint8_t s_list_count = 1;
static uint16_t s_task_count = 3;

static void mock_todo_seed(void)
{
    static int seeded = 0;
    if (seeded) return; seeded = 1;
    strcpy(s_lists[0].id, "abcd1234");
    strcpy(s_lists[0].name, "Inbox");
    strcpy(s_lists[0].icon, "IN");

    strcpy(s_tasks[0].text, "Write report");
    s_tasks[0].priority = 2; s_tasks[0].done = false;
    strcpy(s_tasks[1].text, "Reply email");
    s_tasks[1].priority = 1; s_tasks[1].done = false;
    strcpy(s_tasks[2].text, "Take a walk");
    s_tasks[2].priority = 0; s_tasks[2].done = false;
}

esp_err_t TodoMgr_Init(void) { mock_todo_seed(); return ESP_OK; }
esp_err_t TodoMgr_GetLists(todo_list_t *lists, uint8_t *count)
{
    mock_todo_seed();
    if (s_list_count > *count && *count != 0) { /* caller too small */ }
    uint8_t n = s_list_count;
    if (n > TODO_MAX_LISTS) n = TODO_MAX_LISTS;
    for (uint8_t i=0;i<n;i++) lists[i] = s_lists[i];
    *count = n;
    return ESP_OK;
}
esp_err_t TodoMgr_GetTasks(const char *list_id, todo_task_t *tasks, uint16_t *count)
{
    (void)list_id;
    uint16_t n = s_task_count;
    if (n > TODO_MAX_TASKS) n = TODO_MAX_TASKS;
    for (uint16_t i=0;i<n;i++) tasks[i] = s_tasks[i];
    *count = n;
    return ESP_OK;
}
/* 其余 TodoMgr_* 在固件里存在但 ui_screen/pet_ui 不调用，给空实现防链接报错 */
esp_err_t TodoMgr_CreateList(const char *n, const char *i, char *o){(void)n;(void)i;(void)o;return ESP_FAIL;}
esp_err_t TodoMgr_UpdateList(const char *id, const char *n, const char *i, const uint8_t *o){(void)id;(void)n;(void)i;(void)o;return ESP_FAIL;}
esp_err_t TodoMgr_DeleteList(const char *id, uint16_t *dt){(void)id;(void)dt;return ESP_FAIL;}
esp_err_t TodoMgr_CreateTask(const char *l, const char *t, uint8_t p, const char *d, char *o){(void)l;(void)t;(void)p;(void)d;(void)o;return ESP_FAIL;}
esp_err_t TodoMgr_UpdateTask(const char *id, const bool *d, const uint8_t *p, const char *due){(void)id;(void)d;(void)p;(void)due;return ESP_FAIL;}
esp_err_t TodoMgr_DeleteTask(const char *id){(void)id;return ESP_FAIL;}
void TodoMgr_SetChangeCallback(todo_change_cb_t cb){(void)cb;}

/* ─────────── TimerEngine — 假番茄钟 ─────────── */

static timer_instance_t s_pomo = { .id=1, .kind=TIMER_KIND_POMODORO,
                                   .total_seconds=25*60, .remaining_sec=18*60, .active=1 };
static uint8_t s_pomo_active = 0;

esp_err_t TimerEngine_Init(void){return ESP_OK;}
esp_err_t TimerEngine_StartPomodoro(uint32_t m, uint8_t *id){(void)m;*id=1;s_pomo_active=1;return ESP_OK;}
esp_err_t TimerEngine_StartReminder(uint32_t m, uint8_t *id){(void)m;*id=2;return ESP_OK;}
esp_err_t TimerEngine_Cancel(uint8_t id){(void)id;s_pomo_active=0;return ESP_OK;}
esp_err_t TimerEngine_GetAll(timer_instance_t *out, uint8_t *c)
{
    if (s_pomo_active) { out[0]=s_pomo; *c=1; } else { *c=0; }
    return ESP_OK;
}
const timer_instance_t *TimerEngine_GetPomodoro(void)
{
    return s_pomo_active ? &s_pomo : NULL;
}
void TimerEngine_SetFiredCallback(timer_fired_cb_t cb){(void)cb;}
esp_err_t TimerEngine_Persist(void){return ESP_OK;}
esp_err_t TimerEngine_Restore(void){return ESP_OK;}

void mock_pomodoro_set(bool running, uint32_t remaining_sec, uint32_t total_sec)
{
    s_pomo_active = running ? 1 : 0;
    s_pomo.remaining_sec = remaining_sec;
    s_pomo.total_seconds = total_sec;
}

/* ─────────── 其它 UI 组件：空实现（dashboard 不进这些页就不需要真屏幕） ─────────── */
/* 若要测某个子页（如 weather），把它的真 .c 加进 CMakeLists 即可，这里就不需要桩。 */

esp_err_t TodoUi_Init(void){return ESP_OK;}
void TodoUi_Refresh(void){}
void TodoUi_ShowPage(todo_page_type_t p, const char *id){(void)p;(void)id;}
todo_page_type_t TodoUi_GetCurrentPage(void){return TODO_PAGE_LISTS;}
lv_obj_t *TodoUi_GetScreen(void){return NULL;}
void TodoUi_SetBackCallback(void (*cb)(void)){(void)cb;}

esp_err_t TimerUi_Init(void){return ESP_OK;}
lv_obj_t *TimerUi_GetScreen(void){return NULL;}
void TimerUi_ShowPage(timer_page_t p){(void)p;}
void TimerUi_SetBackCallback(void (*cb)(void)){(void)cb;}
void TimerUi_PushFiredEvent(uint8_t id, timer_kind_t k, uint32_t t){(void)id;(void)k;(void)t;}
void TimerUi_TickUpdate(void){}

void WeatherUI_Init(void){}
lv_obj_t *WeatherUI_GetScreen(void){return NULL;}
void WeatherUI_Render(const weather_snapshot_t *s, bool cached){(void)s;(void)cached;}
void WeatherUI_RenderIfCached(void){}
void WeatherUI_SetBackCallback(void (*cb)(void)){(void)cb;}

void WifiUi_Init(void){}
lv_obj_t *WifiUi_GetScreen(void){return NULL;}
void WifiUi_UpdateStatus(bool s, bool a, const char *ssid, const char *ip,
                         const char *ap_ip, int stas)
{ (void)s;(void)a;(void)ssid;(void)ip;(void)ap_ip;(void)stas; }
void WifiUi_SetBackCallback(void (*cb)(void)){(void)cb;}
void WifiUi_SetToggleApCallback(void (*cb)(void)){(void)cb;}
void WifiUi_SetReconnectCallback(void (*cb)(void)){(void)cb;}
void WifiUi_SetRefreshWeatherCallback(void (*cb)(void)){(void)cb;}

/* DrawUi_* 现在编译进真 draw_ui.c（见 CMakeLists target_sources）。
 * draw_ui.c 用 fopen("/spiffs/draw.bmp")：PC 无 SPIFFS，fopen 失败时
 * HasSavedImage/RenderThumbnail 安全返回 false，sim 降级不崩。*/
