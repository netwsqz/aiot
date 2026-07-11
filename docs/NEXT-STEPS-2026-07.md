# 后续路线建议 — 2026-07-08

基于本轮全项目审查，给出按优先级排序的下一步。每条都标注了**为什么**（动机）和**怎么做**（入手点）。

---

## P0 — 先验证本轮修复（必须）

本轮修了 5 个 CRITICAL + 13 个 HIGH + 5 个 MEDIUM，但全是静态修复，**没有任何真机验证**。
ESP-IDF 硬件代码无法主机 mock，以下场景必须烧录后实测：

1. **深睡眠循环**：双击 PWR 进睡眠 → PWR/BOOT 唤醒 → 再进睡眠，重复 10+ 次。验证：
   - 不再 panic（C2 I2C touch 泄漏 / C3 重入 / H1 EPD 泄漏）
   - 唤醒后触摸仍工作（C2 的 `PortTouch_Deinit` + 重新 `PortTouch_Init`）
   - PSRAM 不随循环增长（H1）
2. **WiFi 自愈**：拔路由器电源 30s 再插上，观察设备是否在 ~60s 内自动重连（H5）。
3. **计时器到点**：起 1min 提醒 ×3 同时到点，验证满屏闹钟 FIFO 队列逐个弹出（C1/H4）。
4. **Web 配置热加载**：连 AP → 改天气城市 → 立即点刷新，验证新城市数据生效无需重启（H8）。
5. **TODO API 边界**：用 curl 发超长 name/text，验证返回 400 而非截断存储（H14/H15）。

---

## P1 — 启用并打磨 AI 语音（最大功能缺口）

`ai_engine`（1201 行）+ `ai_ui` 已在 `.worktrees/weather-feature` 分支完整实现并修过 15 轮并发 bug，
但 master 把它剥离了（git stash `wip: strip ai engine`）。本轮又额外修了 5 个 ai_engine 静态 bug（C5/H12）。

**为什么**：这是项目的核心卖点（"AIoT"），但当前 master 完全没有 AI。worktree 里的实现已经过 15 轮迭代，
质量比从零写高得多。

**怎么做**：
1. 从 `.worktrees/weather-feature` cherry-pick `components/ai_engine/` + `components/ai_ui/` + `audio_bsp` 组件
2. 把 worktree `main.cpp` 的 AI 装配段（`AiEngine_Init` / `AiEngine_Tick` / `AiUi_*` / `AudioBsp` probe）
   合并进 master `main.cpp`，注意与 weather/pet 的初始化顺序
3. `main/CMakeLists.txt` REQUIRES 加 `ai_engine ai_ui audio_bsp`
4. 烧录实测 ASR（Qwen WS）+ LLM（DeepSeek）链路，重点验证本轮 C5/H12 修复在真机不复发
5. **风险**：ai_engine 用 8KB×2 任务栈 + WS/HTTP，DIRAM 已 85.7%，启用后内存要复查 `idf.py size`

---

## P2 — 安全加固（配网面）

当前 AP 开放无密码 + HTTP 全无认证 + CORS `*`。物理接近即可改 WiFi 凭证/删 TODO/改天气 key。

**为什么**：作为桌面助手通常在私人空间，风险可控，但若要给他人用或量产，这是必补项。

**怎么做**（按收益/成本排序）：
1. **AP 加 WPA2-PSK**：`wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK`，密码从 NVS/Kconfig 读。
   成本最低，收益最大。
2. **配网确认按钮**：Web 改 WiFi/天气配置时，设备屏上弹"确认修改？"需点触摸确认。
   防止连入 AP 的恶意客户端静默改凭证。
3. **API key 迁移到 header**：QWeather 支持 `X-QWeather-API-Key` header，移除 URL `key=` 参数，
   避免任何代理/日志泄露。
4. **CORS 限 Origin**：AP 模式限制 `http://192.168.4.*`，STA 模式限设备 IP。

---

## P3 — 可靠性与体验

### P3.1 计时器满屏闹钟无超时自动关闭
当前 alert 必须点屏幕关闭，若用户不在，多个到点提醒会无限积压在队列。
**改法**：`timer_ui` alert 显示 30s/60s 后自动关并弹下一个。

