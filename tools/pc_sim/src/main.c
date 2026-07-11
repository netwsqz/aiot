/*
 * main.c — PC LVGL 测试入口。
 *
 * 用 LVGL 自带的 SDL window driver 开一个 200×200 窗口（放大 3 倍便于看），
 * 调项目的 ui_init() / PetUi_Init()，模拟 data_task 喂数据、轮询页面切换。
 *
 * 键盘：
 *   1  dashboard      2  pet      3  todo
 *   4  timer          5  weather  6  wifi mgmt   7  draw   8  snake
 *   b  电池-10        B  电池+10   h  时钟小时循环
 *   p  切番茄钟开/关
 *   ESC 退出
 */
#include <stdio.h>
#include <stdbool.h>

#include "SDL.h"

#include "lvgl.h"
#include "src/drivers/sdl/lv_sdl_window.h"
#include "src/drivers/sdl/lv_sdl_mouse.h"
#include "src/drivers/sdl/lv_sdl_keyboard.h"

#include "ui.h"
#include "pet_ui.h"
#include "snake_ui.h"

/* mock 的可调接口（mocks.c） */
extern void mock_rtc_set_hour(uint8_t h);
extern void mock_battery_set(uint8_t pct);
extern void mock_pomodoro_set(bool running, uint32_t remaining_sec, uint32_t total_sec);
extern void pc_advance_tick_us(int64_t delta_us);

static int s_batt = 80;
static int s_hour = 14;
static bool s_pomo = false;

static void dump_help(void)
{
    printf("\n=== AIoT PC sim ===\n");
    printf(" 1 dashboard  2 pet  3 todo  4 timer  5 weather  6 wifi  7 draw  8 snake\n");
    printf(" b/-10%%  Shift+b/+10%%  h/hour+1  p/pomodoro toggle  ESC quit\n");
    printf("==================\n");
}

int main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    lv_init();
    lv_tick_set_cb(SDL_GetTicks);
    /* LVGL v9 desktop 构建不会自动装 tick cb；这里显式设上，否则 tick 停在 0，
     * timer/animator 不前进。 */

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "[sim] SDL_Init FAIL: %s\n", SDL_GetError());
        return 1;
    }

    /* 200×200 对应固件 EPD 尺寸；放大 3 倍窗口便于看。 */
    lv_display_t *disp = lv_sdl_window_create(200, 200);
    lv_sdl_window_set_zoom(disp, 3.0f);
    lv_sdl_window_set_title(disp, "AIoT sim");

    lv_sdl_mouse_create();
    lv_sdl_keyboard_create();

    /* 初始化业务 mock（RTC/电池/TODO/Timer 有默认假数据），建 UI。
     * 顺序与固件 app_main 一致：ui → pet → 默认页 pet。 */
    ui_init();
    PetUi_Init();
    PetUi_SetBackCallback(NULL);
    SnakeUi_Init();
    SnakeUi_SetBackCallback(NULL);
    ui_switch_page(PAGE_PET);

    dump_help();

    bool running = true;
    Uint32 last = SDL_GetTicks();
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) { running = false; }
            else if (ev.type == SDL_KEYDOWN) {
                SDL_Keycode k = ev.key.keysym.sym;
                switch (k) {
                    case SDLK_1: ui_switch_page(PAGE_DASHBOARD); break;
                    case SDLK_2: ui_switch_page(PAGE_PET); break;
                    case SDLK_3: ui_switch_page(PAGE_TODO_LISTS); break;
                    case SDLK_4: ui_switch_page(PAGE_TIMER_TOOLS); break;
                    case SDLK_5: ui_switch_page(PAGE_WEATHER); break;
                    case SDLK_6: ui_switch_page(PAGE_WIFI_MGMT); break;
                    case SDLK_7: ui_switch_page(PAGE_DRAW); break;
                    case SDLK_8: ui_switch_page(PAGE_SNAKE); break;
                    case SDLK_b:
                        if (ev.key.keysym.mod & KMOD_SHIFT) { s_batt += 10; if (s_batt>100) s_batt=100; }
                        else { s_batt -= 10; if (s_batt<0) s_batt=0; }
                        mock_battery_set((uint8_t)s_batt);
                        ui_update_battery((uint8_t)s_batt);
                        printf("battery=%d\n", s_batt); break;
                    case SDLK_h: s_hour = (s_hour + 1) % 24;
                                 mock_rtc_set_hour((uint8_t)s_hour);
                                 printf("hour=%d\n", s_hour); break;
                    case SDLK_p: s_pomo = !s_pomo;
                                 mock_pomodoro_set(s_pomo, 18*60, 25*60);
                                 ui_update_timer_mini(s_pomo, 18*60, 25*60);
                                 printf("pomodoro=%d\n", s_pomo); break;
                    case SDLK_ESCAPE: running = false; break;
                    default: break;
                }
            }
        }

        Uint32 now = SDL_GetTicks();
        int32_t delta = (int32_t)(now - last);
        last = now;
        pc_advance_tick_us((int64_t)delta * 1000);
        lv_timer_handler();
        SDL_Delay(5);
    }

    lv_sdl_quit();
    return 0;
}
