# 天气功能实现交接文档

**日期**：2026-06-29
**状态**：设计已完成，实现待开始
**分支**：`feature/weather-forecast`（worktree 在 `E:\aiot\.worktrees\weather-feature`，基于 `c71f741`）

---

## 1. 已完成的工作

### 1.1 设计文档
- `docs/ecc/specs/2026-06-28-weather-forecast-design.md` — 完整设计，已经过 4 维度自审修正

### 1.2 实现计划
- `docs/ecc/plans/2026-06-28-weather-forecast.md` — 7 步实现计划，含完整代码

### 1.3 HTTPS 验证（Task 0 已完成）
- 在 `main.cpp` 中临时添加了 `esp_http_client` + HTTPS 请求代码
- **编译通过**，TLS 路径可用
- **关键发现**：
  - ESP-IDF v5.5.4 中 `HTTP_GET` 已更名为 `HTTP_METHOD_GET`
  - CMakeLists.txt 需要添加 `esp_http_client` + `esp-tls` 到 `PRIV_REQUIRES`
  - `esp_crt_bundle_attach` 可直接使用，无需自定义 CA 证书
  - 固件 1.9MB，分区 3.7MB，空间充足
- 验证后已清理所有临时代码，**无新 commit**

---

## 2. 待实现的任务（按顺序）

### Task 1: `weather_fetch` 组件
**目标**：QWeather HTTP 客户端 + JSON 解析 → `weather_snapshot_t`

需创建 4 个文件：
- `components/weather_fetch/Kconfig`
- `components/weather_fetch/CMakeLists.txt`
- `components/weather_fetch/weather_fetch.h`
- `components/weather_fetch/weather_fetch.c`

**关键注意事项**：
- CMakeLists.txt `PRIV_REQUIRES` 必须包含 `esp_http_client esp-tls wifi_bsp nvs_flash cJSON freertos esp_event`
- 使用 `HTTP_METHOD_GET`（不是 `HTTP_GET`）
- 使用 `esp_crt_bundle_attach` 做 TLS 证书验证
- 每次请求后必须 `esp_http_client_cleanup()` 释放 TLS 上下文
- `WeatherFetch_Init()` 返回 `esp_err_t`，另提供 `WeatherFetch_IsReady()` 检查配置完整性
- Init 中从 Kconfig 读默认值后，还要从 NVS namespace `"weather"` 读取覆盖值（api_key / location_id / city_name）
- `WeatherFetch_Update()` 串行发 3 个 HTTPS 请求（now / 24h / 3d），每个超时 10s
- 24h 解析时只保留前 4 个逐时条目；3d 解析时跳过 index 0（今天），只保存明天+后天
- 计划文档中 `WeatherFetch_Update()` 有一个 bug：24h 部分有死代码 `cJSON_Parse_buf`，应删除，直接用 `http_get_json()` + `cJSON_Parse()` 模式（与 now 和 3d 一致）

### Task 2: `weather_cache` 组件
**目标**：`weather_snapshot_t` NVS 持久化

需创建 3 个文件：
- `components/weather_cache/weather_cache.h`
- `components/weather_cache/weather_cache.c`
- `components/weather_cache/CMakeLists.txt`

**关键注意事项**：
- CMakeLists.txt `PRIV_REQUIRES` 包含 `nvs_flash freertos weather_fetch`
- NVS namespace `"weather"`，key `"snapshot"`（blob）+ `"valid"`（u8）
- `WeatherCache_Load()` 先检查 `valid==1`，再检查 blob size == `sizeof(weather_snapshot_t)`，再检查 `snapshot->version == WEATHER_CACHE_VERSION`
- `WeatherCache_Save()` 写入前设置 `snapshot->version = WEATHER_CACHE_VERSION`
- `nvs_flash_init()` 在 `WeatherCache_Init()` 中调用（幂等）

### Task 3: `weather_ui` 组件
**目标**：独立天气 LVGL 页面 + 主屏天气摘要更新

需创建 3 个文件：
- `components/weather_ui/weather_ui.h`
- `components/weather_ui/weather_ui.c`
- `components/weather_ui/CMakeLists.txt`

