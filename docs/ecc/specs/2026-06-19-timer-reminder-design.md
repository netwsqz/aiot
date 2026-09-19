# 番茄钟 + 定时提醒 设计文档

**日期**：2026-06-19
**状态**：设计已确认，待实现
**所属阶段**：Phase 1 增强（在 TODO 模块基础上扩展）

---

## 1. 目标与范围

在已有 ESP32-S3 1.54" 墨水屏固件（四象限主屏 + TODO 模块）基础上，新增两个独立功能：

- **番茄钟**：单次模式，预设 15 / 25 / 50 分钟可选
- **定时提醒**：预设 5 / 10 / 15 / 30 / 45 / 60 分钟后提醒，最多 3 个并发

使用场景：放在桌上工作时使用，设备插电常亮。

**不做的事**：
- 不做循环番茄钟、不做可配置任意时长
- 不做绝对时间点闹钟（"15:30 叫我"）
- 不做声音 / 蜂鸣 / LED 提示（仅墨水屏视觉）
- 不做断电恢复（重启即清空）
- 不引入深睡眠 / RTC 闹钟唤醒（保持纯常亮）

---

## 2. 关键决策

| 维度 | 决策 |
|---|---|
| 入口 | 点主屏左上时间区域 → "时间工具" 子页 → 番茄钟 / 定时提醒 |
| 计时引擎 | FreeRTOS 软件定时器，每秒 tick |
| 电源策略 | 纯常亮，不睡眠，不持久化 |
| 番茄钟模式 | 单次，全局只能跑 1 个 |
| 番茄钟时长预设 | 15 / 25 / 50 分钟 |
| 定时提醒预设 | 5 / 10 / 15 / 30 / 45 / 60 分钟 |
| 并发提醒上限 | 3 个 |
| 主屏显示 | 番茄钟运行时，右下象限显示 🍅 + 剩余分钟 + 进度条；定时提醒不在主屏显示 |
| 屏幕刷新粒度 | 每分钟刷新一次（数字与进度条整分钟前进） |
| 到点提醒 | 满屏覆盖页（信息丰富：图标 + "时间到" + 类型 + 时长 + 当前时间） |
| 解除方式 | 点屏幕任意处 |
| 多个同时到点 | FIFO 排队，逐个弹 |

---

## 3. 架构与组件

### 3.1 组件划分

```
components/
├── timer_engine/      新增：纯逻辑计时引擎（无 UI 依赖）
├── timer_ui/          新增：番茄钟/提醒页面 + 满屏到点提示
└── ui/                改动：左上点击事件 + 右下迷你显示 + 新增页面枚举
```

切分原则与现有 `todo_manager` + `todo_ui` 一致：engine 不知道 UI，UI 不知道时间怎么走。

### 3.2 timer_engine 接口

纯 C，无 LVGL 依赖，可独立测试。

```c
typedef enum {
    TIMER_KIND_POMODORO,
    TIMER_KIND_REMINDER,
} timer_kind_t;

typedef struct {
    uint8_t       id;             // 内部分配，1..255
    timer_kind_t  kind;
    uint32_t      total_seconds;
    uint32_t      remaining_sec;
    bool          active;
} timer_instance_t;

typedef void (*timer_fired_cb_t)(const timer_instance_t *t);

esp_err_t TimerEngine_Init(void);
esp_err_t TimerEngine_StartPomodoro(uint32_t minutes, uint8_t *out_id);
esp_err_t TimerEngine_StartReminder(uint32_t minutes, uint8_t *out_id);
esp_err_t TimerEngine_Cancel(uint8_t id);
esp_err_t TimerEngine_GetAll(timer_instance_t *out, uint8_t *count);
const timer_instance_t *TimerEngine_GetPomodoro(void);  // 唯一活跃番茄钟，NULL=无
void TimerEngine_SetFiredCallback(timer_fired_cb_t cb);
```

**实现要点**：
- 一个 FreeRTOS 软件定时器，周期 1 秒
- 内部静态数组：`MAX_POMODORO=1`、`MAX_REMINDER=3`，共 4 个槽位
- `engine_mutex` 保护数组（tick 任务写、UI 任务读）
- tick 中 remaining 减到 0 → active=false → 释放锁后调用 fired_cb（避免持锁回调）

**返回值约定**（与 `todo_manager` 风格一致）：

| 错误码 | 场景 |
|---|---|
| `ESP_OK` | 成功 |
| `ESP_ERR_INVALID_ARG` | minutes=0 或 minutes>180 |
| `ESP_ERR_NO_MEM` | 槽位已满 |
| `ESP_ERR_NOT_FOUND` | Cancel 时 id 不存在 |
| `ESP_ERR_INVALID_STATE` | Init 之前调用 |

