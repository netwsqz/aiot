# 用户体验增强 设计文档

**日期**：2026-06-21
**状态**：设计已确认，待实现
**所属阶段**：Phase 1 增强

---

## 1. 目标与范围

修复深睡眠唤醒后的已知问题（待机刷新卡顿、TODO/计时器丢失），并新增自定义待机画面和 WiFi 凭据 Web 配置功能，提升整体用户体验。

**分三步渐进实现**：

| 步骤 | 内容 | 依赖 |
|---|---|---|
| Step 1 | Bug 修复：刷新优化 + NVS 初始化修复 + 计时器持久化 | 无 |
| Step 2 | Web 上传自定义待机画面 | Step 1 |
| Step 3 | WiFi 凭据 Web 配置 | Step 2（共用 Web 页面） |

**不做的事**：
- 不做自动定时唤醒采集
- 不做多屏/主题切换
- 不做数据看板（历史曲线等）
- 不做 OTA 升级
- 不做电池低电量自动睡眠

---

## 2. Step 1 — Bug 修复与持久化

### 2.1 待机画面刷新优化

**问题**：`StandbyScreen_Draw()` 逐像素调用 `EPD_DrawColorPixel()`，200×200 = 40000 次函数调用 + 位运算，耗时约 3-4 秒。

**方案**：直接将 `standby_bitmap[]` 数据 memcpy 到 EPD 驱动的内部 buffer，然后一次性发送。

```cpp
// standby_screen.cpp — 优化后
extern "C" void StandbyScreen_Draw(struct epaper_driver_display *epd)
{
    if (!epd) return;

    epd->EPD_Init();
    /* 不需要 EPD_Clear()：memcpy 覆盖全部 5000 字节 buffer，
     * 比 memset 0xff 再覆盖更高效。 */

    /* 直接写入 bitmap：黑色像素 = 0，白色像素 = 1
     * standby_bitmap 格式已经是 1bpp (1=白,0=黑)，与 EPD buffer 格式一致，
     * 可以直接 memcpy 覆盖 buffer 的前 5000 字节。 */
    extern const uint8_t standby_bitmap[5000];
    memcpy(epd->GetBuffer(), standby_bitmap, 5000);

    epd->EPD_Display();
}
```

**需要改动**：
- `epaper_driver_bsp.h`：新增 `uint8_t* GetBuffer()` 内联方法，返回 `buffer` 指针
- `standby_screen.cpp`：替换逐像素循环为 memcpy

**效果**：CPU 处理时间从 ~3-4s 降至 <1ms（EPD_Display 本身约 2s 不变）。

### 2.2 NVS 初始化修复 — TODO 消失问题

**问题**：深睡眠唤醒路径中，`TodoMgr_Init()` 依赖 NVS 已打开，但唤醒路径跳过了 `WifiBsp_Init()`（其中调用了 `nvs_flash_init()`）。如果 NVS 未初始化，`nvs_open()` 失败，TODO 列表为空。

**方案**：在 `app_main()` 唤醒路径的最开头，显式调用 `nvs_flash_init()`。

```cpp
// main.cpp — 唤醒路径开头
if (wake_cause == ESP_SLEEP_WAKEUP_EXT1) {
    ESP_LOGI(TAG, "=== Wake from deep sleep (EXT1) ===");

    // NVS 必须先初始化，TodoMgr / TimerEngine 持久化依赖它
    // nvs_flash_init() 是幂等的，已初始化时返回 ESP_OK
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // ... 其余唤醒恢复代码
}
```

**改动文件**：`main.cpp` — 唤醒路径增加 `nvs_flash_init()` 调用

### 2.3 计时器持久化（番茄钟 + 提醒）

**问题**：深睡眠后所有计时器丢失，唤醒后番茄钟和提醒列表为空。

**方案**：睡眠前将活跃计时器保存到 NVS，唤醒后根据 RTC 时间差恢复剩余时间。

#### 2.3.1 新增 NVS 存储

在 `timer_engine.c` 中新增：

```c
#define TIMER_NVS_NAMESPACE "timer"
#define TIMER_NVS_KEY       "slots"

// 保存格式：RTC 时间戳 + 计时器快照
typedef struct {
    uint8_t  id;
    uint8_t  kind;
    uint32_t total_seconds;
    uint32_t remaining_sec;
    uint8_t  active;
    uint8_t  _pad[3];        // 对齐
} timer_slot_persist_t;      // 12 bytes per slot

typedef struct {
    int64_t  rtc_timestamp;           // 保存时的 RTC unix 时间戳
    timer_slot_persist_t slots[TIMER_MAX_SLOTS];  // 48 bytes
} timer_persist_t;                    // 总共 56 bytes
```

