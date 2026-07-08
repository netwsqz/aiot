# Bug 修复报告 — 2026-07-08（交付版 v2）

本轮对全项目（23 个组件，~69k LOC）做了一次完整审查 + 修复。
审查方式：5 个 cpp-reviewer 子代理并行覆盖（ai_engine / 网络层 / 计时器+深睡眠 /
天气+Pet+UI / BSP+main），每个 HIGH+ 发现再做人工交叉验证，去重后逐个修复并 `idf.py build` 验证。

**v2 复审**：cpp-reviewer + security-reviewer 二轮复审，发现并修复了 v1 遗留的
1 CRITICAL（深睡眠自删任务死代码）+ 1 CRITICAL/HIGH（天气配置跨核竞态，含 base_url/city_name 残留竞态）+ 1 MEDIUM（cached 标志提前置位）
+ 1 MEDIUM（city_name 校验用错常量）+ 1 CRITICAL（sdkconfig 凭证泄露入仓）。
对 3 个核心代码修复另跑 9-agent 对抗验证（每修复 3 个独立 skeptic 尝试反驳），详见文末「v2 复审追加修复」。

**编译状态**：`idf.py fullclean` + `idf.py build` 通过，`aiot.bin` 1.94MB（app 分区 50% 空闲），DIRAM 85.7% 用，无编译警告。
**测试说明**：ESP-IDF 嵌入式代码无主机 TDD（硬件 I2C/SPI/EPD 无法 mock），验证 = 编译通过 + 设备实测待做。

---

## CRITICAL 修复

### C1. `timer_engine` tick_cb 在 timer 守护任务里 portMAX_DELAY 等锁 → 死锁风险
`tick_cb` 跑在 FreeRTOS timer daemon 任务（优先级 2）。原代码 `xSemaphoreTake(s_mutex, portMAX_DELAY)`：
若 `data_task`（优先级 3）持锁被抢占，daemon 永远拿不到锁 → **所有软件定时器冻结**（不只是计时器，
WiFi AP 超时定时器等也停）。
**修复**：改为 `xSemaphoreTake(s_mutex, 0)` 非阻塞，取不到就跳过本 tick（最多 1s 漂移，无害）。
`components/timer_engine/timer_engine.c`

### C2. 深睡眠路径 I2C Touch 设备句柄泄漏
`i2c_master_Deinit()` 只移除 RTC/SHTC3 设备，但 FT6336 触摸设备由 `port_touch.c` 私有持有，
`i2c_del_master_bus()` 时该设备仍挂载 → ESP-IDF 行为未定义，每次深睡眠周期泄漏一次。
**修复**：新增 `PortTouch_Deinit()`，`main.cpp enter_deep_sleep()` 在 `i2c_master_Deinit()` 前显式调用。
`components/port_touch/port_touch.{c,h}`、`main/main.cpp`

### C3. `enter_deep_sleep` 可重入 + pwr_key_task 未停止 → 二次 teardown 崩溃
`pwr_key_task` 句柄未保存，`enter_deep_sleep()` 删不掉它；用户持续按 PWR 可在 teardown 中段再次触发。
**修复**：保存 `s_pwr_key_task_handle`，进 `enter_deep_sleep()` 第一步先删它 + `s_entering_sleep` 重入保护；
同时补 `WifiAp_Stop()`（APSTA 模式下先收 AP 再 deinit WiFi 驱动）+ 重置 `s_web_want_running`。
`main/main.cpp`

### C4. `weather_fetch` 手动刷新标志跨核竞态
`s_refresh_requested` 是 `volatile bool`，HTTP 任务置位、`data_task` check-then-clear 非原子，
双核 ESP32-S3 上可能丢请求。
**修复**：改用 `portMUX_TYPE` 临界区（`portENTER/EXIT_CRITICAL`）保护置位/清零。
`components/weather_fetch/weather_fetch.c`