### 3.3 timer_ui 接口

```c
esp_err_t  TimerUi_Init(void);
lv_obj_t  *TimerUi_GetToolsScreen(void);     // 时间工具子页
lv_obj_t  *TimerUi_GetPomodoroScreen(void);
lv_obj_t  *TimerUi_GetReminderScreen(void);

// engine 的 fired_cb 在系统 timer 任务中执行，不能直接操作 LVGL
// 这个函数只 push 到队列，等 LVGL 任务消费
void TimerUi_PushFiredEvent(uint8_t id, timer_kind_t kind);

// 由 data_task 在 LVGL 锁内每秒调用，刷新当前页面 + 主屏右下
void TimerUi_TickUpdate(void);
```

### 3.4 ui/（dashboard）改动

- `cont_top_left`：加 `LV_OBJ_FLAG_CLICKABLE` + 点击事件 → `ui_switch_page(PAGE_TIMER_TOOLS)`
- `cont_bottom_right`：改成动态区域；无番茄钟运行时维持 "Image" 占位（与现状一致），番茄钟运行时替换为 🍅 + 剩余分钟 + 迷你进度条
- `ui_page_t` 增加：`PAGE_TIMER_TOOLS`、`PAGE_POMODORO`、`PAGE_REMINDER`、`PAGE_FIRED_ALERT`

### 3.5 main.cpp 改动

- `app_main` 中调用 `TimerEngine_Init()` 与 `TimerUi_Init()`
- 注册 `TimerEngine_SetFiredCallback(...)`，回调内只调 `TimerUi_PushFiredEvent`
- `data_task` 每秒在 `Lvgl_Lock(500)` 内调用 `TimerUi_TickUpdate()`（仍由现有循环驱动，不新建任务）

---

## 4. 数据流与状态机

### 4.1 启动到结束

```
点左上时间区
  → ui_switch_page(PAGE_TIMER_TOOLS)
    → 显示工具子页（两个入口）

点 "番茄钟"
  → ui_switch_page(PAGE_POMODORO)
    → 显示番茄钟页（15/25/50 三个预设）

点 "25 min"
  → TimerEngine_StartPomodoro(25, &id)
    → engine：分配槽位、active=true、remaining=1500
    → 软件定时器开始 tick

每秒 tick：
  engine: remaining_sec--
  data_task: Lvgl_Lock → TimerUi_TickUpdate()
    → 当前在番茄钟页：刷新进度条+数字
    → 当前在主屏：刷新右下迷你显示
    → 仅在"整分钟变化"时实际触发屏幕局刷

remaining_sec == 0：
  engine: active=false → fired_cb(t)
    → TimerUi_PushFiredEvent → 入队
  → data_task 下一秒消费队列 → 切到 PAGE_FIRED_ALERT 显示满屏

点屏幕任意处：
  → s_alert_showing = false
  → 切回原页面
  → 队列还有则立即弹下一个
```

### 4.2 满屏提醒队列

```c
static QueueHandle_t s_fired_queue;   // 容量 4
static bool          s_alert_showing; // 当前是否在显示
```

- `TimerUi_PushFiredEvent` 总是入队
- `data_task` 每秒检查：`!s_alert_showing && 队列非空` → pop + 显示 + 置 true
- 用户解除后置 false，下一秒自动消费下一个

### 4.3 屏幕刷新策略

| 场景 | 刷新频率 |
|---|---|
| 番茄钟页倒计时 | 每分钟 1 次 |
| 主屏右下迷你显示 | 每分钟 1 次 |
| 满屏到点提醒 | 弹出/解除各 1 次 |
| 现有时钟 / 温湿度 | 维持现状（10s / 30s） |

虽然 engine 每秒 tick，但 `lv_label_set_text` 仅在分钟边界调用，避免触发不必要的局刷。

### 4.4 线程模型

```
FreeRTOS 软件定时器（系统 timer 任务）
  每秒：拿 engine_mutex → 减 remaining → 释放
        → 若到 0，调用 fired_cb
fired_cb 不在 LVGL 上下文！绝对不能直接操作 lv_obj
        ↓
TimerUi_PushFiredEvent → xQueueSend
        ↓
data_task（已有，每秒一次）
  Lvgl_Lock(500)
    → 检查 fired_queue → 弹出展示
    → TimerUi_TickUpdate（刷 UI）
  → Unlock
```

