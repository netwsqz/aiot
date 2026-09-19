# 续航优化 + 摆件模式 设计文档

**日期**：2026-06-20
**状态**：设计已确认，待实现
**所属阶段**：Phase 1 增强

---

## 1. 目标与范围

在现有常亮固件基础上，新增深睡眠续航模式：双击电源键进入睡眠（显示像素风摆件画面），单击电源键唤醒快速恢复。

**使用场景**：设备放在桌面，不需要持续刷新数据时进入低功耗摆件模式，延长电池续航。

**不做的事**：
- 不做定时自动唤醒采集数据
- 不做睡眠期间番茄钟/提醒到点唤醒
- 不做断电持久化（番茄钟/提醒重启即清空，与现有设计一致）
- 不做电池电量低自动睡眠
- 不做摆件动画（静态画面）

---

## 2. 关键决策

| 维度 | 决策 |
|---|---|
| 进入睡眠 | 双击电源键（GPIO18），500ms 内两次低电平 |
| 唤醒方式 | 单击电源键（GPIO18）或 BOOT 键（GPIO0） |
| 睡眠前画面 | 全刷一幅像素风角色摆件图，静态 |
| 睡眠期间 | 深睡眠，墨水屏保持画面，不采集数据 |
| 唤醒后 | 快速恢复：I2C → EPD → 传感器，跳过 WiFi/NTP |
| WiFi | 唤醒后不自动重连，按需由用户触发（进 TODO Web 等） |
| 番茄钟/提醒 | 睡眠后丢失（不持久化），唤醒后回到主屏 |
| 摆件画面 | 像素风角色，硬编码在固件中（200×200 黑白位图） |

---

## 3. 架构与组件

### 3.1 新增文件

```
main/
├── standby_screen.h/cpp   新增：摆件画面（像素角色 + 全刷逻辑）
└── main.cpp                改动：pwr_key_task + 睡眠/唤醒流程

components/ui/
└── ui_screen.c             改动：新增 standby 画面相关（可选，如果复用 LVGL）
```

### 3.2 摆件画面（standby_screen）

不依赖 LVGL（睡眠前 LVGL 已停掉），直接用 EPD 驱动画位图。

```c
// standby_screen.h
void StandbyScreen_Draw(epaper_driver_display *epd);
```

实现要点：
- 200×200 黑白位图，编译期硬编码为 `static const uint8_t` 数组
- 调用 `epd->EPD_Init()` → `epd->EPD_All_Clear()` → `epd->EPD_Display()` 写入 BW buffer
- 像素角色设计：一个简单的小动物/角色，放在画面中央偏上，底部留白
- 位图数据量：200×200/8 = 5000 字节（BW），可接受

### 3.3 电源键双击检测（pwr_key_task）

新增 FreeRTOS 任务，与现有 `boot_key_task` 并列：

```cpp
static void pwr_key_task(void *arg)
{
    uint32_t last_press = 0;
    uint8_t  press_count = 0;

    for (;;) {
        if (gpio_get_level(PWR_BUTTON_PIN) == 0) {  // pressed (low active)
            if (press_count == 0) {
                last_press = xTaskGetTickCount() * portTICK_PERIOD_MS;
                press_count = 1;
            } else if (press_count == 1) {
                uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
                if (now - last_press < 500) {
                    // Double-click detected
                    enter_deep_sleep();
                }
            }
            vTaskDelay(pdMS_TO_TICKS(50));  // debounce
        } else {
            // Reset if too long since first press
            if (press_count == 1) {
                uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
                if (now - last_press > 500) press_count = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}
```

### 3.4 进入深睡眠流程（enter_deep_sleep）

```cpp
static void enter_deep_sleep(void)
{
    // 1. Stop background tasks
    vTaskSuspend(data_task_handle);
    vTaskSuspend(boot_key_task_handle);

    // 2. Power off peripherals
    WifiBsp_Deinit();            // disconnect + stop WiFi
    PortTouch_Deinit();          // release I2C touch
    // SHTC3 / RTC / Battery — no active tasks, just stop I2C
    i2c_master_Deinit();

    // 3. Draw standby screen
    StandbyScreen_Draw(epd);

    // 4. Hold EPD power pins so screen stays on during deep sleep
    rtc_gpio_hold_en(EPD_PWR_PIN);
    rtc_gpio_hold_en(AUDIO_PWR_PIN);
    rtc_gpio_hold_en(VBAT_PWR_PIN);

    // 5. Configure wakeup sources
    esp_sleep_pd_config(ESP_PD_DOMAIN_MAX, ESP_PD_OPTION_AUTO);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

    const uint64_t wake_mask = (1ULL << PWR_BUTTON_PIN) | (1ULL << BOOT_BUTTON_PIN);
    esp_sleep_enable_ext1_wakeup_io(wake_mask, ESP_EXT1_WAKEUP_ANY_LOW);

    // Pull-up on wake pins
    rtc_gpio_pullup_en(PWR_BUTTON_PIN);
    rtc_gpio_pullup_en(BOOT_BUTTON_PIN);

    // 6. Go
    esp_deep_sleep_start();
}
```

