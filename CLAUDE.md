# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Flash

ESP-IDF v5.5.4, installed at `E:\esp\.espressif\v5.5.4\esp-idf`.

**Critical**: Claude Code's Bash tool runs in Git Bash (MSys). ESP-IDF v5.5.4 rejects MSys. You MUST strip the `MSYSTEM` env var and call PowerShell:

```bash
# Build (from any directory)
env -u MSYSTEM powershell.exe -ExecutionPolicy Bypass -Command \
  "[Environment]::SetEnvironmentVariable('MSYSTEM', \$null, 'Process'); \
   . E:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1; \
   cd E:\aiot; idf.py build"

# Flash
env -u MSYSTEM powershell.exe -ExecutionPolicy Bypass -Command \
  "[Environment]::SetEnvironmentVariable('MSYSTEM', \$null, 'Process'); \
   . E:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1; \
   cd E:\aiot; idf.py -p COM5 flash"

# Clean build
env -u MSYSTEM powershell.exe -ExecutionPolicy Bypass -Command \
  "[Environment]::SetEnvironmentVariable('MSYSTEM', \$null, 'Process'); \
   . E:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1; \
   cd E:\aiot; idf.py fullclean"
```

User's native shell: PowerShell with `E:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1` loaded.

### PC LVGL sim (host UI regression, no flash)

`tools/pc_sim/` compiles the firmware UI `.c` files verbatim against host SDL2 + LVGL (from `managed_components/`), so you can iterate on UI layout / page-switch / callbacks without hardware. **Not** ESP-IDF — uses host gcc (MinGW) + CMake + Ninja, so do NOT wrap it in the PowerShell/MSYSTEM dance above.

```bash
# one-time: ensure managed_components/ exists (run idf.py build once in repo root)
cmake -S tools/pc_sim -B tools/pc_sim/build -G Ninja \
      -DSDL2_ROOT=<SDL2-2.30.12>/x86_64-w64-mingw32
cmake --build tools/pc_sim/build
tools/pc_sim/build/aiot_sim.exe   # keys 1-7 switch pages, b/h/p mock data, ESC quit
```

See `tools/pc_sim/README.md` for dependencies and the full keyboard map. Sim covers UI logic only — EPD partial-refresh, SPIFFS read/write, indev timing, deep-sleep wake still need `idf.py flash`.

### Verification (no host test framework)

Hardware-dependent code (I2C/SPI/e-ink/audio) can't be mocked on host, so there is no unit-test / coverage toolchain. Verification = `idf.py build` (compile) + `idf.py flash` + on-device observation. `idf.py size` watches PSRAM/flash/stack budget (DIRAM is ~85% full — recheck before adding large buffers). The PC sim is the only host-side check, and only for UI.

## Architecture

Three-layer component architecture, all wired in `main/main.cpp`:

```
┌─────────────────────────────────────────────────┐
│ main.cpp  (app_main — init sequence, tasks)     │
├─────────────────────────────────────────────────┤
│ Application:  todo_ui  timer_ui  todo_web       │
│               todo_manager  timer_engine        │
│               wifi_bsp  wifi_ap  standby_screen │
│               pet_ui  draw_ui  weather_*        │
├─────────────────────────────────────────────────┤
│ LVGL/UI:      lvgl_port  ui  font_cn           │
├─────────────────────────────────────────────────┤
│ BSP/HAL:      epaper_driver_bsp  i2c_bsp        │
│               port_shtc3  port_rtc  port_touch  │
│               port_battery                       │
└─────────────────────────────────────────────────┘
```