**锁顺序**：先 LVGL 锁 → 再 engine_mutex。timer_engine 内部只锁 engine_mutex，不会反向。

---

## 5. UI 详细布局

### 5.1 时间工具子页（PAGE_TIMER_TOOLS）

```
┌────────────────────┐
│      时间工具       │
│                    │
│   ┌──────────────┐ │
│   │   🍅 番茄钟   │ │
│   └──────────────┘ │
│                    │
│   ┌──────────────┐ │
│   │   ⏰ 定时提醒  │ │
│   └──────────────┘ │
│                    │
│        ← 返回       │
└────────────────────┘
```

- 两个大按钮垂直堆叠，宽 ~180、高 ~60
- 返回按钮复用 `todo_ui` 样式
- 支持右滑手势返回（与 todo_ui 一致）

### 5.2 番茄钟页（PAGE_POMODORO）

**未运行**：

```
┌────────────────────┐
│      🍅 番茄钟      │
│                    │
│   选择时长          │
│  ┌────┬────┬────┐  │
│  │ 15 │ 25 │ 50 │  │
│  └────┴────┴────┘  │
│      分钟           │
│                    │
│        ← 返回       │
└────────────────────┘
```

**运行中**（点 "25" 后）：

```
┌────────────────────┐
│      🍅 番茄钟      │
│                    │
│       24 min       │   ← montserrat_28
│                    │
│  ████░░░░░░░░░░░░  │   ← 宽 160，每分钟前进 1/25
│                    │
│   ┌──────────────┐ │
│   │     取消      │ │
│   └──────────────┘ │
│        ← 返回       │
└────────────────────┘
```

- 番茄钟全局只能跑 1 个；运行中再回此页直接显示运行中状态
- "取消"立即停止 → 不弹满屏提醒 → 回到选时长状态
- "返回" / 右滑返回主屏，番茄钟仍在后台跑

### 5.3 定时提醒页（PAGE_REMINDER）

```
┌────────────────────┐
│     ⏰ 定时提醒     │
│                    │
│  ┌────┬────┬────┐  │
│  │ 5  │ 10 │ 15 │  │
│  ├────┼────┼────┤  │
│  │ 30 │ 45 │ 60 │  │
│  └────┴────┴────┘  │
│      分钟后提醒     │
│                    │
│  当前提醒（2/3）    │
│  • 还剩 5 min      │
│  • 还剩 30 min     │
│                    │
│        ← 返回       │
└────────────────────┘
```

- 6 个预设按钮 2×3 排列
- 已挂列表用"还剩 N min"措辞，与番茄钟一致
- 已挂满 3 个时所有预设按钮置灰禁用
- **取消已挂提醒**：点列表项 → 弹"× 取消？" 小气泡按钮 → 确认后从队列移除
- 列表显示每分钟更新一次（跟着 remaining 递减）

### 5.4 满屏到点提醒（PAGE_FIRED_ALERT）

```
┌────────────────────┐
│                    │
│         ⏰         │
│       时间到        │  ← montserrat_28
│                    │
│      🍅 番茄钟      │  ← chinese_14
│   25 分钟已完成     │
│                    │
│      14:57          │  ← 当前实际时间
│                    │
│   点屏幕任意处关闭   │
└────────────────────┘
```

- 整屏 CLICKABLE，点击即解除
- 解除后：
  - 番茄钟到点 → 回到番茄钟页（已变回未运行）
  - 定时提醒到点 → 回到刚才的页面（DASHBOARD 或 REMINDER）
  - 队列还有 → 立即弹下一个

### 5.5 主屏右下角运行中显示

未运行（与现状一致）：保持 "Image" 占位。

番茄钟运行中：

```
┌────────┐
│  🍅    │
│ 24 min │
│ ████░░ │
└────────┘
```

100×100 象限内：图标 + 数字（montserrat_20）+ 迷你进度条（宽 ~80）。

---

## 6. 错误处理与边界情况

