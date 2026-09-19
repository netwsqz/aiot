/*
 * snake_ui.c — 贪吃蛇游戏屏幕（200×200，10×10 格，20×20 网格）。
 *
 * 渲染范式仿 draw_ui：1bpp buffer（EPD 格式 bit=1白/0黑）+ ARGB8888 canvas。
 * 局部刷新：每步只 invalidate 蛇头新格 + 旧尾格（+ 食物格），避开 e-ink
 * 300ms/次全屏闪。节奏：lv_timer 500ms/步。操控：四象限点击转向，禁 180°。
 *
 * 状态机：
 *   READY   — 进页面即此态，画边框 + "点屏幕开始"。首次点屏 → RUNNING
 *   RUNNING — lv_timer 步进，蛇移动
 *   OVER    — 撞墙/自撞，显示分数。点屏 → READY（再点开始）
 */
#include "snake_ui.h"

#include <string.h>
#include <stdio.h>
#include "lvgl.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "font_cn.h"

static const char *TAG = "snake_ui";

/* ── 尺寸常量 ── */
#define SNAKE_W          200
#define SNAKE_H          200
#define SNAKE_ROW_BYTES  ((SNAKE_W + 7) / 8)   /* 25 字节/行 */
#define SNAKE_BUF_SIZE   (SNAKE_W * SNAKE_H / 8) /* 5000 字节 */

#define GRID_W           20
#define GRID_H           20
#define CELL_PX          10                     /* 200/20 */
/* 游戏区：行 0 留给分数行（顶部 16px），网格从 y=16 开始到 y=196，
 * 18 行 × 20 列。为简化用整 20×20 网格从 y=0，分数叠加在顶部留白——
 * 改为游戏区 y=10..190（18 行高）更稳，但 20 行更规整。
 * 取整 20×20 全屏网格，分数 label 浮在左上角（蛇不会马上到那）。 */
#define TOP_MARGIN       0

/* 步进周期：500ms。e-ink 局刷 ~300ms，留 200ms 余量避免堆积。 */
#define STEP_PERIOD_MS   500
/* 每 N 步做一次全屏刷新，清 SSD1681 局刷累积脏（鬼影）。 */
#define FULL_REFRESH_EVERY  20

/* ── 方向 ── */
typedef enum {
    DIR_NONE = -1,
    DIR_UP = 0,
    DIR_DOWN,
    DIR_LEFT,
    DIR_RIGHT
} dir_t;

/* ── 游戏状态 ── */
typedef enum {
    SNAKE_STATE_READY = 0,
    SNAKE_STATE_RUNNING,
    SNAKE_STATE_OVER
} snake_state_t;

/* ── 静态状态 ── */
static lv_obj_t      *s_screen      = NULL;
static lv_obj_t      *s_canvas      = NULL;
static lv_color32_t  *s_canvas_buf  = NULL;  /* ARGB8888，200*200*4=160KB PSRAM */
static uint8_t       *s_buf         = NULL;  /* 1bpp，EPD 格式 1=白 0=黑 */
static lv_obj_t      *s_score_lbl   = NULL;  /* 顶部分数（montserrat_14）*/
static lv_obj_t      *s_hint_lbl    = NULL;  /* 中央提示（chinese_14）*/

static lv_timer_t    *s_step_timer  = NULL;
static void         (*s_back_cb)(void) = NULL;

/* 蛇身坐标（头在 index 0）。GRID_W*GRID_H=400 格上限。 */
static int8_t s_body_x[GRID_W * GRID_H];
static int8_t s_body_y[GRID_W * GRID_H];
static int    s_length;            /* 含头 */
static dir_t  s_dir;               /* 当前行进方向 */
static dir_t  s_next_dir;          /* 下一步要转的方向（点击设，步进时采纳）*/
static int8_t s_food_x, s_food_y;
static int    s_score;
static snake_state_t s_state;
static int    s_step_count;        /* 用于周期全刷 */

