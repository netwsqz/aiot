# Plan: 桌宠人性化增强 v1

**Source**: 自由需求「让桌宠模式更加复杂更加人性化」
**Complexity**: Medium

## Summary
在现有 6 状态桌宠基础上，新增 4 个状态（GREETING/CELEBRATE/REST/POKE）+ 状态对白气泡（8s 自动隐藏）+ 长按"撸"交互 + 双击跳 TODO。本期不新增位图，新状态复用现有 HAPPY/SLEEP 图，靠对白区分。SLEEP 周期性 Zzz 气泡模拟睡眠，避免局刷鬼影。

## 已确认决策
- 交互：单击=回 dashboard（不变）；长按=撸（POKE+对白，3s 后恢复）。跳 TODO 入口砍掉。
- 气泡：短暂显示 8s 后 `lv_timer` 自动隐藏，不缩 canvas（保持 192×192）。
- 位图：GREETING/CELEBRATE/REST/POKE 复用现有 HAPPY/SLEEP 位图，用户后续新画再替换。
- 多帧伪动画：降级为 SLEEP 状态下周期性 Zzz 气泡，零新位图、零额外局刷负担。
- 跳 TODO 功能砍除：不再保留任何跳 TODO 列表的入口（原 `screen_longpress_cb` 的 `ui_switch_page(PET_PAGE_TODO_LISTS)` 删除，`PET_PAGE_TODO_LISTS` 宏与 `ui_switch_page` 前向声明可一并移除）。

## Patterns to Mirror
| Category | Source | Pattern |
|---|---|---|
| 命名 | `pet_ui.c:1305` | `PetUi_*` 前缀，`s_` 静态变量 |
| 错误处理 | `pet_ui.c:1320` | `heap_caps_malloc` + NULL 检查返回 `ESP_ERR_NO_MEM` |
| LVGL 锁 | `CLAUDE.md` | 所有 `lv_*` 在锁内；`main.cpp` 已保证 `PetUi_Refresh` 在锁内 |
| 中文字体 | `CLAUDE.md` | `lv_font_chinese_14`，line_height=16px |
| 回调路由 | `pet_ui.c:1245` | `s_back_cb` 静态函数指针模式 |
| 双击/手势 | LVGL v9 | `LV_EVENT_DOUBLE_CLICKED` / `LV_EVENT_LONG_PRESSED` 事件 |

## Files to Change
| Files to Change | | |
| `components/pet_ui/pet_ui.h` | UPDATE | 扩展 `pet_state_t`（+4） |
| `components/pet_ui/pet_ui.c` | UPDATE | 对白表、气泡 label、8s 隐藏 timer、SLEEP Zzz timer、长按撸手势、compute_state 扩展、移除跳 TODO 依赖 |
| `components/pet_ui/CMakeLists.txt` | UPDATE | `PRIV_REQUIRES` 可移除对 `ui` 的依赖（pet_ui 不再调 `ui_switch_page`）——实际当前未依赖 ui，只需确认 |
| `main/main.cpp` | 无需改 | `data_task` 仍调 `PetUi_Refresh`；手势在 pet_ui 内部处理 |

## Tasks

### Task 1: 扩展状态枚举与对白表
- **Action**: `pet_state_t` 加 `PET_STATE_GREETING / CELEBRATE / REST / POKE`。每个状态配 3-4 句中文对白，`static const char *s_lines[PET_STATE_COUNT][4]`。`compute_state` 选定状态后用 `esp_random()` 在该状态对白里随机取一句。每句 ≤14 字（200px/14px）。
- **Mirror**: `static const` 二维数组，索引=状态枚举值。
- **Validate**: `idf.py build`

### Task 2: 对白气泡 UI（8s 自动隐藏）
- **Action**: `PetUi_Init` 时创建 `s_bubble = lv_label_create(s_pet_screen)`，白底黑字 14px，`LV_LABEL_LONG_DOT` 截断，`lv_obj_set_width(192)`，`lv_obj_align` 到 canvas 下方（y=172，高 24）。初始隐藏。`apply_state` 时按状态决定：SLEEP/HAPPY/FOCUS/LOW_BATT 不显示气泡；GREETING/CELEBRATE/REST/POKE/ANXIOUS/BUSY 显示对白。显示时 `lv_label_set_text` + `lv_obj_clear_flag(HIDDEN)` + 启动 8s `lv_timer`（one-shot）到点 `lv_obj_add_flag(HIDDEN)`。重复触发先删旧 timer。
- **Mirror**: 复用 `lv_obj_create` + style 模式（参考 `pet_ui.c:1309` screen 创建）。
- **Validate**: `idf.py build`；设备实测气泡显隐。
- **Risk**: 气泡 y=172 与 canvas 底部（y=4+192=196）重叠 24px。需把 canvas 上移或气泡盖在 canvas 下沿。**方案**：canvas 保持原位，气泡盖在 canvas 最底部 24px（角色脚部），对白优先于脚部显示。可接受——脚部细节少。