### C5. `ai_engine` `ws_send_audio` mutex 提前释放 → 静态缓冲区被覆盖
`s_b64_mtx` 保护 `s_audio_b64` + `s_audio_json`，原代码在 `snprintf` 后就 give mutex，
但随后 `esp_websocket_client_send_text` 仍在用 `s_audio_json` —— 下一个 chunk 进来会覆盖正在发送的数据。
**修复**：把 `xSemaphoreGive` 移到 send 返回之后。
`components/ai_engine/ai_engine.c`（注：此组件 master 未编译，修复为将来启用做准备）

---

## HIGH 修复

### H1. EPD 对象每次唤醒泄漏 ~5KB PSRAM
唤醒路径 `new epaper_driver_display` 但从不 `delete` 旧对象，析构函数空 → 每次深睡眠唤醒泄漏一个对象 + PSRAM buffer。
**修复**：唤醒路径 `new` 前先 `delete epd`；析构函数 `heap_caps_free(buffer)`。
`components/epaper_driver_bsp/epaper_driver_bsp.cpp`、`main/main.cpp`

### H2. LVGL 锁非递归 → 回调内再 Lock 死锁
`lvgl_mux` 是普通 mutex。LVGL 事件回调在 `lvgl_handler_task`（已持锁）里执行，
回调内调 `Lvgl_Lock()` 会自死锁。原代码靠注释"do NOT lock"防，无编译保护。
**修复**：改 `xSemaphoreCreateRecursiveMutex` + `Take/GiveRecursive`。
`components/lvgl_port/lvgl_port.cpp`

### H3. `timer_engine` Restore 不校验 blob 长度
`nvs_get_blob` 后未检查 `len == sizeof(timer_persist_t)`，旧版本结构体可能读到部分初始化数据。
**修复**：长度不匹配返回 `ESP_ERR_INVALID_SIZE` 并忽略。同时修 `s_next_id` 溢出（max_id==255 时回绕到 0）：
仅统计 active 槽的 max，且 `(uint8_t)(max_id+1)` 显式回绕。
`components/timer_engine/timer_engine.c`

### H4. `timer_engine` `xTimerStart(..., 0)` 静默失败风险
0 超时下若 timer 命令队列瞬时满，1s tick 永不起。
**修复**：改 `pdMS_TO_TICKS(100)` 给点余量。
`components/timer_engine/timer_engine.c`

### H5. WiFi STA 断连后无自动重连
原代码重试 5 次后放弃，路由器重启/信号丢失后设备永久离线，直到用户手动按重连。
**修复**：MAX_RETRY 失败后起一个 30s 一次性 `esp_timer`，回调里重置重试预算 + `esp_wifi_connect()`；
正常重试路径会再次触发，形成自愈循环。Deinit 时先停+删该 timer 再 deinit WiFi 驱动。
`components/wifi_bsp/wifi_bsp.c`（新增 `esp_timer` 依赖到 CMakeLists）

### H6. `wifi_bsp` `s_connected` 跨任务无保护
事件任务写、data_task/HTTP handler 读，普通 bool。
**修复**：改 `volatile bool`（与 `wifi_ap.cpp` 用 `std::atomic<int>` 的做法对齐的 C 友好等价物）。
`components/wifi_bsp/wifi_bsp.c`

### H7. `wifi_bsp` SetCredentials 不校验长度
超长 SSID/密码写入 NVS，Reconnect 读回时 `nvs_get_str` 失败回退到 Kconfig 默认。
**修复**：`SetCredentials` 加 `>32` / `>64` 校验返回 `ESP_ERR_INVALID_ARG`。
`components/wifi_bsp/wifi_bsp.c`

### H8. 天气配置 Web 写 NVS 后不热加载
`handle_post_weather` 写 NVS 后只清缓存，`weather_fetch` 内存里的 `s_api_key`/`s_city_name` 仍是旧值，要重启才生效。
**修复**：新增 `WeatherFetch_ReloadConfig()`（重读 NVS 到静态变量），Web 写入后立即调用；
同时支持 `base_url` 通过 NVS 覆盖（QWeather 2026 改了 per-developer host，否则改 host 要重编固件）。
`components/weather_fetch/weather_fetch.{c,h}`、`components/todo_web/todo_web.cpp`