### 3.5 唤醒后快速恢复

在 `app_main()` 开头检测唤醒原因：

```cpp
extern "C" void app_main(void)
{
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

    if (cause == ESP_SLEEP_WAKEUP_EXT1) {
        // Deep sleep wakeup — fast path
        uint64_t wake_pins = esp_sleep_get_ext1_wakeup_status();
        ESP_LOGI(TAG, "Wake from deep sleep, pins: 0x%llx", wake_pins);

        // Power on peripherals
        gpio_set_level(EPD_PWR_PIN, 0);
        gpio_set_level(AUDIO_PWR_PIN, 0);
        gpio_set_level(VBAT_PWR_PIN, 1);

        // Init I2C + EPD + sensors only (skip WiFi)
        i2c_master_Init();
        // EPD init + base image restore
        // LVGL init + ui_init()
        // PortShtc3_Init(), PortRtc_Init(), PortTouch_Init(), PortBattery_Init()
        // Start data_task, boot_key_task, pwr_key_task
        // SKIP: WifiBsp_Init(), NTP sync, WifiAp, TodoWeb

        return;  // app_main returns after starting tasks
    }

    // Cold boot — full init (existing code)
    ...
}
```

**关键**：唤醒后 EPD 需要重新初始化（SSD1681 在深睡眠后寄存器丢失），但墨水屏面板本身保持最后一帧画面。所以要先 `EPD_Init()` → `EPD_DisplayPartBaseImage()` 恢复基图 → 然后 LVGL 正常局刷。

### 3.6 线程模型变化

```
现有：
  data_task      (core 0, prio 3)
  boot_key_task  (core 1, prio 5)

新增：
  pwr_key_task   (core 1, prio 5)  ← 与 boot_key_task 同核同优先级
```

两个 key task 都在 core 1，轮询间隔分别是 30ms（pwr）和 50ms（boot），不会互相饿死。

---

## 4. 数据流与状态机

### 4.1 睡眠进入

```
双击 PWR
  → pwr_key_task 检测到
  → enter_deep_sleep()
    → 停 data_task / boot_key_task
    → 关 WiFi / Touch / I2C
    → StandbyScreen_Draw(epd)     ← 全刷摆件画面
    → rtc_gpio_hold_en(电源引脚)   ← 保持墨水屏供电
    → esp_deep_sleep_start()
```

### 4.2 唤醒恢复

```
单击 PWR / BOOT
  → EXT1 唤醒
  → app_main() 重新执行
  → 检测 ESP_SLEEP_WAKEUP_EXT1
  → 快速路径：
    → 开电源引脚
    → I2C + EPD + LVGL + 传感器
    → 启动 data_task / boot_key_task / pwr_key_task
    → 不连 WiFi
  → 主屏显示（时间来自 RTC，温湿度首次采集后更新）
```

### 4.3 冷启动 vs 热唤醒

| | 冷启动 | 热唤醒（深睡眠后） |
|---|---|---|
| 电源引脚 | 初始化 | 重新拉电平 |
| I2C | 初始化 | 初始化 |
| EPD | Init + Clear + BaseImage | Init + BaseImage（不 Clear，保留摆件画面直到 LVGL 首帧局刷） |
| LVGL | 初始化 | 初始化 |
| SHTC3/RTC/Touch/Battery | 初始化 | 初始化 |
| WiFi + NTP | 连接 + 同步 | **跳过** |
| WiFi AP + Web | 初始化 | **跳过** |
| Timer Engine | 初始化（空） | 初始化（空，不恢复） |
| data_task | 启动 | 启动 |
| boot_key_task | 启动 | 启动 |
| pwr_key_task | 启动 | 启动 |

---

## 5. 摆件画面设计

### 5.1 画面布局