**关键注意事项**：
- CMakeLists.txt `PRIV_REQUIRES` 包含 `lvgl lvgl_port weather_fetch weather_cache font_cn`
- 页面 200×200，白底，无边框/圆角/阴影
- 布局：居中焦点式（大温度 28px → 天气文字 → 城市 → 湿度风速 → 逐时 4 条 → 明日后日预报 → 更新时间）
- 图标用 RGB565A8 格式的 `lv_img_dsc_t`（5 个 24×24 图标 ~2.8KB flash），按 QWeather 图标码分类：sun/cloud/rain/snow/fog
- **`weather_ui.h` 中还需声明 `ui_update_weather_summary()`** — 供 main.cpp 中 data_task 调用，更新主屏右上角
- `ui_update_weather_summary(snapshot, indoor_temp, outdoor_available)`：
  - 有 outdoor 数据时：`lbl_temperature` 显示室外温度，`lbl_humidity` 显示室外湿度，`lbl_indoor_temp` 显示室内温度
  - 无 outdoor 数据时：`lbl_temperature` 显示室内温度，`lbl_humidity` 显示室内湿度，隐藏 `lbl_indoor_temp`
- **`ui_switch_page(PAGE_WEATHER)` 只做 `lv_screen_load()`，不调用 `WeatherUI_Render()`** — 避免反向依赖。天气页渲染由 `data_task` 在检测到页面可见时负责
- 天气页返回：点击页面空白处回到 `PAGE_DASHBOARD`

### Task 4: 修改 `ui` 组件
**目标**：主屏右上角天气摘要 + 页面注册

需修改：
- `components/ui/ui.h` — 新增 `lbl_indoor_temp`、`PAGE_WEATHER`、`ui_update_weather_summary()` 声明、`#include "weather_fetch.h"`
- `components/ui/ui_screen.c` — 新增 `lbl_indoor_temp` 控件、右上角点击进入天气页、`ui_switch_page` 新增 `PAGE_WEATHER` 分支
- `components/ui/CMakeLists.txt` — `PRIV_REQUIRES` 加入 `weather_ui`

**关键注意事项**：
- `ui_switch_page(PAGE_WEATHER)` **只做 `lv_screen_load(weather_screen)`，不调用 `WeatherUI_Render()`**。渲染由 main.cpp 的 data_task 负责
- 右上角点击回调：`weather_entry_click_cb` → `ui_switch_page(PAGE_WEATHER)`
- 新增 `static lv_obj_t *s_weather_screen = NULL;`（与 `s_todo_screen`、`s_timer_screen` 同级）
- `lbl_indoor_temp` 放在 `lbl_humidity` 下方，字号 `lv_font_chinese_14`

### Task 5: 修改 `main.cpp`
**目标**：初始化序列 + data_task 天气调度 + 深睡眠缓存恢复

**关键注意事项**：
- 新增 `#include "weather_fetch.h"` / `"weather_cache.h"` / `"weather_ui.h"`
- **SHTC3 温度变量**：当前 `data_task` 中没有缓存 SHTC3 温度值的静态变量（`sht.Temp` 是内联的）。需要新增 `static float s_indoor_temp_c = 0;`，在 SHTC3 读取时更新
- **天气调度**：在 data_task 中新增 `s_weather_fetch_tick`（秒计数）和 `s_weather_consecutive_fail`（连续失败计数）
  - 正常间隔 `CONFIG_WEATHER_FETCH_INTERVAL_MIN * 60` 秒
  - 连续失败 3 次后间隔翻倍（×2, ×4, ×8...），最大 7200 秒
  - 成功后重置 `s_weather_consecutive_fail = 0`
- **Lvgl_Lock 边界**：
  - `WeatherFetch_Update()` 和 `WeatherCache_Save()` **不在锁内**
  - 只有 `WeatherUI_Render()` 和 `ui_update_weather_summary()` **在锁内**
- **深睡眠唤醒路径**：在 TimerEngine/TodoMgr init 之后，WiFi 之前：
  ```cpp
  WeatherCache_Init();
  bool weather_loaded = WeatherCache_Load(&s_weather_snapshot);
  s_weather_has_cached = weather_loaded;
  WeatherFetch_Init();
  if (Lvgl_Lock(-1)) {
      WeatherUI_Init();
      if (weather_loaded) WeatherUI_Render(&s_weather_snapshot, true);
      Lvgl_Unlock();
  }
  ```
- **冷启动路径**：同上，但放在 WiFi init 之后（因为冷启动有 WiFi）
- **天气页渲染时机**：data_task 中天气拉取成功后，检查 `ui_get_current_page() == PAGE_WEATHER` 再决定是否调用 `WeatherUI_Render()`