#### 2.3.2 新增 API

```c
// 睡眠前调用：保存活跃计时器 + 当前 RTC 时间
esp_err_t TimerEngine_Persist(void);

// 唤醒后调用：从 NVS 恢复计时器，根据 RTC 时间差调整 remaining_sec
esp_err_t TimerEngine_Restore(void);
```

#### 2.3.3 保存逻辑（TimerEngine_Persist）

```c
esp_err_t TimerEngine_Persist(void)
{
    // 1. 获取当前 RTC 时间（秒级 unix timestamp）
    rtc_datetime_t dt = PortRtc_GetTime();
    struct tm tm = {0};
    tm.tm_year = dt.year - 1900;
    tm.tm_mon  = dt.month - 1;
    tm.tm_mday = dt.day;
    tm.tm_hour = dt.hour;
    tm.tm_min  = dt.minute;
    tm.tm_sec  = dt.second;
    int64_t now_sec = (int64_t)mktime(&tm);

    // 2. 快照所有活跃计时器
    timer_persist_t data = {0};
    data.rtc_timestamp = now_sec;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < TIMER_MAX_SLOTS; i++) {
        data.slots[i].id            = s_slots[i].id;
        data.slots[i].kind          = s_slots[i].kind;
        data.slots[i].total_seconds = s_slots[i].total_seconds;
        data.slots[i].remaining_sec = s_slots[i].remaining_sec;
        data.slots[i].active        = s_slots[i].active;
    }
    xSemaphoreGive(s_mutex);

    // 3. 写入 NVS
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(TIMER_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(nvs, TIMER_NVS_KEY, &data, sizeof(data));
    nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}
```

#### 2.3.4 恢复逻辑（TimerEngine_Restore）

```c
esp_err_t TimerEngine_Restore(void)
{
    // 1. 从 NVS 读取
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(TIMER_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    timer_persist_t data;
    size_t len = sizeof(data);
    err = nvs_get_blob(nvs, TIMER_NVS_KEY, &data, &len);
    if (err != ESP_OK) { nvs_close(nvs); return err; }

    // 2. 清除 NVS 中的数据（一次性恢复）
    nvs_erase_key(nvs, TIMER_NVS_KEY);
    nvs_commit(nvs);
    nvs_close(nvs);

    // 3. 计算时间差
    rtc_datetime_t dt = PortRtc_GetTime();
    struct tm tm = {0};
    tm.tm_year = dt.year - 1900;
    tm.tm_mon  = dt.month - 1;
    tm.tm_mday = dt.day;
    tm.tm_hour = dt.hour;
    tm.tm_min  = dt.minute;
    tm.tm_sec  = dt.second;
    int64_t now_sec = (int64_t)mktime(&tm);
    int64_t elapsed = now_sec - data.rtc_timestamp;
    if (elapsed < 0) elapsed = 0;  // RTC 走慢了？

    // 4. 恢复活跃计时器
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < TIMER_MAX_SLOTS; i++) {
        if (!data.slots[i].active) continue;

        uint32_t remaining = data.slots[i].remaining_sec;
        if ((int64_t)remaining <= elapsed) {
            // 睡眠期间已到期 → 视为已触发，不恢复
            continue;
        }
        remaining -= (uint32_t)elapsed;

        s_slots[i].id            = data.slots[i].id;
        s_slots[i].kind          = data.slots[i].kind;
        s_slots[i].total_seconds = data.slots[i].total_seconds;
        s_slots[i].remaining_sec = remaining;
        s_slots[i].active        = true;
    }
    // 恢复 next_id（取所有 slot id 的最大值 + 1）
    uint8_t max_id = 0;
    for (int i = 0; i < TIMER_MAX_SLOTS; i++) {
        if (s_slots[i].id > max_id) max_id = s_slots[i].id;
    }
    s_next_id = max_id + 1;
    if (s_next_id == 0) s_next_id = 1;

    xSemaphoreGive(s_mutex);
    return ESP_OK;
}
```

#### 2.3.5 main.cpp 调用位置

