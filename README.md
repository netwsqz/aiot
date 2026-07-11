# AIoT Dashboard（ESP32-S3 墨水屏待办/天气/计时器桌面助手）

基于 ESP32-S3 + 1.54" SSD1681 墨水屏（200×200）+ PSRAM 的桌面助手固件。
集成 TODO 管理、番茄钟/提醒计时器、和风天气、电子宠物、深睡眠待机，
并通过 WiFi AP + HTTP REST 与手机配网/同步数据。

- **平台**：ESP-IDF v5.5.4，LVGL v9（managed component），FreeRTOS
- **屏幕**：200×200 1bpp e-ink（SSD1681），无动画，~300ms 局刷
- **存储**：PSRAM（Octal 80MHz）放显存/缓存，NVS 存 TODO/计时器/天气凭证，SPIFFS 存自定义待机图
- **配网**：长按 BOOT 进 WiFi 管理页 → 启动 AP 热点 → 手机连入 → 浏览器配置 WiFi/天气

## 快速开始

构建（必须在 Git Bash 里通过 PowerShell 调 `idf.py`，ESP-IDF v5.5.4 拒绝 MSys）：

```bash
env -u MSYSTEM powershell.exe -ExecutionPolicy Bypass -Command \
  "[Environment]::SetEnvironmentVariable('MSYSTEM', \$null, 'Process'); \
   . E:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1; \
   cd E:\aiot; idf.py build"
```

烧录：`idf.py -p COM5 flash`（同上 PowerShell 包裹）。

首次烧录前，先用 `nvs_config.csv` 生成 NVS 分区初始数据写入 0x9000，或通过 Kconfig/
Web 配置接口填写 WiFi/天气凭证（避免源码硬编码密钥）。

## 架构

三层组件结构，全部在 `main/main.cpp` 的 `app_main()` 装配：

```
┌─────────────────────────────────────────────────────────┐
│ main.cpp        app_main — 初始化序列、任务、深睡眠路径   │
├─────────────────────────────────────────────────────────┤
│ 应用层  todo_ui  timer_ui  todo_web  todo_manager        │
│         timer_engine  wifi_bsp  wifi_ap  wifi_ui         │
│         standby_screen  weather_fetch/cache/ui  pet_ui   │
├─────────────────────────────────────────────────────────┤
│ LVGL/UI lvgl_port  ui  font_cn                           │
├─────────────────────────────────────────────────────────┤
│ BSP/HAL epaper_driver_bsp  i2c_bsp                       │
│         port_shtc3  port_rtc  port_touch  port_battery   │
└─────────────────────────────────────────────────────────┘
```

只有 `main/main.cpp` 是 C++，其余为 C 或 C-with-extern-C wrapper。

### 关键数据流

- **TODO**：手机 → WiFi AP → HTTP REST（`todo_web`）→ NVS（`todo_manager`）→ LVGL 屏（`todo_ui`）
- **计时器**：按钮/触摸 → `timer_engine`（FreeRTOS timer）→ NVS 持久化 → `timer_ui`（满屏闹钟）
- **天气**：`data_task` 每 30 min HTTPS 拉取（`weather_fetch`，gzip+miniz 解压）→ NVS 缓存（`weather_cache`）→ `weather_ui`
- **传感器**：`data_task` 轮询 SHTC3/RTC/Battery → 更新 dashboard
- **深睡眠**：双击 PWR → `enter_deep_sleep()` 持久化计时器、绘待机图、EXT1 唤醒（IO0/IO18）
  → 唤醒后重跑 `app_main()`（不同初始化路径，跳过 WiFi，延迟到长按 BOOT）

### 任务列表

| 任务 | Core | 优先级 | 栈 | 职责 |
|------|------|--------|----|----|
| lvgl_handler | 1 | 4 | 8KB | LVGL tick + timer_handler + 触摸读取 |
| data_task | 0 | 3 | 12KB | 传感器轮询 + UI 刷新 + WiFi 状态 + 天气拉取（含 mbedTLS 握手） |
| boot_key_task | 1 | 5 | 4KB | BOOT 短按切屏 / 长按进 WiFi 管理页 |
| pwr_key_task | 1 | 5 | 3KB | PWR 双击 → 深睡眠 |
| timer tick | 系统 timer 任务 | — | — | 1s 倒计时 |

## 组件清单