```
┌──────────────────────────┐
│                          │
│         ░░░░░░           │
│        ░██████░          │
│        ░██░░██░          │   ← 像素角色
│        ░██████░          │     （中央偏上）
│        ░░██░░░░          │
│         ░░█░░░           │
│         ░░░░░░░          │
│                          │
│                          │
│        Zzz...            │   ← 小文字（底部）
│                          │
└──────────────────────────┘
```

### 5.2 位图生成

- 200×200 单色位图，用 Python/PIL 脚本生成 C 数组
- 存储在 `main/standby_screen_data.h`（`static const uint8_t standby_bitmap[5000]`）
- 编译期链接，无运行时开销

---

## 6. 错误处理与边界情况

| 场景 | 处理 |
|---|---|
| 睡眠前番茄钟在跑 | 直接丢弃（不弹提醒），唤醒后 engine 初始化为空 |
| 睡眠前有定时提醒 | 直接丢弃，唤醒后不恢复 |
| 睡眠前 WiFi AP 开着 | 关 AP → 关 WiFi → 睡眠。唤醒后 AP 不自动开启 |
| 睡眠期间 BOOT 被按下 | 和 PWR 一样唤醒，走快速恢复路径 |
| 唤醒后 RTC 时间不准 | 首次采集时 RTC 读数即为当前值（睡眠期间 RTC 继续走时） |
| 唤醒后触摸不响应 | 和冷启动一样初始化 FT6336，不受睡眠影响 |
| 快速恢复中 EPD 初始化失败 | read_busy 有 5s 超时保护，不会死锁 |
| 连续快速双击多次 | 第一次双击已进入睡眠，后续按键成为唤醒源 |
| 电池极低时进入睡眠 | 正常进入；唤醒时如果电池彻底耗尽，下次就是冷启动了 |

---

## 7. 资源与依赖

**新增内存占用**：
- `standby_bitmap`：5000 字节（Flash，非 RAM）
- `pwr_key_task` 栈：2KB（可复用 boot_key_task 的 4KB 配置）
- 无新增 FreeRTOS 定时器

**功耗预估**（参考 ESP32-S3 数据手册）：
- 深睡眠（RTC + 外部 32kHz）：~7μA
- 墨水屏静态保持：~0μA（画面保持不需要功耗）
- 电池 500mAh 理论待机：~8 年（实际受自放电限制，约数月）

**对现有模块的影响**：
- `main.cpp`：新增 `pwr_key_task`、`enter_deep_sleep()`、唤醒路径分支
- `wifi_bsp`：可能需要 `WifiBsp_Deinit()` 接口（当前没有）
- `port_touch`：可能需要 `PortTouch_Deinit()` 接口
- `i2c_bsp`：可能需要 `i2c_master_Deinit()` 接口
- 其他模块无影响

---

## 8. 测试策略

### 8.1 集成验证（真机）

```
□ 冷启动 → 正常进入主屏（现有功能不受影响）
□ 双击 PWR → 摆件画面出现 → 设备静默（串口无输出）
□ 单击 PWR → 唤醒 → 快速恢复主屏（时间/温湿度正常显示）
□ 单击 BOOT → 唤醒 → 快速恢复主屏
□ 唤醒后触摸正常（滑动切页面）
□ 唤醒后 RTC 时间正确（睡眠 1 分钟，时间走了约 1 分钟）
□ 唤醒后电池读数正常
□ 睡眠前设番茄钟 → 唤醒后番茄钟已清空（主屏右下无迷你显示）
□ 睡眠前设 3 个提醒 → 唤醒后提醒列表为空
□ 冷启动 → 双击睡眠 → 单击唤醒 → 再双击睡眠 → 再单击唤醒（多次循环）
□ 睡眠期间 BOOT 长按 → 唤醒（不会进 AP 模式，因为唤醒后 WiFi 没初始化）
□ 唤醒后手动进 TODO Web → 此时 WiFi 未连接，需处理（或提示用户）
```

### 8.2 不做的验证

- 不做功耗实测（无专业功耗仪）
- 不做长时间睡眠压力测试（如睡眠 24 小时）
- 不做电池耗尽边界测试

---

## 9. 实现顺序建议

1. **standby_screen** — 摆件位图数据 + 绘制函数
2. **pwr_key_task** — 双击检测 + enter_deep_sleep 框架
3. **Deinit 接口补齐** — WifiBsp_Deinit / PortTouch_Deinit / i2c_master_Deinit
4. **app_main 唤醒分支** — 检测 EXT1 唤醒 → 快速恢复路径
5. **main.cpp 接线** — 冷启动路径保持不变，启动 pwr_key_task
6. **集成测试** — 按 8.1 清单逐项验证
