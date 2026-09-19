# PC LVGL 仿真工具（`tools/pc_sim`）

在 Windows 主机上用 SDL2 跑项目 UI 源码，免实机烧录，方便快速验证 UI 改动。

不参与固件编译 —— 仅用于主机侧 UI 回归。跑实机时 UI 由真正的 ESP-IDF 组件提供。

## 依赖

| 依赖 | 版本 | 说明 |
|------|------|------|
| MinGW-w64 gcc | 15.x | `D:\mingw64`（或 PATH 里的 gcc） |
| CMake | ≥3.16 | 配置 + 生成 |
| Ninja | 任意 | 构建后端（`-G Ninja`） |
| SDL2 devel | 2.30.12 | MinGW x86_64 包，解压到本机任意路径 |
| LVGL | 9.5.0 | 主仓 `managed_components/lvgl__lvgl`（由 `idf.py build` 生成） |

LVGL 源码不在 git 里（gitignored），必须先在主仓根跑一次 `idf.py build` 让它被 fetch 下来，本工程才能编。

SDL2 也不在 git 里 —— 从 [libsdl SDL2 release](https://github.com/libsdl-org/SDL/releases) 下 `SDL2-devel-2.30.12-mingw.zip`，解压后把 `x86_64-w64-mingw32` 子目录路径作为 `-DSDL2_ROOT` 传入。

## 构建

从主仓根运行（路径要写 `tools/pc_sim`，不要进子目录跑）：

```bash
cmake -S tools/pc_sim -B tools/pc_sim/build -G Ninja \
      -DSDL2_ROOT=<你的 SDL2>/x86_64-w64-mingw32
cmake --build tools/pc_sim/build
```

产物：`tools/pc_sim/build/aiot_sim.exe`（SDL2.dll 已 post-copy 到同目录）。

> 必须 Git Bash 里调 gcc/CMake（ESP-IDF 的 `idf.py` 才需要 PowerShell，本工程不碰 ESP-IDF）。

## 运行

```bash
tools/pc_sim/build/aiot_sim.exe
```

开一个 200×200 窗口（放大 3 倍），默认进 pet 页。

| 键 | 动作 |
|----|------|
| 1 | dashboard |
| 2 | pet |
| 3 | todo |
| 4 | timer |
| 5 | weather |
| 6 | wifi mgmt |
| 7 | draw（画板） |
| b / B | 电池 -10 / +10 |
| h | 时钟小时 +1 |
| p | 番茄钟开/关 |
| ESC | 退出 |

画板页可鼠标拖拽画线（模拟触摸）。BOOT 键短按在真机上是 save&exit，PC 上用 ESC 退。

## 文件结构

```
tools/pc_sim/
├── CMakeLists.txt     # 构建配置；pc_resolve() 把项目源码相对主仓根解析
├── lv_conf.h          # LVGL 配置（16bpp, Montserrat 14/20/28, SDL driver）
├── include/           # ESP-IDF API 的 PC shim 头（esp_timer/esp_log/esp_random/...）
└── src/
    ├── main.c         # 仿真入口：SDL 窗口 + 键盘路由 + mock 数据循环
    └── mocks.c        # 业务层桩：TodoMgr/TimerEngine/PortRtc/PortBattery 等假数据
```

## 设计要点

- **不改项目源码一行**：`components/ui/ui_screen.c`、`components/pet_ui/pet_ui.c`、
  `components/draw_ui/draw_ui.c`、`components/font_cn/lv_font_chinese_14.c` 被直接编进 sim，
  靠 `mocks.c` 提供业务层假数据 + `include/` 的 esp shim 头让它们链接通过。
- **SPIFFS 降级**：`draw_ui.c` 用 `fopen("/spiffs/draw.bmp")`，PC 无 SPIFFS，
  `fopen` 失败时 `DrawUi_HasSavedImage/RenderThumbnail` 安全返回 false，
  dashboard 右下角显示 "Image" 文字占位。笔迹保存/缩略图在 sim 上降级，不崩。
- **中文方框**：`lv_conf.h` 默认字体指向 Montserrat 14，无 CJK 字形。
  `lv_font_chinese_14` 虽编进去了但未设为默认 —— 仿真看布局够用，要真中文需改 `LV_FONT_DEFAULT`。
- **无动画/无 EPD 刷新**：sim 用 SDL 实时渲染，不受墨水屏 300ms 局刷限制，
  所以这里看不出局刷鬼影/LUT 截断等问题 —— 那些必须真机验证。

## 限制

sim 覆盖不到的真机行为（必须 `idf.py flash` 实测）：

- EPD 局刷路径、SSD1681 LUT、PSRAM 显存分配
- SPIFFS `/spiffs/draw.bmp` 真实读写 + 2×2 多数票降采样缩略图
- indev 8ms 提速（`DrawUi_SetIndevFast`）对触摸曲线的实际影响
- 深睡眠唤醒后 `app_main` 重跑路径

## 主机自检：`bmp1bpp_selftest.c`

sim 跑不到 SPIFFS，所以 `bmp1bpp.c`（standby 图和画板共用的 1bpp BMP 读写）
单独有个纯主机往返检查，不依赖 SDL/LVGL/ESP-IDF：

```bash
gcc -I tools/pc_sim/include -I components/draw_ui \
    components/draw_ui/bmp1bpp.c tools/pc_sim/bmp1bpp_selftest.c -o bmpcheck.exe
./bmpcheck.exe
```

验证：写出的 BMP 能被自己读回、像素零差异、文件大小 5662、头字段（bpp/宽高/数据偏移）
符合 1bpp 200×200 约定。改 `bmp1bpp.c` 的位序或行序后必跑。

## 主机自检：`timer_pure_selftest.c`

`timer_engine` 里改动最频繁、出过最多 bug 的三段算术（倒计时归零、睡眠后 elapsed
折算、id 回绕）已抽成 `components/timer_engine/timer_pure.c` —— 无锁、无 NVS、
不碰 FreeRTOS，所以能脱离硬件跑：

```bash
gcc -I tools/pc_sim/include -I components/timer_engine \
    components/timer_engine/timer_pure.c tools/pc_sim/timer_pure_selftest.c \
    -o timercheck.exe
./timercheck.exe
```

覆盖：递减与归零顺序、`fired[]` 快照的 active 必须为 false（回调依赖）、
死槽不重复触发、`remaining==0` 不回绕、RTC 调回时 elapsed 钳 0、
睡眠正好到期（`<=` 是刻意的）、id 回绕钳 1。改 `timer_pure.c` 后必跑。