### Task 3: SLEEP 翻转 → Zzz 气泡（降级方案）
- **Action**: SLEEP 状态下，用 `lv_timer_create` 每 90s 触发一次"Zzz"气泡（复用 Task 2 气泡机制，文本="Zzz..."），8s 后隐藏。仅在 `s_cur_state==SLEEP` 时生效；切出 SLEEP 时停 timer。不碰 canvas 位图，零鬼影。
- **Validate**: `idf.py build`；夜间实测 Zzz 周期出现。
- **Risk**: timer 在非 SLEEP 状态需正确停止，否则泄露。`apply_state` 切换时统一管理 timer 生命周期。

### Task 4: 触摸手势路由
- **Action**: `screen_clicked_cb` → 单击直接 `s_back_cb()`（不变）。`screen_longpress_cb` → 改为 `PetUi_Poke()`：切 POKE 状态 + 对白，3s 后恢复 `compute_state` 的状态。**删除跳 TODO 逻辑**：移除 `ui_switch_page(PET_PAGE_TODO_LISTS)` 调用、`PET_PAGE_TODO_LISTS` 宏、`ui_switch_page` 前向声明（pet_ui 不再依赖 ui 模块）。POKE 期间屏蔽 Refresh 覆盖（`s_poke_until_tick` > now 时 Refresh 跳过）。
- **Mirror**: 复用 `lv_obj_add_event_cb`（`pet_ui.c:1331`）。
- **Validate**: `idf.py build`；设备实测两手势（单击回 dashboard、长按撸）。
- **Risk**: 无双击竞争——交互更干净。

### Task 5: compute_state 扩展
- **Action**: 优先级调整为 SLEEP > LOW_BATT > GREETING > CELEBRATE > REST > FOCUS > ANXIOUS > BUSY > HAPPY。
  - GREETING：RTC 6-9 早 / 19-22 晚，每唤醒周期只触发一次（`s_greet_done_hour` 记录已问候的小时，跨小时重置）。
  - CELEBRATE：`s_prev_undone > 0 && undone == 0`，触发后 `s_prev_undone = 0` 防重复。
  - REST：`s_last_active_tick`（每次 Refresh 更新）距今 > 50min。poke/触摸重置该 tick。
- **Validate**: `idf.py build`；设备模拟各条件。
- **Risk**: GREETING 的 `s_greet_done_hour` 在深睡眠唤醒后重置为 -1（app_main 重跑），即每次唤醒都会问候一次——符合预期。

## Validation
```bash
env -u MSYSTEM powershell.exe -ExecutionPolicy Bypass -Command \
  "[Environment]::SetEnvironmentVariable('MSYSTEM', \$null, 'Process'); \
   . E:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1; \
   cd E:\aiot; idf.py build"
```
+ `idf.py -p COM5 flash` 设备实测。

## Risks
| Risk | Likelihood | Mitigation |
|---|---|---|
| 气泡盖住角色脚部 | 中 | 可接受，脚部细节少；后续可缩 canvas |
| 双击与单击竞争误触 | — | 已砍掉双击，无此问题 |
| Zzz timer 泄露 | 低 | apply_state 统一管理 timer 生命周期 |
| 对白超长 | 低 | ≤14 字 + LONG_DOT 截断 |
| 新状态无新位图，辨识度靠对白 | 中 | 本期接受，用户后续补图 |

## Acceptance
- [ ] `idf.py build` 通过
- [ ] 设备实测：GREETING/CELEBRATE/REST/POKE 四状态触发 + 对白
- [ ] 气泡 8s 自动隐藏
- [ ] SLEEP 夜间 Zzz 周期出现
- [ ] 单击回 dashboard / 长按撸 两手势正常
- [ ] 无内存泄漏（timer 正确回收）
