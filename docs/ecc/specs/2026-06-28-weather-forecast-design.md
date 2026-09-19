# 天气 + 预报功能设计文档

**日期**：2026-06-28
**状态**：设计已确认，待实现
**所属阶段**：Phase 1 增强

---

## 1. 目标与范围

在现有 ESP32-S3 1.54" 200×200 墨水屏固件上新增天气功能：保留室内温度显示，同时增加室外当前天气与天气预报。

目标体验：

- 主屏右上角显示天气摘要：室外温度、天气图标、室外湿度，并保留室内温度。
- 新增独立天气页，采用“居中焦点式”布局：当前天气大字展示，下方显示逐时趋势与未来预报。
- 使用和风天气 QWeather 作为天气数据源。
- 支持深睡眠唤醒后显示上次缓存的天气数据。

不做的事：

- 不做天气动画或复杂图形效果。
- 不做历史天气曲线。
- 不做 GPS 定位或自动城市定位。
- 不做多城市切换。
- 不做离线天气预测；无网络时只显示缓存。
- 不新增自动定时唤醒联网刷新天气。

---

## 2. 关键决策

| 维度 | 决策 |
|---|---|
| 数据源 | 和风天气 QWeather |
| 天气范围 | 当前天气 + 未来 24 小时逐时预报 + API 使用 3d 端点（内部存储/显示明天+后天） |
| API Base URL | Kconfig 可配置，默认 `devapi.qweather.com`（免费开发版）；生产使用 `api.qweather.com`（付费订阅） |
| 主屏显示 | 当前室外天气摘要 + 室内温度 |
| 独立天气页布局 | 居中焦点式：大温度、大图标、逐时预报、未来预报 |
| 室内温度 | 继续使用 SHTC3，本地传感器数据不被天气 API 替代 |
| 配置方式 | Kconfig 默认值 + Web API 修改后存 NVS |
| 缓存策略 | 成功拉取后写入 NVS；深睡眠唤醒先显示缓存 |
| 缓存有效期 | 2 小时；超过有效期标注"数据过旧" |
| 刷新周期 | 默认 30 分钟，Kconfig 可配置 |
| 失败重试 | 不立即重试，等待到下一调度周期；连续失败 3 次后间隔翻倍至最大 2 小时 |
| 图标策略 | 将和风天气图标码映射到 5-6 个黑白 RGB565A8 图标 |
| 网络策略 | 复用现有 WiFi STA；不为天气新增常驻热点或后台重连策略 |
| 深睡眠刷新策略 | 深睡眠唤醒后不自动联网刷新天气；仅显示 NVS 缓存数据，直到底层 WiFi 恢复 |

---

## 3. 架构与组件

### 3.1 组件划分

新增 3 个组件，修改 2 个现有组件：

```text
components/
├── weather_fetch/     新增：和风天气 HTTP 客户端、JSON 解析、数据模型
├── weather_cache/     新增：天气快照 NVS 缓存
├── weather_ui/        新增：独立天气 LVGL 页面
├── ui/                修改：主屏右上角天气摘要 + 室内温度
└── main/              修改：组件初始化、定时拉取、页面调度
```

切分原则：

- `weather_fetch` 只负责联网与解析，不依赖 LVGL。
- `weather_cache` 只负责持久化，不发起网络请求。
- `weather_ui` 只负责渲染，不关心数据如何获取。
- `main.cpp` 负责把 WiFi、缓存、拉取、UI 更新串起来。

### 3.2 数据流

正常联网刷新：

```text
data_task 每 30 分钟
  → WeatherFetch_Update()              # 不持有 Lvgl_Lock
  → WeatherCache_Save()                # 不持有 Lvgl_Lock
  → Lvgl_Lock(500)
  → WeatherUI_Render(snapshot, false)
  → ui_update_weather_summary(snapshot, indoor_temp)
  → Lvgl_Unlock()
```

深睡眠唤醒：

```text
app_main()
  → WeatherCache_Load()                # 不要求 WiFi
  → Lvgl_Lock(500)
  → WeatherUI_Render(snapshot, true)
  → ui_update_weather_summary(snapshot, indoor_temp)
  → Lvgl_Unlock()
  → 等用户通过 BOOT 长按恢复 WiFi 后，再进入正常联网刷新
```

`Lvgl_Lock()` 只允许包住 LVGL 渲染，不允许包住 HTTP 请求或 NVS 操作。

---

## 4. 数据模型

天气组件使用解析后的紧凑数据结构，避免 UI 和缓存直接依赖 QWeather 原始 JSON。