- **BSP layer** (`*_bsp`, `port_*`): Hardware abstraction. I2C bus is shared on IO47/IO48 (addresses 0x70/0x51/0x38 don't conflict).
- **LVGL bridge** (`lvgl_port`): Connects C++ EPD driver to LVGL flush callback. All UI updates must go through `Lvgl_Lock()`/`Lvgl_Unlock()`.
- **Application modules** (`todo_*`, `timer_*`, `wifi_*`, `pet_ui`, `draw_ui`, `weather_*`): Self-contained C components with `extern "C"` headers. Each owns its own LVGL screen.
- **Only `main.cpp` is C++** — everything else is C or C-with-extern-C-wrapper.

### Key data flows

- **TODO**: Phone → WiFi AP → HTTP REST (`todo_web`) → NVS (`todo_manager`) → LVGL screen (`todo_ui`)
- **Timer**: Button/touch → `timer_engine` (FreeRTOS timers) → NVS persist → `timer_ui` (fullscreen alert)
- **Pet**: `data_task` aggregates device state → `pet_ui` `compute_state()` picks one of 10 states (SLEEP>LOW_BATT>GREETING>CELEBRATE>REST>FOCUS>ANXIOUS>BUSY>HAPPY + POKE on long-press). Speech bubble auto-hides after 8s. New states (GREETING/CELEBRATE/REST/POKE) reuse HAPPY/SLEEP bitmaps via `s_bitmap_for[]` — replace entries when art is drawn.
- **Draw**: dashboard bottom-right quadrant → `PAGE_DRAW` → `draw_ui` canvas. BOOT short-press on draw page = `DrawUi_SaveAndExit()` (NOT long-press — LVGL LONG_PRESSED conflicts with drawing). Indev sampling 33ms→8ms while on draw page.
- **Sensors**: FreeRTOS `data_task` polls SHTC3/RTC/Battery → updates `ui` dashboard
- **Deep sleep**: Double-PWR → tear-down sequence (`app_main` steps 1-12, ~`main.cpp:347`) → `esp_deep_sleep_start()` → wake re-runs `app_main()` (different init path, skips WiFi)

### LVGL on e-ink constraints

- **LVGL v9** (managed component, Kconfig — no `lv_conf.h` on firmware; PC sim has its own `tools/pc_sim/lv_conf.h`)
- **No animations, no shadows** — e-ink refresh is ~300ms, no partial animation possible
- **Use `lv_font_chinese_14`** for any Chinese text (Source Han Sans SC, line_height=16px). Montserrat fonts lack CJK glyphs. The pet speech bubble and any CN labels must set this font explicitly.
- **LVGL v8 vs v9 API**: This project uses LVGL v9. If referencing Waveshare V2 examples, they use LVGL v8 — `lv_obj_set_flex_gap()` is v9-only, `lv_obj_set_style_pad_column()` works in both.
- **Partial refresh**: Must call `EPD_DisplayPartBaseImage()` before any partial update. SSD1681 LUT arrays must be complete (159 values for `WF_PARTIAL_1IN54_0`) or BUSY pin hangs.
- **Checkbox label wrapping**: Must set `LV_LABEL_LONG_WRAP` on the internal label child (index 1) and limit width on both the checkbox and its label.
- **ARGB8888 canvas for 1bpp patterns**: `pet_ui`/`draw_ui` use `LV_COLOR_FORMAT_ARGB8888` (4 bytes/pixel) and write `0xFFFFFFFF`/`0xFF000000` directly, to avoid LVGL v9 I1 indexed-color canvas stride/alignment pitfalls.

## Hardware Pinout (Quick Reference)

| GPIO | Function | Note |
|------|----------|------|
| IO0  | BOOT btn | Active low, internal pullup |
| IO4  | Battery ADC | ADC1_CH3, 2:1 voltage divider |
| IO5  | RTC alarm IRQ | Deep sleep wake source |
| IO6  | EPD power | **Active low** |
| IO8  | EPD BUSY | Input |
| IO9  | EPD RST | |
| IO11 | EPD SPI CS | Manual GPIO toggle |
| IO12/13 | EPD SPI CLK/MOSI | |
| IO17 | VBAT power | Active high |
| IO18 | PWR btn / wake | Active low, deep sleep wake |
| IO21 | Touch IRQ | Falling edge |
| IO42 | Audio PA EN | **Active low** |
| IO47/48 | I2C SDA/SCL | Shared: SHTC3(0x70), RTC(0x51), Touch(0x38) |

Full pinout and peripheral details in GPIO table below.

## Common Patterns

### Adding a new LVGL screen
1. Create component with `*_init()`, `*_get_screen()` returning `lv_obj_t*`
2. Screen size: 200×200, white bg, no border, no radius, no pad
3. Register in `main.cpp`: init → add to `ui_switch_page()` dispatch
4. All `lv_*` calls must be inside `Lvgl_Lock()`/`Lvgl_Unlock()`

### NVS persistence pattern
- Key namespace + single blob per record
- `TodoMgr_*` and `TimerEngine_Persist/Restore` are the established patterns
- Deep sleep wake path restores state from NVS without network

### Static ID pools for LVGL callbacks
LVGL event `user_data` must point to memory that outlives the function call. Stack arrays die on return. Use static pools like `s_list_id_pool[TODO_MAX_LISTS][TODO_ID_LEN]` — see `todo_ui.cpp` for the pattern.

### Adding a component / wiring it in
1. `components/<name>/CMakeLists.txt` with `idf_component_register(SRCS ... INCLUDE_DIRS "." PRIV_REQUIRES ...)`.
2. Add the component name to `main/CMakeLists.txt` `PRIV_REQUIRES`.
3. If it owns an LVGL screen: `#include` in `main.cpp`, init in `app_main` (BOTH the normal path AND the deep-sleep wake path — wake re-runs `app_main`), register in `ui_switch_page()` dispatch (`components/ui/ui_screen.c`), add a `PAGE_<X>` enum to `ui.h`.
4. ESP-IDF auto-discovers components under `components/` — but `PRIV_REQUIRES` is what makes `#include` resolve. Forgetting it = link error.

### Orphan: `components/ai_engine/`
`ai_engine.c` (1261 lines) is tracked but **not wired** — no `CMakeLists.txt`, not in any `PRIV_REQUIRES`, not called from `main.cpp`. It was stripped from master (stash `wip: strip ai engine`) after 15 rounds of concurrency fixes on the `feature/weather-forecast` branch. To re-enable: cherry-pick `ai_ui` + `audio_bsp` from `.worktrees/weather-feature`, add CMakeLists, add to `main/CMakeLists.txt` REQUIRES, and recheck `idf.py size` (it uses 8KB×2 task stacks + WS/HTTP; DIRAM is ~85% full). See `docs/NEXT-STEPS-2026-07.md` P1.

## Reference

- Waveshare V2 examples: `V2/` (13 projects, LVGL v8 unless noted) — reference only, not built
- Design specs: `docs/ecc/specs/` · Implementation plans: `docs/ecc/plans/` and `.claude/plans/`
- Web UI (standalone dev): `web_dev/index.html`
- Roadmap / known gaps: `docs/NEXT-STEPS-2026-07.md`
- Project rules (ESP-IDF specialization of common/cpp standards): `.claude/rules/ecc/`

## Agent skills

### Issue tracker

Issues are tracked in GitHub Issues on `netwsqz/aiot` (via the `gh` CLI). See `docs/agents/issue-tracker.md`.

### Triage labels

The five canonical triage roles map to the default label strings (`needs-triage`, `needs-info`, `ready-for-agent`, `ready-for-human`, `wontfix`). See `docs/agents/triage-labels.md`.

### Domain docs

Single-context: one `CONTEXT.md` at the repo root plus `docs/adr/` for decisions. See `docs/agents/domain.md`.

---

## GPIO Full Table

| GPIO | Function | Note |
|------|----------|------|
| IO0  | BOOT 按键 | 低有效，带内部上拉 |
| IO3  | LED | |
| IO4  | 电池 ADC | ADC1_CH3, 分压比 2:1 |
| IO5  | RTC 闹钟中断 | 深睡眠唤醒源 |
| IO6  | EPD 电源使能 (EPD3V3_EN) | **低有效** |
| IO7  | 触摸屏复位 (EPD_TP_RST) | |
| IO8  | 墨水屏忙信号 (EPD_BUSY) | 输入 |
| IO9  | 墨水屏复位 (EPD_RST) | |
| IO10 | 墨水屏 D/C | |
| IO11 | 墨水屏 SPI CS | 软件手动控制 |
| IO12 | 墨水屏 SPI SCLK | |
| IO13 | 墨水屏 SPI SDI (MOSI) | |
| IO14 | I2S MCLK | |
| IO15 | I2S SCLK | |
| IO16 | I2S ASDOUT | |
| IO17 | 电池电源控制 (VBAT_PWR) | **高有效** |
| IO18 | PWR 按键 / PA EN | 低有效，深睡眠唤醒源 |
| IO21 | 触摸屏中断 (EPD_TP_INT) | 下降沿 |
| IO38 | I2S LRCK | |
| IO39 | SD 卡 CLK | SDMMC 1-bit |
| IO40 | SD 卡 MISO (D0) | |
| IO41 | SD 卡 MOSI (CMD) | |
| IO42 | 音频功放电源 (PA EN) | **低有效** |
| IO43 | UART TX | |
| IO44 | UART RX | |
| IO45 | I2S DSDIN | |
| IO46 | PA CTRL | |
| IO47 | I2C SDA | 触摸屏/RTC/SHTC3 共用 |
| IO48 | I2C SCL | 触摸屏/RTC/SHTC3 共用 |

## 硬件要点

- **PSRAM 必须启用** — 墨水屏显存和 LVGL 缓冲区分配在 PSRAM
- **I2C 总线共用** — SHTC3/RTC/触摸屏共用 IO47/IO48，地址 0x70/0x51/0x38 不冲突
- **墨水屏电源低有效** — EPD_PWR_PIN=LOW 开机，HIGH 关机
- **电源顺序** — EPD(IO6)、Audio(IO42)、VBAT(IO17) 需同时开启，否则 EPD 可能不工作
- **局刷前必须全刷基图** — `EPD_DisplayPartBaseImage()` 写入 BW+RED 双 RAM
- **SSD1681 LUT 数据必须完整** — `WF_PARTIAL_1IN54_0` 必须 159 个值，少一个都会导致控制器卡死 BUSY 引脚
- **SHTC3 无寄存器地址** — 用 write_read 方式通信
- **深睡眠唤醒后重新执行 app_main()** — 不是从睡眠点恢复
- **read_busy() 需要安全超时** — V2 原版无限等待，已加 5 秒超时防止死锁