### H9. `weather_ui` 用非线程安全 `localtime()`
`data_task` 和 LVGL 任务都可能调，共享静态 `struct tm` 会损坏。
**修复**：改 `localtime_r(&ft, &t_buf)`。
`components/weather_ui/weather_ui.c`

### H10. `weather_ui` 不判空 `daily[0]`
QWeather 3d 若只返回今天，`parse_daily` 跳过今天后 count=0，UI 渲染 "明日  0/0"。
**修复**：`text_day[0]` 为空时显示 "明日 --"。
`components/weather_ui/weather_ui.c`

### H11. `main.cpp` `s_weather_snapshot` 跨任务无锁写入
`WeatherFetch_Update` 在锁外，`s_weather_snapshot = snap`（~300B 非原子）与锁内 `ui_update_weather_summary` 读竞态。
**修复**：把结构体拷贝移入 `Lvgl_Lock` 内。
`main/main.cpp`

### H12. `ai_engine` `s_ws_connected` 无锁跨核读写
WS client 任务写、audio_stream_task/Tick 读。
**修复**：改 `volatile bool`。另修 `auth_header` 栈缓冲区悬垂（改 static）、
`s_audio_task_running` 在 `xTaskCreate` 前置位（避免 Tick 误判任务死亡提前 finish）、
LLM abort 路径不置 `s_llm_received` 导致 Tick 空 etc 等 30s、WAITING_LLM 双重 `history_push`/回调、
`json_escape` 不转义 `\n\r\t`、`json_get_str_in` 不处理转义引号、LLM body 截断后 JSON 不闭合。
`components/ai_engine/ai_engine.c`（master 未编译，备用）

### H13. `todo_web` HTTP 状态码全映射成 400
`send_json` 把所有非 200 一律设 "400 Bad Request"，404/500 客户端无法区分。
**修复**：按 status_code 映射 200/400/404/500 正确状态行。
`components/todo_web/todo_web.cpp`

### H14. `todo_web` POST /api/wifi 不校验长度
SSID/密码超长写入 NVS。
**修复**：加 `>32`/`>64` 校验 400。
`components/todo_web/todo_web.cpp`

### H15. `todo_web` TODO 端点字段不校验长度
name/icon/text/due 超长被 `strncpy` 静默截断（可能截断 UTF-8）。
**修复**：create/update list/task 各字段加 `strlen > LEN-1` 校验 400。
`components/todo_web/todo_web.cpp`

### H16. `todo_web` `max_uri_handlers=6` 恰好满额
6 个 handler 用满，无余量，未来扩展静默失败。
**修复**：改 8。
`components/todo_web/todo_web.cpp`

### H17. 唤醒路径未设默认页面 + 未重置 web 状态
唤醒后首屏不确定（取决于 ui_init 默认），且 `s_web_is_running` 等保留冷启动旧值。
**修复**：唤醒路径末尾加 `ui_switch_page(PAGE_PET)`；`enter_deep_sleep` 重置 `s_web_want_running`。
`main/main.cpp`

---

## MEDIUM 修复

- **M1** `todo_manager` `load_*_from_nvs` 不判 `valuestring` NULL → 损坏 NVS 数据会 `strncpy(NULL)` 崩溃。加 NULL 判断 + NUL 兜底。`components/todo_manager/todo_manager.cpp`
- **M2** `port_battery` `adc_cali_raw_to_voltage` 返回值未检查，不支持曲线拟合的芯片返回 0V。加失败回退粗略估算。`components/port_battery/port_battery.c`
- **M3** `i2c_bsp` `i2c_write_buff` `malloc` 未判 NULL。加 `ESP_ERR_NO_MEM` 返回。`components/i2c_bsp/i2c_bsp.c`
- **M4** `main.cpp` `s_last_activity_tick` 跨任务读写无保护。改 `volatile uint32_t`。`main/main.cpp`
- **M5** `timer_engine` `s_fired_buf` 回调前可能被覆盖（理论）：1s 间隔下风险低，加注释说明，不改架构。`components/timer_engine/timer_engine.c`