### P3.2 idle 返回 pet 页用 32 位 tick，49 天溢出
`tick` 是 `uint32_t`，溢出时 `tick - s_last_activity_tick` 回绕成大数，立即触发跳 pet。
**改法**：用 `uint64_t` 或 `xTaskGetTickCount()` + 回绕安全比较（`(int32_t)(tick - last) >= 60`）。

### P3.3 `data_task` 栈 12KB 紧张
天气 HTTPS（mbedTLS 握手 ~30KB）+ 8KB JSON 解析栈缓冲 + SHTC3 阻塞 70ms 都挤在一个 12KB 栈任务。
启用 AI 后会更紧。
**改法**：把天气拉取拆到独立任务（更大栈，按需启停），`data_task` 只做轻量轮询。

### P3.4 HTTP server 栈 8KB + handler 内 ~3.3KB 局部数组
`handle_get_lists` 栈上 `todo_task_t[32]` ≈ 2.8KB + cJSON。当前够用但无余量。
**改法**：handler 内大数组改静态/堆。

### P3.5 `pet_ui` 1405 行（1200 行字模数据）
字模由 `scripts/inject_bitmaps.py` 自动注入，逻辑只 ~163 行。
**改法**：字模抽到独立 `pet_bitmaps.h`（generated），让逻辑部分可读。低优先。

---

## P4 — 架构改进（长期）

### P4.1 统一并发原语
项目里并发保护五花八门：`wifi_ap` 用 `std::atomic`，`weather_fetch` 现在用 `portMUX`，
`ai_engine` 用 mutex + volatile，`todo_manager`/`timer_engine` 用 mutex。
**改法**：定一个项目级约定（如：单 flag 用 atomic/critical，结构体用 mutex），写进 `CLAUDE.md`。

### P4.2 深睡眠 teardown 用任务通知替代 vTaskDelete
`vTaskDelete(data_task)` 立即杀任务，若它正持 I2C/LVGL 锁，后续 deinit 可能 assert。
当前靠 50ms delay + bus 重建兜底，不严谨。
**改法**：`data_task` 改成监听一个"请退出"信号（task notification / stop flag），
在安全点（循环顶部、释放锁后）自行 return。`enter_deep_sleep` 发信号 + 等待 `eTaskGetState == Deleted`。

### P4.3 NVS 数据版本化
`todo_manager` 用 JSON blob，`timer_engine`/`weather_cache` 用 struct blob + version 字段。
struct blob 改字段会让旧数据全部失效（weather_cache 已有 version，timer 加了 size 校验）。
**改法**：todo_manager 也加 schema version 字段；或统一用 JSON（灵活但占空间）。

### P4.4 EPD 局刷效率
`lvgl_port` flush_cb 每次全帧 5000 字节重绘 + `EPD_Clear`。e-ink 无动画，但 LVGL 内部状态变就触发 flush。
**改法**：flush_cb 开头检查 area 是否真有变化；或加帧率限制（如最小 5s 间隔）降功耗。

---

## P5 — 功能扩展（按用户价值）

- **多日天气**：`WEATHER_DAILY_COUNT` 现在只 1（明日），屏幕 200×200 其实能塞 2-3 天。改 Kconfig + UI 布局。
- **TODO 提醒到期**：`todo_task_t.due` 字段已存但未用。加 due 时间到点触发 timer 提醒。
- **宠物互动**：当前 pet 只 6 表情静态。加点击喂食/状态反馈（饱腹度等），用 NVS 持久化。
- **OTA 升级**：分区表已支持（factory 0x3B0000），加 `esp_https_ota` 组件 + Web 触发接口。
- **BLE 配网**：替代/补充 AP 配网，手机 APP 直连。

---

## 文档维护建议

- `docs/ecc/specs/` 的设计文档已落后于代码（如 weather 的 base_url NVS 覆盖是本轮新加的，spec 没提）。
  建议下次改功能时同步更新对应 spec，或加 `docs/ecc/specs/CHANGELOG.md` 记录偏离。
- `CLAUDE.md` 的 GPIO 表与 `app_config.h` 一致，但 EPD_SetWindows 调用参数顺序（X=0,Y=Width-1,Xend=Height-1,Yend=0）
  看起来反了但实测能工作——值得加注释说明 SSD1681 的 X/Y 地址空间约定，避免后人误改。