```cpp
// 进入深睡眠前
static void enter_deep_sleep(void)
{
    // ... 现有清理代码 ...

    TimerEngine_Persist();  // 保存计时器状态 ← 新增

    // ... 其余睡眠代码 ...
}

// 唤醒后
if (wake_cause == ESP_SLEEP_WAKEUP_EXT1) {
    nvs_flash_init();  // 确保 NVS 可用

    // ... 电源/I2C/EPD 恢复 ...

    TimerEngine_Init();     // 初始化引擎（创建 mutex + tick timer）
    TimerEngine_Restore();  // 从 NVS 恢复计时器 ← 新增

    // ... 其余恢复代码 ...
}
```

#### 2.3.6 边界情况

| 场景 | 处理 |
|---|---|
| 睡眠期间计时器到期 | `remaining_sec <= elapsed`，不恢复，相当于静默触发 |
| RTC 时间走偏（慢） | `elapsed < 0` 时钳位为 0，计时器剩余时间不变 |
| NVS 无持久化数据 | `TimerEngine_Restore()` 返回错误，不影响正常初始化 |
| 多次唤醒循环 | 每次睡眠前覆盖 NVS，恢复后立即清除，不会累积 |
| 计时器刚到期时进入睡眠 | 保存时 `remaining_sec=0`，`active=false`，不会被恢复 |

---

## 3. Step 2 — Web 上传自定义待机画面

### 3.1 方案概述

通过 WiFi AP Web 页面上传 200×200 单色 BMP/PNG 图片，存储到 SPIFFS 分区，睡眠时从 SPIFFS 加载显示。如果 SPIFFS 中无自定义画面，回退到编译期硬编码的默认位图。

### 3.2 存储方案 — SPIFFS

**分区表改动**：从 factory 分区末尾划出 256KB 给 SPIFFS。

```csv
# partitions.csv（修改后）
# Name,      Type, SubType, Offset,   Size,    Flags
nvs,         data, nvs,     0x9000,   0x6000,
phy_init,    data, phy,     0xf000,   0x1000,
factory,     app,  factory, 0x10000,  0x3B0000,
spiffs,      data, spiffs,  0x3C0000, 0x40000,
```

**存储文件**：`/spiffs/standby.bmp` — 200×200 单色 BMP，约 5.2KB（54 字节头 + 5000 字节像素数据）

### 3.3 Web 上传接口

新增 HTTP 端点：

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/standby` | 返回当前待机画面（BMP 文件） |
| POST | `/api/standby` | 上传新的待机画面（multipart/form-data） |
| DELETE | `/api/standby` | 删除自定义画面，恢复默认 |

**上传格式要求**：
- 200×200 像素，1-bit 单色 BMP
- 文件大小 ≤ 6KB
- 非法格式返回 400 错误

### 3.4 图片处理流程

```
用户上传 BMP
  → Web 后端校验尺寸和格式
  → 转换为 1bpp 像素数据（与 standby_bitmap 相同格式）
  → 写入 /spiffs/standby.bmp
  → 返回成功
```

### 3.5 StandbyScreen_Draw 改动

```cpp
extern "C" void StandbyScreen_Draw(struct epaper_driver_display *epd)
{
    if (!epd) return;

    epd->EPD_Init();

    uint8_t *buf = epd->GetBuffer();

    // 尝试从 SPIFFS 加载自定义画面
    bool loaded = standby_load_from_spiffs(buf, 5000);

    if (!loaded) {
        // 回退到编译期默认位图
        extern const uint8_t standby_bitmap[5000];
        memcpy(buf, standby_bitmap, 5000);
    }

    epd->EPD_Display();
}
```

### 3.6 新增文件

```
main/
├── standby_screen.h/cpp   改动：新增 SPIFFS 加载逻辑
components/todo_web/
├── todo_web.cpp           改动：新增 /api/standby 端点
```

### 3.7 SPIFFS 初始化

在 `app_main()` 中，冷启动和唤醒路径都需挂载 SPIFFS：

```cpp
esp_vfs_spiffs_conf_t conf = {};
conf.base_path = "/spiffs";
conf.partition_label = NULL;
conf.max_files = 2;
conf.format_if_mount_failed = true;
esp_vfs_spiffs_register(&conf);
```

---

## 4. Step 3 — WiFi 凭据 Web 配置

### 4.1 方案概述

在 WiFi AP Web 页面新增 WiFi 配置区域，用户输入 SSID 和密码后保存到 NVS，下次冷启动自动使用新凭据连接。

### 4.2 NVS 存储

使用现有 `wifi` NVS namespace（`WifiBsp_Init` 已在读取）：

| Key | 类型 | 说明 |
|---|---|---|
| `ssid` | string | WiFi SSID |
| `password` | string | WiFi 密码 |

### 4.3 Web 接口

新增 HTTP 端点：

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/wifi` | 返回当前 SSID（不返回密码）和连接状态 |
| POST | `/api/wifi` | 保存新凭据 `{ssid, password}`，自动重连 |