/* ── 1bpp buffer 操作（EPD 格式：bit=1 白，bit=0 黑；byte 内 MSB=最左像素）── */
static inline void buf_set_pixel(int x, int y, bool black)
{
    if (x < 0 || x >= SNAKE_W || y < 0 || y >= SNAKE_H) return;
    uint16_t idx = (uint16_t)(y * SNAKE_ROW_BYTES + (x >> 3));
    uint8_t  bit = (uint8_t)(0x80 >> (x & 7));
    if (black) s_buf[idx] &= ~bit;
    else       s_buf[idx] |=  bit;
}

/* 画一个网格格（cell 坐标，非像素）为黑/白实心方块。 */
static inline void buf_fill_cell(int gx, int gy, bool black)
{
    int px = gx * CELL_PX;
    int py = gy * CELL_PX + TOP_MARGIN;
    for (int dy = 0; dy < CELL_PX; dy++)
        for (int dx = 0; dx < CELL_PX; dx++)
            buf_set_pixel(px + dx, py + dy, black);
}

/* 把 1bpp buffer 的指定像素矩形同步到 ARGB canvas，再 invalidate 该矩形。
 * 只刷新改动的格子区域，e-ink 只局刷该小块。w/h 为像素单位。 */
static void canvas_refresh_rect(int px, int py, int w, int h)
{
    lv_color32_t *cbuf = (lv_color32_t *)s_canvas_buf;
    for (int y = py; y < py + h && y < SNAKE_H; y++) {
        for (int x = px; x < px + w && x < SNAKE_W; x++) {
            uint8_t byte = s_buf[y * SNAKE_ROW_BYTES + (x >> 3)];
            uint8_t bit  = (uint8_t)(0x80 >> (x & 7));
            cbuf[y * SNAKE_W + x] = (byte & bit)
                ? lv_color32_make(255, 255, 255, 255)
                : lv_color32_make(0, 0, 0, 255);
        }
    }
    lv_area_t a;
    a.x1 = px;  a.y1 = py;
    a.x2 = px + w - 1;  a.y2 = py + h - 1;
    lv_obj_invalidate_area(s_canvas, &a);
}

/* 刷新一个网格格（cell 坐标）到 canvas + invalidate。 */
static inline void refresh_cell(int gx, int gy)
{
    canvas_refresh_rect(gx * CELL_PX, gy * CELL_PX + TOP_MARGIN, CELL_PX, CELL_PX);
}

/* 全屏同步 buffer→canvas 并 invalidate（周期清鬼影用）。 */
static void canvas_refresh_all(void)
{
    lv_color32_t *cbuf = (lv_color32_t *)s_canvas_buf;
    for (int y = 0; y < SNAKE_H; y++) {
        for (int x = 0; x < SNAKE_W; x++) {
            uint8_t byte = s_buf[y * SNAKE_ROW_BYTES + (x >> 3)];
            uint8_t bit  = (uint8_t)(0x80 >> (x & 7));
            cbuf[y * SNAKE_W + x] = (byte & bit)
                ? lv_color32_make(255, 255, 255, 255)
                : lv_color32_make(0, 0, 0, 255);
        }
    }
    lv_obj_invalidate(s_canvas);
}

/* ── 食物：在空格随机生成 ── */
static void place_food(void)
{
    /* 收集所有空格（非蛇身）到栈数组，随机取一个。400 格上限。 */
    bool occupied[GRID_W * GRID_H];
    memset(occupied, 0, sizeof(occupied));
    for (int i = 0; i < s_length; i++) {
        int idx = s_body_y[i] * GRID_W + s_body_x[i];
        if (idx >= 0 && idx < GRID_W * GRID_H) occupied[idx] = true;
    }
    int empty[GRID_W * GRID_H];
    int n = 0;
    for (int i = 0; i < GRID_W * GRID_H; i++) {
        if (!occupied[i]) empty[n++] = i;
    }
    if (n == 0) {  /* 满了 — 胜利，按 Game Over 处理 */
        s_state = SNAKE_STATE_OVER;
        return;
    }
    int pick = empty[esp_random() % n];
    s_food_x = pick % GRID_W;
    s_food_y = pick / GRID_W;
}