```c
#define WEATHER_TEXT_LEN       16
#define WEATHER_ICON_CODE_LEN  8
#define WEATHER_DATE_LEN       12
#define WEATHER_HOURLY_COUNT   4
#define WEATHER_DAILY_COUNT    2
#define WEATHER_CACHE_VERSION  1

typedef struct {
    char text[WEATHER_TEXT_LEN];
    char icon[WEATHER_ICON_CODE_LEN];
    char city_name[WEATHER_TEXT_LEN];
    int temp;
    int feels_like;
    int humidity;
    int wind_scale;
    int64_t obs_time;
} weather_now_t;

typedef struct {
    int hour;
    char icon[WEATHER_ICON_CODE_LEN];
    int temp;
} weather_hourly_t;

typedef struct {
    char date[WEATHER_DATE_LEN];
    char text_day[WEATHER_TEXT_LEN];
    char icon_day[WEATHER_ICON_CODE_LEN];
    int temp_min;
    int temp_max;
} weather_daily_t;

typedef struct {
    uint32_t version;
    weather_now_t now;
    weather_hourly_t hourly[WEATHER_HOURLY_COUNT];
    weather_daily_t daily[WEATHER_DAILY_COUNT];
    int64_t fetch_time;
} weather_snapshot_t;
```

说明：

- 逐时预报只保留接下来 4 小时，适合 200×200 小屏显示。
- 未来预报只显示明天和后天，当前天气区域已经覆盖“今天”。
- QWeather `3d` 端点会返回今天、明天、后天；解析时丢弃今天，只保存明天和后天。
- `city_name` 来自 Web 配置或 Kconfig，避免在设备端调用额外的城市查询 API。
- `version` 固定为 `WEATHER_CACHE_VERSION`，NVS 读取时先检查版本再使用。
- 字符串字段使用固定长度数组，写入前必须边界截断并保证 NUL 结尾。

---

## 5. QWeather API

使用 3 个端点，Base URL 通过 `CONFIG_WEATHER_API_BASE_URL` 配置：

```text
GET https://<base-url>/v7/weather/now?location=<id>&key=<key>
GET https://<base-url>/v7/weather/24h?location=<id>&key=<key>
GET https://<base-url>/v7/weather/3d?location=<id>&key=<key>
```

默认使用 `devapi.qweather.com`，适合免费开发版。发布或长期运行时可改为 `api.qweather.com`，但需要用户确认对应 QWeather 订阅计划和额度。

调用策略：

- 串行调用，避免并发 socket 增加内存与网络复杂度。
- 单次 HTTP 超时 10 秒。
- 响应只接受 JSON。
- 每个端点单独解析与校验；任一关键端点失败则本轮更新失败，保留旧缓存。

API 配置：

```text
CONFIG_WEATHER_API_KEY=""
CONFIG_WEATHER_LOCATION_ID="101010100"
CONFIG_WEATHER_CITY_NAME="北京"
CONFIG_WEATHER_API_BASE_URL="devapi.qweather.com"
CONFIG_WEATHER_FETCH_INTERVAL_MIN=30
CONFIG_WEATHER_CACHE_TTL_MIN=120
```

`CONFIG_WEATHER_LOCATION_ID` 默认值 `101010100` 为北京，仅作为可运行默认城市。正式使用时通过 Kconfig 或 Web 配置修改。

---

## 6. 天气图标

将 QWeather 图标码映射为少量黑白图标，避免为所有图标码制作资源。

| QWeather 图标码 | 语义 | 墨水屏图标 |
|---|---|---|
| 100 | 晴 | sun |
| 101-104 | 多云、少云、晴间多云、阴 | cloud |
| 300-399 | 雨 | rain |
| 400-499 | 雪 | snow |
| 500-509 | 雾、霾、沙尘 | fog |
| 其他 | 未分类 | cloud |

资源策略：

- 图标尺寸 24×24。
- 使用 LVGL v9 可直接渲染的 RGB565A8 C 数组，不使用 1bit raw bitmap。
- 5 个 24×24 图标约增加 2.8KB flash，可接受。
- 编译进 `weather_ui.c`，不放 SPIFFS。
- 先提供 5 个核心图标；未来可扩充但不影响数据结构。

---

## 7. 组件接口

### 7.1 weather_fetch

`weather_fetch` 提供初始化、拉取、错误查询接口。

```c
esp_err_t WeatherFetch_Init(void);
bool WeatherFetch_IsReady(void);
bool WeatherFetch_Update(weather_snapshot_t *snapshot);
const char *WeatherFetch_LastError(void);
```

行为：