---

## 未修（评估后判断为低风险或设计取舍）

- **API key 在 URL query**：QWeather v7 用 subdomain host 认证，`key=` 参数冗余但移除需实测 QWeather 是否仍要；
  HTTPS 下 URL 不进 ESP_LOG，风险可控。建议后续迁移到 header。
- **AP 开放无密码 + HTTP 无认证**：嵌入式配网常见取舍；连入 AP 需物理接近，且配网窗口 15min 自动关。
  建议后续加可选 AP 密码 + 配网确认按钮。
- **`pet_ui` 147KB PSRAM canvas 无 Deinit**：深睡眠 CPU 重启，PSRAM 分配随进程消失，无实际泄漏。
- **CORS `*`**：AP 局域网内，限制 Origin 收益有限。
- **`epd` SPI 总线/设备句柄未在析构释放**：总线由 app 拥有且跨组件共享，析构里 free 会破坏其他使用者；
  仅释放自有 buffer（已做）。
- **`s_indoor_temp_c` float 跨任务**：4 字节单次读写原子，pet/weather 都只读不写共享，加 volatile 收益低、改动大，暂不动。

---

## 验证

- `idf.py fullclean` + `idf.py build` ✅ 通过（v1 修复了 3 个编译错误：`due` 重复声明、`esp_timer.h` 缺包含、`wifi_bsp` 缺 `esp_timer` 依赖；v2 追加修复无新编译错误）
- `idf.py size`：app 1.94MB / 分区 3.76MB（50% 空闲），DIRAM 85.7%（49KB 剩余），IRAM 100%（正常，cache 用）
- 无编译警告（扫描 build/log 已确认）
- **设备实测待做**：以下场景需烧录后在真机验证
  - 双击 PWR 进/出深睡眠循环（验证 C2/C3/H1 + v2-C6 不再泄漏/崩溃/死代码）
  - WiFi 断开后 30s 自动重连（H5）
  - Web 改天气配置后立即生效（H8 + v2-C7 配置竞态不再产生撕裂 key）
  - 计时器到点 + 多个同时到点（C1/H4 队列）
  - TODO API 各字段超长返回 400（H14/H15）
  - Web 设置城市名 16–31 字符（v2-M6 不再被误拒）

## 风险

- `ai_engine` 修复（C5/H12）在 master 未编译，仅静态修复，启用 AI 时需实测 ASR/LLM 链路。
- WiFi 自动重连定时器在 `WifiBsp_Deinit` 中先停再 deinit，但极端时序下 timer 回调与 deinit 仍有窗口——
  已用 unregister event handler + stop/delete timer 顺序覆盖，实测建议在路由器重启场景观察。
- **v2 新增**：sdkconfig 中的真实 WiFi 凭证已从仓库移除（sdkconfig.defaults 改占位符 + sdkconfig 加入 .gitignore），
  但**历史提交中仍存在泄露的凭证**（origin/main、origin/master 均含）。需人工轮换 WiFi 密码并用 BFG/filter-branch 清史。
  见 v2-C8。

---

## v2 复审追加修复（cpp-reviewer + security-reviewer 二轮）

v1 交付后做了一次独立复审（cpp-reviewer 覆盖并发/深睡眠/内存；security-reviewer 覆盖 HTTP/NVS/WiFi/凭证），
发现 v1 引入或遗漏的 5 个问题，全部已修并 `idf.py build` 通过：

### C6. `enter_deep_sleep()` 自删当前任务 → teardown 成死代码（CRITICAL，v1 引入）