**POST 处理流程**：
1. 校验 ssid 非空，password 可空（开放网络）
2. 写入 NVS `wifi` namespace
3. 断开当前连接
4. 用新凭据重新连接
5. 返回连接结果

### 4.4 Web 页面改动

在现有 TODO Web 页面新增"设置"区域：
- SSID 输入框（预填当前值）
- 密码输入框
- "保存并连接" 按钮
- 连接状态显示

### 4.5 安全考虑

- 密码不通过 GET 接口返回
- 仅在 AP 模式下可配置（用户已物理接触设备）
- NVS 中密码明文存储（与现有 `wifi_bsp.c` 行为一致，ESP-IDF WiFi 配置本身就是明文）

---

## 5. 整体改动文件汇总

| 文件 | Step 1 | Step 2 | Step 3 |
|---|---|---|---|
| `main/main.cpp` | ✅ NVS init + Persist/Restore 调用 | ✅ SPIFFS 挂载 | |
| `main/standby_screen.cpp` | ✅ memcpy 优化 | ✅ SPIFFS 加载 | |
| `main/standby_screen.h` | | ✅ 新增加载函数声明 | |
| `components/epaper_driver_bsp/epaper_driver_bsp.h` | ✅ GetBuffer() | | |
| `components/timer_engine/timer_engine.h` | ✅ Persist/Restore API | | |
| `components/timer_engine/timer_engine.c` | ✅ Persist/Restore 实现 | | |
| `components/todo_web/todo_web.cpp` | | ✅ /api/standby | ✅ /api/wifi |
| `components/todo_web/web_page.h` | | ✅ 上传 UI | ✅ WiFi 配置 UI |
| `partitions.csv` | | ✅ 新增 spiffs 分区 | |
| `sdkconfig.defaults` | | ✅ SPIFFS 配置 | |

---

## 6. 错误处理与边界情况

| 场景 | 处理 |
|---|---|
| SPIFFS 挂载失败 | format_if_mount_failed=true，格式化后重试；仍失败则回退默认位图 |
| 上传文件格式错误 | 返回 400，不写入 SPIFFS |
| SPIFFS 空间不足 | 返回 507，提示用户 |
| WiFi 凭据错误 | NVS 已保存但连接失败，保持 AP 模式让用户重新配置 |
| 唤醒后 NVS 未初始化 | Step 1 修复：显式调用 nvs_flash_init() |
| 深睡眠期间计时器到期 | 不恢复，静默丢弃（睡眠期间无提醒能力） |
| SPIFFS 分区损坏 | format_if_mount_failed=true 自动格式化，丢失自定义画面但不影响功能 |

---

## 7. 测试策略

### Step 1 验证

```
□ 双击 PWR 进入睡眠 → 画面刷新明显变快（对比之前卡顿感）
□ 唤醒后 TODO 列表完整恢复
□ 唤醒后番茄钟继续倒计时（剩余时间正确扣除睡眠时长）
□ 唤醒后提醒继续倒计时
□ 睡眠期间计时器到期 → 唤醒后该计时器不出现
□ 多次循环：睡眠→唤醒→睡眠→唤醒，TODO 和计时器不丢失
```

### Step 2 验证

```
□ Web 上传 BMP → 睡眠时显示自定义画面
□ 删除自定义画面 → 睡眠时回退默认位图
□ 上传非法格式 → 返回 400 错误
□ SPIFFS 未挂载时 → 回退默认位图
```

### Step 3 验证

```
□ Web 配置新 WiFi 凭据 → 保存成功 → 自动重连
□ 错误密码 → 保存但连接失败 → AP 仍可用
□ 冷启动 → 使用 NVS 中的凭据连接
□ GET /api/wifi → 返回 SSID 但不返回密码
```