/* ── 重置棋盘：清 buffer、画边框、初始蛇、放食物 ── */
static void snake_reset(void)
{
    /* 清全白 */
    memset(s_buf, 0xFF, SNAKE_BUF_SIZE);

    /* 画游戏区边框：1px 黑框围绕 0..199（200×200）。
     * 用 buf_set_pixel 画上下左右各一条线。 */
    for (int x = 0; x < SNAKE_W; x++) {
        buf_set_pixel(x, 0, true);
        buf_set_pixel(x, SNAKE_H - 1, true);
    }
    for (int y = 0; y < SNAKE_H; y++) {
        buf_set_pixel(0, y, true);
        buf_set_pixel(SNAKE_W - 1, y, true);
    }

    /* 初始蛇：3 格，头在 (10,10)，向右 */
    s_length = 3;
    s_body_x[0] = 10; s_body_y[0] = 10;   /* 头 */
    s_body_x[1] = 9;  s_body_y[1] = 10;
    s_body_x[2] = 8;  s_body_y[2] = 10;
    s_dir = DIR_RIGHT;
    s_next_dir = DIR_RIGHT;
    s_score = 0;
    s_step_count = 0;
    place_food();

    /* 画初始蛇身 + 食物 */
    for (int i = 0; i < s_length; i++)
        buf_fill_cell(s_body_x[i], s_body_y[i], true);
    buf_fill_cell(s_food_x, s_food_y, true);

    canvas_refresh_all();
}

/* ── 一步移动 ── */
static void snake_step(void)
{
    if (s_state != SNAKE_STATE_RUNNING) return;

    /* 采纳转向（禁 180°）*/
    if (s_next_dir == DIR_UP    && s_dir != DIR_DOWN)  s_dir = DIR_UP;
    if (s_next_dir == DIR_DOWN  && s_dir != DIR_UP)    s_dir = DIR_DOWN;
    if (s_next_dir == DIR_LEFT  && s_dir != DIR_RIGHT) s_dir = DIR_LEFT;
    if (s_next_dir == DIR_RIGHT && s_dir != DIR_LEFT)  s_dir = DIR_RIGHT;
    s_next_dir = s_dir;

    /* 算新头 */
    int nx = s_body_x[0];
    int ny = s_body_y[0];
    switch (s_dir) {
        case DIR_UP:    ny--; break;
        case DIR_DOWN:  ny++; break;
        case DIR_LEFT:  nx--; break;
        case DIR_RIGHT: nx++; break;
        default: break;
    }

    /* 撞墙（边框内格 1..18，因 0 和 19 是边框线占的像素——但网格坐标
     * 0..19 对应像素 0..199，边框画在像素 0/199。为避免蛇与边框重叠，
     * 游戏区限定网格 1..18？这里简化：撞墙判 nx/ny 越出 0..19。
     * 边框像素线在格子内部，视觉上蛇贴边框走，可接受。*/
    if (nx < 0 || nx >= GRID_W || ny < 0 || ny >= GRID_H) {
        s_state = SNAKE_STATE_OVER;
        return;
    }

    /* 撞自身（除尾——尾下一步会移走，除非吃食物）*/
    bool ate = (nx == s_food_x && ny == s_food_y);
    int check_len = ate ? s_length : s_length - 1;  /* 不吃时尾会移走 */
    for (int i = 0; i < check_len; i++) {
        if (s_body_x[i] == nx && s_body_y[i] == ny) {
            s_state = SNAKE_STATE_OVER;
            return;
        }
    }

    /* 移动 body 数组：从尾往头挪一位，头设新坐标。
     * 吃食物则 length++，不挪尾（数组整体后移）。 */
    if (ate) {
        for (int i = s_length; i > 0; i--) {
            s_body_x[i] = s_body_x[i - 1];
            s_body_y[i] = s_body_y[i - 1];
        }
        s_length++;
        s_score++;
    } else {
        /* 先记旧尾格（刷新用），再整体前移 */
        int old_tail_x = s_body_x[s_length - 1];
        int old_tail_y = s_body_y[s_length - 1];
        for (int i = s_length - 1; i > 0; i--) {
            s_body_x[i] = s_body_x[i - 1];
            s_body_y[i] = s_body_y[i - 1];
        }
        /* 旧尾格清白 */
        buf_fill_cell(old_tail_x, old_tail_y, false);
        refresh_cell(old_tail_x, old_tail_y);
    }
    s_body_x[0] = nx;
    s_body_y[0] = ny;
    buf_fill_cell(nx, ny, true);
    refresh_cell(nx, ny);

    /* 吃食物：放新食物 + 刷新食物格 + 更新分数 */
    if (ate) {
        place_food();
        buf_fill_cell(s_food_x, s_food_y, true);
        refresh_cell(s_food_x, s_food_y);
        char buf[24];
        snprintf(buf, sizeof(buf), "%d", s_score);
        lv_label_set_text(s_score_lbl, buf);
        /* 周期全刷清鬼影 */
        if (s_step_count % FULL_REFRESH_EVERY == 0) {
            canvas_refresh_all();
        }
    }

    s_step_count++;
}