| 场景 | 处理 |
|---|---|
| 番茄钟运行中再进番茄钟页 | 直接显示"运行中状态"，不允许重复启动 |
| 已挂 3 个提醒再设第 4 个 | 6 个预设按钮置灰禁用 |
| 番茄钟运行中点取消 | 立即停止，不弹满屏提醒，回到选时长状态 |
| 已挂提醒点列表项 | 弹"× 取消？"气泡，确认后移除 |
| 满屏提醒弹出时新到点事件 | 入 fired_queue，当前 alert 解除后立即弹下一个 |
| 满屏提醒时主屏右下/子页面被切走 | 不可能。alert 通过 `lv_screen_load` 接管整屏，触摸事件被 alert 接收 |
| 主屏外取消番茄钟 | engine 的 cancel 应触发 UI 刷新通知，主屏右下变回 Image |
| NTP 时间被修正（往前/往后跳） | 不影响。计时基于 remaining 减秒，与 wall-clock 解耦 |
| 番茄钟跑一半设备掉电 | 重启后清空（不持久化） |
| BOOT 长按进 AP 模式时番茄钟在跑 | 互不干扰；timer_engine 继续 tick |
| LVGL 锁竞争超时 | `Lvgl_Lock(500)` 超时后跳过本秒刷新，下一秒再来 |
| 番茄钟到点瞬间用户按取消 | cancel 检查 active 标志，已 inactive 直接 ESP_OK 返回 |

---

## 7. 资源与依赖

**新增内存占用**：
- timer_engine：~64 字节静态数据 + 1 个 FreeRTOS 软件定时器
- timer_ui 4 个页面：约 8KB LVGL 对象（参考 todo_ui 规模），按需创建
- fired_queue：4 项 × 8 字节 = 32 字节

**屏幕寿命影响**：番茄钟整分钟刷新 1 次。以 25 min 番茄钟为例，单次约 25 次局刷，按官方寿命 100 万次局刷，相当于 0.0025% 寿命。

**无新增硬件依赖**。

**与现有模块的交互**：
- 复用 `lvgl_port` 的 `Lvgl_Lock` / `Lvgl_Unlock`
- 复用 `font_cn` 的中文字体
- 复用 `port_rtc` 的当前时间（满屏提醒页显示 14:57）
- 不影响 `todo_manager` / `todo_ui` / `wifi_*` / `port_shtc3` / `port_battery` / `port_touch` 等

---

## 8. 测试策略

### 8.1 单元测试

**跳过**。timer_engine 无 LVGL 依赖、可独立测试，但项目当前无主机测试基础设施，搭建工作量超过收益。等核心稳定后视复杂度再补。

### 8.2 集成验证（真机）

```
□ 启动番茄钟 25 min → 主屏右下出现 🍅 + 24 min + 进度条
□ 切回主屏 → 右下倒计时每分钟更新
□ 进番茄钟页 → 显示运行中状态 + 取消按钮
□ 取消番茄钟 → 回到选时长状态，右下变回 Image 占位
□ 启动番茄钟 → 返回主屏 → 右下倒计时持续走，不中断
□ 设 3 个提醒（5/10/15）→ 提醒页列表显示 3 项
□ 第 4 个提醒预设按钮全部置灰
□ 取消中间一个提醒 → 列表更新为 2/3
□ 番茄钟到点 → 满屏弹页（"番茄钟 25 分钟已完成" + 当前时间）
□ 点屏幕任意处 → 解除，回到番茄钟页未运行状态
□ 设两个提醒接近同时到点 → 第一个弹 → 解除 → 立即弹第二个（FIFO）
□ 满屏提醒时番茄钟仍在后台跑（如果同时设过）
□ 主屏左上点击 → 时间工具页 → 番茄钟/提醒入口
□ 各页面右滑手势返回（与 todo_ui 一致）
□ 主屏时钟/温湿度/TODO/WiFi 状态不受影响
```

### 8.3 内存与稳定性

- 启动番茄钟 + 3 提醒 + 满屏 alert 反复弹出 10 次 → `heap_caps_get_free_size` 不泄漏
- 番茄钟跑完整 25 分钟 → 内存稳定
- 满屏 alert 页面按需创建并复用，不每次新建销毁

### 8.4 不做的验证

- 不做 CI 自动化测试（项目无 CI 基础设施）
- 不做功耗测试（与现有相同，纯常亮）
- 不做 1000 次番茄钟压力测试（使用强度远不到）

---

## 9. 实现顺序建议

写计划时建议如下分阶段：

1. **timer_engine** 骨架 + 番茄钟单实例 + 软件定时器 tick + fired_cb
2. **timer_engine** 扩展支持多提醒槽位（3 个）+ 全列表查询
3. **timer_ui** 时间工具子页 + 番茄钟页（未运行 / 运行中两态）
4. **timer_ui** 满屏到点提醒 + fired_queue + FIFO 排队
5. **timer_ui** 定时提醒页 + 已挂列表 + 取消气泡
6. **ui/** 主屏左上点击事件 + 右下迷你显示动态化
7. **main.cpp** 接线：Init + 注册 fired_cb + data_task 每秒 TickUpdate
8. 集成测试清单按节 8.2 走一遍

每一步都能在真机上独立验证一部分。