### Task 6: 修改 `todo_web`
**目标**：天气配置 API

**关键注意事项**：
- CMakeLists.txt `PRIV_REQUIRES` 加入 `weather_fetch nvs_flash`
- 新增两个 handler：`api_get_weather_config` + `api_post_weather_config`
- **GET 必须从 NVS 读取**（不是 Kconfig 宏），因为用户可能通过 POST 修改了配置。读取逻辑：
  ```cpp
  nvs_handle_t handle;
  esp_err_t err = nvs_open("weather", NVS_READONLY, &handle);
  if (err == ESP_OK) {
      size_t len = sizeof(buf);
      nvs_get_str(handle, "api_key", buf, &len);
      // ... 同理读 location_id, city_name
      nvs_close(handle);
  } else {
      // fallback to Kconfig defaults
  }
  ```
- API Key masking：只显示最后 4 位，前面用 `****` 替换
- POST 写入 NVS 后调用 `WeatherCache_Clear()` 清除旧缓存
- 注册路由：`httpd_register_uri_handler()` 分别注册 GET 和 POST `/api/weather/config`

---

## 3. 全局约束（实现时必须遵守）

- 所有 LVGL 调用必须在 `Lvgl_Lock()` / `Lvgl_Unlock()` 内执行
- `Lvgl_Lock()` 严禁包住网络请求、NVS 读写或任何可能阻塞的操作
- 页面根对象 200×200，白底，无边框、无圆角、无阴影
- 无动画，所有状态切换直接刷新
- 中文文本使用 `lv_font_chinese_14`
- 大温度数字使用 `lv_font_montserrat_28`
- ESP-IDF v5.5.4 使用 `HTTP_METHOD_GET`（不是 `HTTP_GET`）
- 每次 HTTPS 请求后必须 `esp_http_client_cleanup()`
- 串行请求，不要并发 socket
- 缓存有效期 2 小时，刷新间隔默认 30 分钟
- 连续失败 3 次后刷新间隔翻倍，最大 2 小时
- 深睡眠唤醒不自动联网刷新天气

---

## 4. 编译验证命令

```bash
env -u MSYSTEM powershell.exe -ExecutionPolicy Bypass -Command \
  "[Environment]::SetEnvironmentVariable('MSYSTEM', \$null, 'Process'); \
   . E:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1; \
   cd E:\aiot; idf.py build"
```

每个 Task 完成后都需编译验证。

---

## 5. 关键文件参考

| 文件 | 用途 |
|------|------|
| `docs/ecc/specs/2026-06-28-weather-forecast-design.md` | 设计文档（完整需求） |
| `docs/ecc/plans/2026-06-28-weather-forecast.md` | 实现计划（含代码，注意修正下方已知的 bug） |
| `components/wifi_bsp/wifi_bsp.h` + `.c` | WiFi 初始化模式参考 |
| `components/wifi_bsp/Kconfig` | Kconfig 格式参考 |
| `components/timer_engine/timer_engine.h` + `.c` | 纯 C 组件 + NVS 持久化模式参考 |
| `components/todo_web/todo_web.cpp` | HTTP API dispatcher + cJSON 解析模式参考 |
| `components/ui/ui.h` + `ui_screen.c` | LVGL 页面注册 + 页面切换模式参考 |
| `main/main.cpp` | 初始化序列 + data_task 调度参考 |

---

## 6. 计划文档中已确认的 Bug（需在实现时修正）

1. **24h 解析死代码**：`WeatherFetch_Update()` 中 24h 部分有 `cJSON_Parse_buf`（未定义变量），应删除这两行，改为与 now 和 3d 一致的 `http_get_json()` + `cJSON_Parse()` 模式
2. **`HTTP_GET` → `HTTP_METHOD_GET`**：ESP-IDF v5.5.4 枚举名变更
3. **GET `/api/weather/config` 应从 NVS 读取**，不应使用 Kconfig 宏
4. **`ui_switch_page(PAGE_WEATHER)` 不调用 `WeatherUI_Render()`**：避免 ui 组件反向依赖 weather_ui。渲染由 data_task 负责
5. **`s_sh_temp_c` 不存在**：main.cpp 中没有缓存 SHTC3 温度的静态变量，需要新增
6. **CMakeLists.txt 依赖**：`weather_fetch` 需要 `esp_http_client esp-tls`（不是 `esp_https_ota`，计划中写错了）