- 初始化时读取 Kconfig 默认配置与 NVS 覆盖配置。
- `WeatherFetch_Init()` 只表示模块初始化是否成功，不把“未配置 API Key”当作初始化失败。
- `WeatherFetch_IsReady()` 返回 API Key、location ID、city name 是否都已配置。
- `WeatherFetch_Update()` 执行当前天气、24小时预报、3日预报三次请求。
- 解析失败、网络失败、API key 缺失、城市 ID 缺失时返回 `false`。
- `LastError()` 返回最近一次错误说明，用于日志与调试。

### 7.2 weather_cache

`weather_cache` 提供天气快照持久化接口。

```c
bool WeatherCache_Init(void);
void WeatherCache_Save(const weather_snapshot_t *snapshot);
bool WeatherCache_Load(weather_snapshot_t *snapshot);
void WeatherCache_Clear(void);
```

NVS 设计：

| Namespace | Key | 类型 | 用途 |
|---|---|---|---|
| `weather` | `snapshot` | blob | 最近一次天气快照 |
| `weather` | `valid` | u8 | 快照有效标记 |

约束：

- `WeatherCache_Save()` 只保存解析后的 `weather_snapshot_t`，不保存原始 JSON。
- 读取缓存时先检查 `version == WEATHER_CACHE_VERSION`，再检查 blob 大小是否等于 `sizeof(weather_snapshot_t)`。
- 城市或 API Key 改变后调用 `WeatherCache_Clear()`，避免显示旧城市天气。

### 7.3 weather_ui

`weather_ui` 提供独立天气页面。

```c
void WeatherUI_Init(void);
lv_obj_t *WeatherUI_GetScreen(void);
void WeatherUI_Render(const weather_snapshot_t *snapshot, bool is_cached);
void WeatherUI_SetIndoorTemp(float temp_c);
```

约束：

- 所有 LVGL 调用必须在 `Lvgl_Lock()` / `Lvgl_Unlock()` 内执行。
- `Lvgl_Lock()` 严禁包住网络请求、NVS 读写或任何可能阻塞的操作；只包住 `WeatherUI_Render()` 和主屏摘要更新。
- 页面根对象为 200×200，白底，无边框、无圆角、无阴影。
- 无动画，所有状态切换直接刷新。
- 中文文本使用 `lv_font_chinese_14`，保证中文天气文本可显示。
- 当前温度大字使用现有 Montserrat 数字字体；若现有字体尺寸不足，新增仅包含数字、负号和 °C 的轻量字体。

---

## 8. UI 设计

### 8.1 主屏右上角摘要

现有右上角区域从“室内温湿度”升级为“天气摘要 + 室内温度”。

```text
┌──────────────┐
│ ☁ 26° 室外   │
│ 湿度 65%     │
│ 室内 24°     │
└──────────────┘
```

改动点：

- `img_weather` 从空占位变为天气图标。
- `lbl_temperature` 改为室外温度。
- `lbl_humidity` 改为室外湿度。
- 新增 `lbl_indoor_temp` 显示 SHTC3 室内温度。

如果 API Key 未配置或没有缓存，则右上角退回室内传感器模式：

```text
┌──────────────┐
│ 室内 24°     │
│ 室内湿度 55% │
│ 天气未配置   │
└──────────────┘
```

也就是说，`lbl_humidity` 在有天气快照时显示室外湿度；在天气不可用时显示 SHTC3 室内湿度，避免主屏丢失本地传感器信息。

### 8.2 独立天气页

采用用户确认的“居中焦点式”布局。

```text
┌────────────────────┐
│        ☁           │
│       26°C         │
│     多云 · 北京     │
│  湿度65% 风速3级    │
├────────────────────┤
│ 逐时预报            │
│ 14 ☀28  15 ⛅27    │
│ 16 ☁25  17 🌧23    │
├────────────────────┤
│ 明日 🌧 16/22°     │
│ 后日 ☀ 20/28°      │
│ 14:30 更新 / 缓存   │
└────────────────────┘
```

布局原则：

- 当前温度最大，保证一眼可读。
- 逐时预报固定展示 4 个点，避免小字拥挤。
- 未来预报显示明天、后天，不强行显示今天。
- 缓存数据必须标明“缓存”或更新时间；超过 2 小时标注“数据过旧”。
- 入口：点击主屏右上角天气摘要区域进入天气页；点击天气页空白处或使用现有页面返回机制回到主屏。

---

## 9. Web 配置接口

在现有 `todo_web` API dispatcher 中新增天气配置接口。

```text
GET  /api/weather/config
POST /api/weather/config
```

GET 响应用于 Web 表单回显配置，但不返回完整 API Key：

```json
{
  "location_id": "101010100",
  "city_name": "北京",
  "api_key_masked": "****abcd"
}
```

POST 请求体：

```json
{
  "api_key": "your_qweather_key",
  "location_id": "101010100",
  "city_name": "北京"
}
```