v1 的 C3 修复在 `enter_deep_sleep()` 开头加了 `vTaskDelete(s_pwr_key_task_handle)`，
但 `enter_deep_sleep()` 本身就跑在 `pwr_key_task` 里。FreeRTOS 对自删任务在**下一次 yield**
（`vTaskDelay`/`vTaskSuspend`）时放弃其执行上下文，调度器不再返回该任务。
后果：`vTaskDelay(50ms)` 之后的所有 teardown（`Lvgl_PortDeinit` / `WifiBsp_Deinit` /
`i2c_master_Deinit` / `StandbyScreen_Draw` / `esp_deep_sleep_start`）**永远不会执行**，
设备卡在唤醒态耗电，不进深睡眠。v1 注释里"deferred to idle task... still runs during vTaskDelay"
是对 FreeRTOS 语义的误解——idle task 只回收 TCB/栈内存，不会续跑被删任务的代码。

**修复**：移除 `vTaskDelete(s_pwr_key_task_handle)` 整段，改为 `(void)s_pwr_key_task_handle;` + 注释。
防二次进入完全靠已有的 `s_entering_sleep` 重入保护（`pwr_key_task` 二次双击会命中 guard 直接 return）。
`esp_deep_sleep_start()` 本身会终止所有任务，无需手动删 pwr_key_task。
`main/main.cpp`

**复审备注（9-agent 对抗验证，5/5 判 CORRECT/PARTIAL，无 INCORRECT）**：`s_entering_sleep` 是
普通 `volatile bool` 的 check-then-set，非原子、无临界区。当前安全**仅因** `enter_deep_sleep`
只有一个调用者（`pwr_key_task`，pin 在 Core 1），单任务不会与自身并发。若未来新增第二个调用者
（如 boot_key_task、定时器、中断），该 guard 会退化为 TOCTOU。已在注释中标注该不变量。

### C7. `WeatherFetch_ReloadConfig` 跨核写配置缓冲区无锁 → 撕裂 key（CRITICAL/HIGH，v1 H8 引入）

v1 的 H8 修复加了 `WeatherFetch_ReloadConfig()` 让 Web 改配置立即生效，但它直接往
`s_api_key`/`s_location_id`/`s_city_name`/`s_base_url` 四个静态 char 数组写值，无任何同步。
写者跑在 HTTP server 任务，读者（`WeatherFetch_Update` 里的 `snprintf` 拼 URL）跑在 data_task，
双核 ESP32-S3 上可能读到半更新的 key（如旧 `abc123` 被新 `xyz789` 覆盖到一半 → `xyz123`），
导致天气请求用错凭证、QWeather 返回鉴权失败或拉到错城市数据。

**v2 首轮修复**：新增 `s_config_spinlock`（`portMUX_TYPE`）。
- `ReloadConfig`：先把 NVS 读到栈局部（局部先用当前值 seed，保证缺 key 时保持原值），
  再在临界区内把四个局部一次性 `memcpy` 发布到静态缓冲区。NVS 读在临界区外（避免长时间关中断）。
- `WeatherFetch_Update`：进入后在临界区内把 `s_api_key`/`s_location_id` 快照到栈局部，
  之后用局部拼 URL，与写者互斥。

**v2 复审发现残留竞态（9-agent 对抗验证，6/6 指出）**：首轮只快照了 `s_api_key`/`s_location_id`，
但 `s_base_url`（`http_get_json` 在一次 Update 的 3 次 HTTPS 请求里裸读，窗口达数秒）和
`s_city_name`（`parse_now` 裸拷进 `snapshot->now.city_name`）仍无锁。`ReloadConfig` 可在多秒的
fetch 窗口中途换 host，导致同一 Update 的 /now、/24h、/3d 命中不同 host；`city_name` 也可能撕裂。

**v2 终版修复**：`WeatherFetch_Update` 进入后在临界区内把**全部四个**配置缓冲区快照到栈局部，
再发布到 per-Update 作用域全局 `s_fetch_base_url`/`s_fetch_city_name`（`s_fetch_in_progress` 置位）。
`http_get_json` 与 `parse_now` 改为读这些 per-Update 快照（`s_fetch_in_progress` 为真时）而非 live statics，
保证一次多秒、3 端点的 fetch 全程用同一 config epoch。Update 在所有出口路径（成功 + 三处失败 goto）
都清 `s_fetch_in_progress`。临界区只含 `memcpy`（≤192B），不持锁调阻塞 API，portENTER_CRITICAL 安全。
`components/weather_fetch/weather_fetch.c`