/* ── 步进 timer 回调（LVGL 锁内）── */
static void step_timer_cb(lv_timer_t *t)
{
    (void)t;
    snake_step();
    if (s_state == SNAKE_STATE_OVER) {
        /* 停 timer，显示 Game Over */
        if (s_step_timer) { lv_timer_del(s_step_timer); s_step_timer = NULL; }
        lv_label_set_text(s_hint_lbl, "结束  点屏重开");
        lv_obj_clear_flag(s_hint_lbl, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ── 四象限转向 / 点屏开始-重开 ── */
static void canvas_pressed_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    if (s_state == SNAKE_STATE_READY) {
        /* 点屏开始：启 timer */
        s_state = SNAKE_STATE_RUNNING;
        lv_obj_add_flag(s_hint_lbl, LV_OBJ_FLAG_HIDDEN);
        if (!s_step_timer)
            s_step_timer = lv_timer_create(step_timer_cb, STEP_PERIOD_MS, NULL);
        return;
    }
    if (s_state == SNAKE_STATE_OVER) {
        /* 点屏重开：重置 + 回 READY（再点开始）。
         * 直接重置并立即开始，少一次交互。 */
        snake_reset();
        s_state = SNAKE_STATE_RUNNING;
        lv_obj_add_flag(s_hint_lbl, LV_OBJ_FLAG_HIDDEN);
        if (!s_step_timer)
            s_step_timer = lv_timer_create(step_timer_cb, STEP_PERIOD_MS, NULL);
        return;
    }

    /* RUNNING：四象限转向。中心 (100,100)。 */
    int dx = p.x - SNAKE_W / 2;
    int dy = p.y - SNAKE_H / 2;
    if (dx < 0 && -dx >= (dy < 0 ? -dy : dy))      s_next_dir = DIR_LEFT;
    else if (dx > 0 && dx >= (dy < 0 ? -dy : dy))  s_next_dir = DIR_RIGHT;
    else if (dy < 0)                                s_next_dir = DIR_UP;
    else                                            s_next_dir = DIR_DOWN;
}

/* ── 初始化 ── */
esp_err_t SnakeUi_Init(void)
{
    if (s_screen) return ESP_OK;

    s_screen = lv_obj_create(NULL);
    lv_obj_set_size(s_screen, SNAKE_W, SNAKE_H);
    lv_obj_set_scrollbar_mode(s_screen, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(s_screen, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_set_style_radius(s_screen, 0, 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_add_flag(s_screen, LV_OBJ_FLAG_CLICKABLE);

    s_buf = (uint8_t *)heap_caps_malloc(SNAKE_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_buf) {
        ESP_LOGE(TAG, "buf alloc failed");
        return ESP_ERR_NO_MEM;
    }

    s_canvas_buf = (lv_color32_t *)heap_caps_malloc(
        SNAKE_W * SNAKE_H * sizeof(lv_color32_t), MALLOC_CAP_SPIRAM);
    if (!s_canvas_buf) {
        ESP_LOGE(TAG, "canvas buf alloc failed");
        free(s_buf);
        s_buf = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_canvas = lv_canvas_create(s_screen);
    lv_canvas_set_buffer(s_canvas, s_canvas_buf, SNAKE_W, SNAKE_H, LV_COLOR_FORMAT_ARGB8888);
    lv_obj_set_pos(s_canvas, 0, 0);
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_clear_flag(s_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_canvas, canvas_pressed_cb, LV_EVENT_PRESSED, NULL);

    /* 顶部分数 label（左上角，montserrat_14）。浮在 canvas 上层。 */
    s_score_lbl = lv_label_create(s_screen);
    lv_obj_set_pos(s_score_lbl, 4, 2);
    lv_label_set_text(s_score_lbl, "0");
    lv_obj_set_style_text_font(s_score_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_score_lbl, lv_color_white(), 0);  /* 白字（canvas 此处黑边框）*/
    /* 给分数一个黑底白字小条，确保可读 */
    lv_obj_set_style_bg_color(s_score_lbl, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_score_lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_left(s_score_lbl, 2, 0);
    lv_obj_set_style_pad_right(s_score_lbl, 2, 0);

    /* 中央提示 label（chinese_14）。READY/OVER 时显示。 */
    s_hint_lbl = lv_label_create(s_screen);
    lv_obj_set_pos(s_hint_lbl, 0, SNAKE_H / 2 - 8);
    lv_obj_set_size(s_hint_lbl, SNAKE_W, 16);
    lv_label_set_text(s_hint_lbl, "点屏幕开始");
    lv_obj_set_style_text_font(s_hint_lbl, &lv_font_chinese_14, 0);
    lv_obj_set_style_text_color(s_hint_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_align(s_hint_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(s_hint_lbl, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_hint_lbl, LV_OPA_COVER, 0);

    s_state = SNAKE_STATE_READY;
    snake_reset();

    ESP_LOGI(TAG, "snake_ui init done");
    return ESP_OK;
}

lv_obj_t *SnakeUi_GetScreen(void)
{
    return s_screen;
}

void SnakeUi_Enter(void)
{
    if (!s_screen) return;
    /* 每次进入重置为新局（不复用上一局的残局） */
    snake_reset();
    s_state = SNAKE_STATE_READY;
    lv_label_set_text(s_hint_lbl, "点屏幕开始");
    lv_obj_clear_flag(s_hint_lbl, LV_OBJ_FLAG_HIDDEN);
    /* timer 不在此启——等用户首次点屏（canvas_pressed_cb READY 分支）启 */
}

void SnakeUi_Exit(void)
{
    if (s_step_timer) {
        lv_timer_del(s_step_timer);
        s_step_timer = NULL;
    }
}

void SnakeUi_SetBackCallback(void (*cb)(void))
{
    s_back_cb = cb;
}

void SnakeUi_Tick(void)
{
    /* 供 PC sim 手动驱动（固件用 lv_timer）。 */
    snake_step();
    if (s_state == SNAKE_STATE_OVER) {
        lv_label_set_text(s_hint_lbl, "结束  点屏重开");
        lv_obj_clear_flag(s_hint_lbl, LV_OBJ_FLAG_HIDDEN);
    }
}