成功响应：

```json
{
  "ok": true
}
```

错误响应：

```json
{
  "error": "api_key required"
}
```

校验规则：

- `api_key` 必须是非空字符串。
- `location_id` 必须是非空字符串，最大 16 字符。
- `city_name` 必须是非空字符串，最大 15 个 UTF-8 字节，超过则拒绝。
- NVS 写入失败返回 500。
- 配置成功后清除旧天气缓存。

安全注意：

- API Key 不写入源码。
- Web 响应不回显完整 API Key。
- 日志不打印完整 API Key。

---

## 10. 错误处理与降级

| 场景 | 行为 | UI 表现 |
|---|---|---|
| API Key 未配置 | `WeatherFetch_IsReady()` 返回 false | 主屏退回室内传感器模式 + 天气未配置 |
| WiFi 未连接 | 跳过联网拉取 | 显示缓存天气；无缓存则显示未连接 |
| HTTP 超时 | 本轮更新失败 | 保留旧缓存，不清空 UI |
| API 限流或返回错误码 | 本轮更新失败 | 保留旧缓存，日志记录错误码 |
| 连续失败 3 次 | 刷新间隔翻倍 | 最大间隔 2 小时，成功后恢复默认间隔 |
| JSON 字段缺失 | 关键字段缺失则失败 | 保留旧缓存 |
| NVS 缓存损坏 | 清除 valid 标记 | 显示无缓存状态 |
| 缓存超过 2 小时 | 仍可显示但标注过旧 | 天气页底部显示“数据过旧” |
| 深睡眠唤醒 | 不立即要求联网 | 显示缓存数据与更新时间，直到用户恢复 WiFi |

关键原则：

- 不因天气失败影响 TODO、计时器、主屏传感器刷新。
- 不阻塞 `data_task` 太久；联网请求应有超时。
- 拉取失败时不得覆盖最后一次成功缓存。

---

## 11. 实现顺序

1. 先做最小 HTTPS 验证：用 `esp_http_client` 请求 QWeather `now` 端点，确认 TLS 握手、证书配置、响应大小、任务栈与 heap 余量。
2. 新增 `weather_fetch` 数据模型与 QWeather 客户端。
3. 新增 `weather_cache` NVS 缓存。
4. 新增 `weather_ui`，先用 mock `weather_snapshot_t` 验证布局。
5. 修改 `ui` 主屏右上角，拆分室外天气与室内温度。
6. 修改 `main.cpp` 初始化顺序、页面注册、`data_task` 定时刷新。
7. 修改 `todo_web`，新增天气配置 API。
8. 编译验证 `idf.py build`。
9. 设备验证：配置 API Key 与城市，查看主屏摘要、天气页、深睡眠缓存显示。

---

## 12. 验证计划

由于本项目硬件依赖较重，标准主机 TDD/覆盖率不适用，按项目测试规则使用编译与设备实测验证。

编译验证：

```bash
idf.py build
```

设备实测：

- API Key 未配置时，主屏仍能显示室内温湿度。
- 配置 API Key 和城市后，主屏显示室外温度、室外湿度、室内温度。
- 天气页显示当前天气、逐时预报、明日/后日预报。
- 断网或 WiFi 未连接时，显示上次缓存并标注缓存状态。
- 深睡眠唤醒后，在 WiFi 尚未初始化时能显示缓存天气。
- 天气拉取失败不影响 TODO、计时器、RTC、SHTC3、Battery 刷新。

---

## 13. 风险与待确认事项

| 风险 | 影响 | 缓解 |
|---|---|---|
| HTTPS 证书与内存开销 | `esp_http_client` + TLS 可能增加 RAM 使用 | 启用 `CONFIG_MBEDTLS_DYNAMIC_BUFFER`；将 `CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN` 降到 4096；每次请求后 `esp_http_client_cleanup()`；请求前检查 internal heap 余量低于 40KB 时跳过 |
| QWeather API 额度限制 | 频繁拉取可能触发限流 | 默认 30 分钟刷新；失败不立即重试；连续失败 3 次后间隔翻倍，最大 2 小时 |
| 200×200 小屏文字拥挤 | 预报可读性下降 | 逐时只保留 4 条，未来只显示 2 天 |
| 深睡眠后 WiFi 延迟初始化 | 天气无法立即刷新 | NVS 缓存先显示；深睡眠唤醒不自动联网，直到用户恢复 WiFi 后刷新 |
| API Key 泄露 | 安全风险 | 不写源码，不回显完整 Key，不打印完整 Key |

当前设计没有未决需求；实现时如果实际渲染过于拥挤，优先缩减逐时预报数量，而不是缩小中文字体。