### M6. `todo_web` city_name 校验用错常量 → 合法城市名被误拒（MEDIUM，v1 遗留）

`POST /api/weather/config` 的 `city_name` 长度校验用的是 `WEATHER_TEXT_LEN - 1`（15），
但配置缓冲区 `s_city_name` 实际是 `WEATHER_CITY_NAME_LEN`（32）。导致 16–31 字符的合法城市名
（如 "Xigaze Prefecture"）被 400 拒绝，而缓冲区本放得下。
（注：`weather_now_t.city_name` 仍是 16 字节，`weather_fetch.c` 的 `safe_strcpy` 会截断显示用，
但配置存储层应允许满 31 字符。）

**修复**：改用 `WEATHER_CITY_NAME_LEN - 1`。
`components/todo_web/todo_web.cpp`

### M7. `s_weather_has_cached` 在锁外提前置位 → 单帧陈旧快照（MEDIUM，v1 H11 残留）

v1 的 H11 把 `s_weather_snapshot = snap` 移进了 `Lvgl_Lock`，但 `s_weather_has_cached = true`
仍留在锁外、在拷贝之前。读者可能看到 `cached==true` 但快照还是上一轮的旧值，渲染一帧空/旧数据。

**修复**：把 `s_weather_has_cached = true` 也移进 `Lvgl_Lock` 块、放在 `s_weather_snapshot = snap` 之后，
flag 与快照成对更新。`Lvgl_Lock` 已是递归锁（H2），不会因回调重入而死锁。
`main/main.cpp`

**复审备注**：另有两处（冷启动 `app_main` 行 872、唤醒路径行 711）也写 `s_weather_has_cached = WeatherCache_Load(&s_weather_snapshot)`，
均在 `Lvgl_Lock` 之外。复审判定为**当前安全**：这两处都在 `data_task`（唯一并发读者）创建之前执行，
不存在并发读写，紧随其后的 `WeatherUI_Render` 也在 `Lvgl_Lock(-1)` 内。仅当未来调整 `app_main`
初始化顺序、把 `data_task` 提前到 weather cache load 之前才会成为竞态——已在代码注释中标注该不变量。

### C8. sdkconfig 真实 WiFi 凭证入仓并推到 origin（CRITICAL，安全，历史遗留）

`sdkconfig`（被 git 跟踪）和 `sdkconfig.defaults` 都含真实 WiFi SSID/密码
（`CMCC-fAPM` / `sgps6663`），且已推到 `origin/main` 和 `origin/master`。
任何有仓库读权限者均可获取家庭/办公网络凭证。

**修复（已完成）**：
1. `sdkconfig.defaults` 的 WiFi 默认值改为占位符 `YOUR_SSID_HERE` / `YOUR_PASSWORD_HERE`，并加注释说明真实值只走 menuconfig/本地 sdkconfig。
2. `.gitignore` 加入 `sdkconfig`（仅保留 `sdkconfig.defaults` 跟踪）。
3. `components/wifi_bsp/Kconfig` 的 default 本就是 `your_ssid`/`your_password` 占位符，无需改。

**修复（需人工执行，不在代码改动范围）**：
4. **立即在路由器上轮换该 WiFi 密码**（CMCC-fAPM 的 `sgps6663`）。
5. 用 BFG Repo-Cleaner 或 `git filter-repo` 从全历史清除 `sdkconfig` 中的凭证，
   然后 force-push 重写 origin/main 与 origin/master（会改变所有提交哈希，需协调协作者）。
6. QWeather API key 在 sdkconfig 中为空（`""`），无泄露；location_id/city_name 是公开城市编码，非敏感。

> ⚠️ 仓库当前对匿名访问返回 404（疑似 private 或已删），但**不能据此判定安全**——
> 协作者、fork、缓存镜像、未来改 public 都会暴露已入仓的凭证。轮换密码是唯一可靠补救。