| 组件 | 行数 | 职责 |
|------|------|----|
| ai_engine | 1201 | AI 语音对话（Qwen ASR WS + DeepSeek LLM）— **当前 master 未编译**，见下 |
| pet_ui | 1362 | 电子宠物 192×192 1bpp，6 表情聚合 TODO/计时器/电量/时间 |
| todo_web | 737 | HTTP REST + 配网页面（gzip）+ WiFi/天气/待机图配置接口 |
| weather_fetch | 595 | QWeather HTTPS（now/24h/3d），gzip 解压，NVS 配置覆盖 |
| timer_ui | 666 | 番茄钟页 / 提醒页 / 满屏闹钟（FIFO 队列桥接 timer 任务→data_task） |
| todo_manager | 596 | TODO NVS 持久化（list blob + 分块 task blob），mutex 保护 |
| ui | 457 | dashboard 4 象限 + 7 屏页面调度 |
| standby_screen | 197 | 深睡眠待机画面（SPIFFS 自定义 BMP / 默认字模） |
| epaper_driver_bsp | 358 | SSD1681 驱动（全刷/局刷/LUT），PSRAM 显存 |
| wifi_bsp | 260 | WiFi STA + NTP + 自动重连定时器 |
| timer_engine | 295 | 4 槽计时器（1 番茄 + 3 提醒），NVS 持久化/恢复（含睡眠 elapsed 校正） |
| weather_ui | 270 | 天气页 Swiss editorial 布局 |
| wifi_ap | 159 | APSTA 模式，15min 超时，原子站点计数 |
| lvgl_port | 212 | LVGL v9 桥接，递归 mutex，PSRAM 双缓冲 |
| weather_cache | 116 | 天气快照 NVS blob（version + size 校验） |
| port_shtc3/rtc/touch/battery | 79-129 | I2C/ADC 外设抽象 |

## 配置

- **WiFi 凭证**：`components/wifi_bsp/Kconfig`（默认）→ NVS `wifi` namespace 覆盖 → Web `POST /api/wifi`
- **天气凭证**：`components/weather_fetch/Kconfig`（API key / location / city / base_url）→ NVS `weather` namespace 覆盖 → Web `POST /api/weather/config`
- **分区表**：`partitions.csv`（NVS 0x6000 / app 0x3B0000 / SPIFFS 0x40000）

## HTTP API（`todo_web`，catch-all `/api/*`）

| 方法 | 路径 | 说明 |
|------|------|----|
| GET | `/` | 配网页面（gzip） |
| GET/POST | `/api/lists` | 列表增删 |
| PUT/DELETE | `/api/lists/{id}` | 改/删列表 |
| GET/POST | `/api/lists/{id}/tasks` | 任务查/增 |
| PUT/DELETE | `/api/tasks/{id}` | 改/删任务 |
| GET/POST/DELETE | `/api/standby` | 待机图下载/上传(5000B 1bpp)/删除 |
| GET/POST | `/api/wifi` | WiFi SSID/状态 / 保存凭证重连 |
| GET/POST | `/api/weather/config` | 天气配置（key 脱敏返回）/ 写入并热加载 |
| POST | `/api/weather/refresh` | 触发立即拉取 |

## 硬件引脚

完整 GPIO 表见 `CLAUDE.md`。要点：
- I2C 共用 IO47/48（SHTC3 0x70 / RTC 0x51 / Touch 0x38）
- EPD 电源 IO6 低有效，Audio IO42 低有效，VBAT IO17 高有效 —— 三者需同时开启
- PWR(IO18)/BOOT(IO0) 为深睡眠 EXT1 唤醒源
- PSRAM 必须启用（Octal 80MHz）

## 文档

- `CLAUDE.md` — 构建命令、架构、GPIO 表、常见模式（给 Claude Code 用）
- `docs/ecc/specs/` — 行为设计规格（TODO/计时器/待机睡眠/天气）
- `docs/ecc/plans/` — 实现计划
- `docs/BUGFIX-REPORT-2026-07.md` — 本轮 bug 修复报告
- `docs/NEXT-STEPS-2026-07.md` — 后续路线建议
- `V2/` — Waveshare 原厂示例（LVGL v8，仅参考）
- `web_dev/index.html` — Web UI 独立开发版

## 关于 `ai_engine`

`components/ai_engine/ai_engine.c`（1201 行）实现了完整的 AI 语音对话（Qwen ASR WebSocket +
DeepSeek LLM HTTP），但**当前 master 分支不编译它**——`main/CMakeLists.txt` 未列入 REQUIRES，
`main.cpp` 已移除所有 `AiEngine_*` 调用（保留在 git stash `wip: strip ai engine` 中）。
完整可用的 AI 实现存在于 worktree `.worktrees/weather-feature` 的 `feature/weather-forecast` 分支
上。如需启用 AI，从该分支 cherry-pick `ai_engine` 组件 + `ai_ui` 组件 + `main.cpp` 的 AI 装配段。
详见 `docs/NEXT-STEPS-2026-07.md`。
