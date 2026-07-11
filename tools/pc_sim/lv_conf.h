#ifndef LV_CONF_H
#define LV_CONF_H

/* ── PC 测试用 LVGL 配置 ──────────────────────────────
 * 对齐固件 sdkconfig 的关键项（16bpp、Montserrat 14/20/28、DPI 130），
 * 但在 PC 上额外开启 SDL driver 以便开窗渲染。
 * 中文 lv_font_chinese_14 不编进 PC 工程（用 Montserrat 14 顶替，
 * 中文会显示为方框 —— 骨架阶段够用，后续要真中文再加 font_cn 源码）。
 * 文件级注意事项见 test_pc/README.md。
 * ------------------------------------------------- */

#include <stdint.h>

/* ==================== Graphical ==================== */
#define LV_COLOR_DEPTH          16
#define LV_COLOR_CHROMA_KEY     lv_color_hex(0xff00ff)
#define LV_DPI_DEF              130
#define LV_DRAW_BUF_ALIGN       4

/* ==================== Memory ==================== */
#define LV_MEM_CUSTOM           0
#define LV_MEM_SIZE             (128U * 1024U)
#define LV_MEM_ADR              0

/* ==================== OS ==================== */
#define LV_USE_OS               LV_OS_NONE

/* ==================== Tick ==================== */
#define LV_TICK_CUSTOM          1
#define LV_TICK_CUSTOM_INCLUDE  <SDL.h>
#define LV_TICK_CUSTOM_SYSZONE_EXPR  (SDL_GetTicks())

/* ==================== Drawing ==================== */
#define LV_USE_DRAW_SW          1
#define LV_USE_DRAW_SDL         0       /* 用 SDL window driver，不走 draw_sdl */
#define LV_DRAW_SW_DRAW_UNIT_CNT 1

/* ==================== Log ==================== */
#define LV_USE_LOG              0

/* ==================== Fonts ==================== */
#define LV_FONT_MONTSERRAT_8    0
#define LV_FONT_MONTSERRAT_10   0
#define LV_FONT_MONTSERRAT_12   0
#define LV_FONT_MONTSERRAT_14   1
#define LV_FONT_MONTSERRAT_16   0
#define LV_FONT_MONTSERRAT_18   0
#define LV_FONT_MONTSERRAT_20   1
#define LV_FONT_MONTSERRAT_22   0
#define LV_FONT_MONTSERRAT_24   0
#define LV_FONT_MONTSERRAT_26   0
#define LV_FONT_MONTSERRAT_28   1
#define LV_FONT_DEFAULT         &lv_font_montserrat_14
#define LV_USE_FONT_PLACEHOLDER 1

/* ==================== Widget / Layout ==================== */
#define LV_USE_OBJX_NAMES       1
#define LV_USE_SYSMON           0

/* ==================== SDL driver (PC 渲染后端) ==================== */
#define LV_USE_SDL              1
#define LV_SDL_RENDER_MODE      LV_DISPLAY_RENDER_MODE_PARTIAL
#define LV_SDL_BUF_SIZE_BYTE    (32 * 1024)
#define LV_SDL_INCLUDE          "SDL2/SDL.h"

/* ==================== Windows ==================== */
#define LV_USE_WINDOWS          0

#endif /* LV_CONF_H */
