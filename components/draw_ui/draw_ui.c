#include "draw_ui.h"
#include "bmp1bpp.h"

#include <string.h>
#include <stdio.h>
#include "lvgl.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "draw_ui";

/* ── 尺寸常量 ── */
#define DRAW_W          200
#define DRAW_H          200
#define DRAW_ROW_BYTES  ((DRAW_W + 7) / 8)   /* 25 字节/行 */
#define DRAW_BUF_SIZE   (DRAW_W * DRAW_H / 8) /* 5000 字节 */

#define DRAW_SPIFFS_PATH  "/spiffs/draw.bmp"

/* ── 静态状态 ── */
static lv_obj_t      *s_draw_screen = NULL;
static lv_obj_t      *s_canvas      = NULL;
static lv_color32_t  *s_canvas_buf  = NULL;  /* ARGB8888，200*200*4=160KB PSRAM */
static uint8_t       *s_draw_buf    = NULL;  /* 1bpp 笔迹累加缓冲，EPD 格式 1=白 0=黑 */
static void         (*s_back_cb)(void) = NULL;

/* 上一触摸点（笔触连线用）。-1 表示未按下 */
static int s_last_x = -1;
static int s_last_y = -1;

/* ── 1bpp 画布操作（EPD 格式：bit=1 白，bit=0 黑；byte 内 MSB=最左像素）── */
static inline void buf_set_pixel(int x, int y, bool black)
{
    if (x < 0 || x >= DRAW_W || y < 0 || y >= DRAW_H) return;
    uint16_t idx = (uint16_t)(y * DRAW_ROW_BYTES + (x >> 3));
    uint8_t  bit = (uint8_t)(0x80 >> (x & 7));
    if (black) s_draw_buf[idx] &= ~bit;   /* 0=黑 */
    else       s_draw_buf[idx] |=  bit;   /* 1=白 */
}

/* 画一个 3×3 圆头笔触（中心 + 8 邻域），让线宽 ~3px。
 * e-ink 上 1px 细线偏细、触摸抖动显得"画不准"；加粗后视觉更贴合手指，
 * 也遮盖 FT6336 单点抖动。*/
static inline void buf_stamp_brush(int cx, int cy, bool black)
{
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++)
            buf_set_pixel(cx + dx, cy + dy, black);
}

/* Bresenham 画线，沿线盖 3×3 笔触（黑笔）。只写 1bpp buffer，不触发显示。*/
static void draw_line(int x0, int y0, int x1, int y1)
{
    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int dy = (y1 > y0) ? (y0 - y1) : (y1 - y0);  /* 负斜率时 dy 为正 */
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;
    int x = x0, y = y0;
    for (;;) {
        buf_stamp_brush(x, y, true);
        if (x == x1 && y == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x += sx; }
        if (e2 <= dx) { err += dx; y += sy; }
    }
}

/* 把 1bpp 笔迹缓冲同步到 ARGB8888 canvas，再 invalidate 触发一次局刷。
 * 抬笔时调；拖动期间不调（避开 e-ink 300ms/次局刷物理上限）。*/
static void canvas_refresh(void)
{
    lv_canvas_fill_bg(s_canvas, lv_color_white(), LV_OPA_COVER);
    lv_color32_t *px = (lv_color32_t *)s_canvas_buf;
    for (int y = 0; y < DRAW_H; y++) {
        for (int x = 0; x < DRAW_W; x++) {
            uint8_t byte = s_draw_buf[y * DRAW_ROW_BYTES + (x >> 3)];
            uint8_t bit  = (uint8_t)(0x80 >> (x & 7));
            /* EPD: bit=1 白，bit=0 黑 → 0=黑 */
            px[y * DRAW_W + x] = (byte & bit)
                ? lv_color32_make(255, 255, 255, 255)
                : lv_color32_make(0, 0, 0, 255);
        }
    }
    lv_obj_invalidate(s_canvas);
}

/* 清空画布为全白（进入画板时调）。*/
static void canvas_clear(void)
{
    memset(s_draw_buf, 0xFF, DRAW_BUF_SIZE);
    canvas_refresh();
}

/* ── 触摸事件 ──
 * 用 LVGL 原生 pointer indev（lvgl_port 已注册）。
 * PRESSED:   记起点并画一个点（单点点击也能留痕）
 * PRESSING:  拖动期间持续触发，连线 prev→cur，更新 prev，不 invalidate（抬笔统一刷）
 * RELEASED:  一次 canvas_refresh 出图
 *
 * 退出交互：不用长按（LVGL 的 LONG_PRESSED 在任意 >=400ms 按压时必触发，
 * 与画线不可共存）。退出靠 BOOT 键单击——见 main.cpp 的 boot_key_task。
 * DrawUi_SaveAndExit 由该路径调用：保存笔迹 + 导航回 dashboard。*/
static void draw_pressed_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    s_last_x = p.x;
    s_last_y = p.y;
    buf_stamp_brush(p.x, p.y, true);   /* 起点留痕（3×3）*/
}

static void draw_pressing_cb(lv_event_t *e)
{
    if (s_last_x < 0) return;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    draw_line(s_last_x, s_last_y, p.x, p.y);
    s_last_x = p.x;
    s_last_y = p.y;
}

static void draw_released_cb(lv_event_t *e)
{
    (void)e;
    s_last_x = -1;
    s_last_y = -1;
    canvas_refresh();
}

/* ──────────────────────────────────────────────────────────────
 * BMP 读写（1bpp，200×200）—— 格式实现在 bmp1bpp.c，standby_screen 共用。
 * 位约定转换在这里做一次：EPD 缓冲 1=白，BMP 1bpp 1=黑，故写出/读入各取反。
 * ────────────────────────────────────────────────────────────── */

void draw_ui_save_current(void)
{
    if (!s_draw_buf) return;

    uint8_t *bmp = heap_caps_malloc(BMP1BPP_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!bmp) {
        ESP_LOGE(TAG, "save buf alloc failed");
        return;
    }
    for (int i = 0; i < DRAW_BUF_SIZE; i++) bmp[i] = (uint8_t)~s_draw_buf[i];

    FILE *f = fopen(DRAW_SPIFFS_PATH, "wb");
    if (!f) {
        ESP_LOGE(TAG, "open %s for write failed", DRAW_SPIFFS_PATH);
        free(bmp);
        return;
    }
    bool ok = Bmp1Bpp_Write(f, bmp);
    fclose(f);
    free(bmp);
    if (ok) ESP_LOGI(TAG, "draw image saved");
    else    ESP_LOGE(TAG, "draw image save failed");
}

/* 保存当前笔迹并退出回 dashboard。由 BOOT 键单击路径调用（替代长按退出）。
 * 保存由 DrawUi 自己做，导航靠 s_back_cb（main.cpp 注入的 ui_switch_page(DASHBOARD)）。*/
void DrawUi_SaveAndExit(void)
{
    draw_ui_save_current();
    if (s_back_cb) s_back_cb();
}

bool DrawUi_HasSavedImage(void)
{
    FILE *f = fopen(DRAW_SPIFFS_PATH, "rb");
    if (!f) return false;
    bool ok = Bmp1Bpp_Validate(f);
    fclose(f);
    return ok;
}

/* 缩略图目标尺寸（右下角占位区 100×100）。
 * 源 200×200 → 2×2 块多数投票降到 100×100，4 个像素里 >=2 黑则该像素黑。*/
#define THUMB_W 100
#define THUMB_H 100

/* 把 SPIFFS 里保存的作品降采样渲染到 dst canvas（须为 THUMB_W×THUMB_H ARGB8888）。
 * 无文件或校验失败返回 false。dst_buf 须至少 THUMB_W*THUMB_H*4 字节。
 * 黑(BMP bit=1) → 0xFF000000，白 → 0xFFFFFFFF。
 * 在 LVGL 锁内调用（写 canvas buffer + invalidate）。*/
bool DrawUi_RenderThumbnail(uint8_t *dst_buf, int len)
{
    if (!dst_buf || len < THUMB_W * THUMB_H * 4) return false;

    FILE *f = fopen(DRAW_SPIFFS_PATH, "rb");
    if (!f) return false;

    /* 借 Bmp1Bpp_Read 做校验 + 取像素。这里**不要**取反：下面的多数投票
     * 是按 BMP 位约定（1=黑）写的，取反会得到负片缩略图。 */
    uint8_t *src = heap_caps_malloc(BMP1BPP_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!src || !Bmp1Bpp_Read(f, src)) {
        fclose(f);
        free(src);
        return false;
    }
    fclose(f);

    lv_color32_t *px = (lv_color32_t *)dst_buf;

    for (int ty = 0; ty < THUMB_H; ty++) {
        int sy0 = ty * 2;
        int sy1 = ty * 2 + 1;

        for (int tx = 0; tx < THUMB_W; tx++) {
            int sx0 = tx * 2, sx1 = tx * 2 + 1;
            /* 数 4 个源像素里黑的个数。BMP 1=黑。*/
            int black = 0;
            uint8_t b;
            b = src[sy0 * BMP1BPP_ROW_BYTES + (sx0 >> 3)]; if (b & (0x80 >> (sx0 & 7))) black++;
            b = src[sy0 * BMP1BPP_ROW_BYTES + (sx1 >> 3)]; if (b & (0x80 >> (sx1 & 7))) black++;
            b = src[sy1 * BMP1BPP_ROW_BYTES + (sx0 >> 3)]; if (b & (0x80 >> (sx0 & 7))) black++;
            b = src[sy1 * BMP1BPP_ROW_BYTES + (sx1 >> 3)]; if (b & (0x80 >> (sx1 & 7))) black++;
            /* >=2 黑 → 黑（多数投票，保留笔迹主体）*/
            px[ty * THUMB_W + tx] = (black >= 2)
                ? lv_color32_make(0, 0, 0, 255)
                : lv_color32_make(255, 255, 255, 255);
        }
    }

    free(src);
    return true;
}

/* ──────────────────────────────────────────────────────────────
 * 公开 API
 * ────────────────────────────────────────────────────────────── */
esp_err_t DrawUi_Init(void)
{
    if (s_draw_screen) return ESP_OK;

    s_draw_screen = lv_obj_create(NULL);
    lv_obj_set_size(s_draw_screen, DRAW_W, DRAW_H);
    lv_obj_set_scrollbar_mode(s_draw_screen, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(s_draw_screen, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_draw_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_draw_screen, 0, 0);
    lv_obj_set_style_radius(s_draw_screen, 0, 0);
    lv_obj_set_style_pad_all(s_draw_screen, 0, 0);
    lv_obj_add_flag(s_draw_screen, LV_OBJ_FLAG_CLICKABLE);

    /* 1bpp 笔迹缓冲（PSRAM，与 EPD 显存惯例一致）*/
    s_draw_buf = (uint8_t *)heap_caps_malloc(DRAW_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_draw_buf) {
        ESP_LOGE(TAG, "draw buf alloc failed");
        return ESP_ERR_NO_MEM;
    }

    /* ARGB8888 canvas（仿 pet_ui，避开 LVGL v9 I1 对齐坑）*/
    s_canvas_buf = (lv_color32_t *)heap_caps_malloc(
        DRAW_W * DRAW_H * sizeof(lv_color32_t), MALLOC_CAP_SPIRAM);
    if (!s_canvas_buf) {
        ESP_LOGE(TAG, "canvas buf alloc failed");
        free(s_draw_buf);
        s_draw_buf = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_canvas = lv_canvas_create(s_draw_screen);
    lv_canvas_set_buffer(s_canvas, s_canvas_buf, DRAW_W, DRAW_H, LV_COLOR_FORMAT_ARGB8888);
    lv_obj_set_pos(s_canvas, 0, 0);

    /* canvas 是真正的触摸命中对象（铺满 200×200）。事件直接注册在 canvas 上，
     * 不依赖冒泡到 screen——路径最短、最稳。
     * PRESS_LOCK：按下后锁定到 canvas，PRESSING 期间 LVGL 不再重新搜索命中对象
     * （lv_indev.c:1268 的 act_obj 重选会发 PRESS_LOST 并重置 vect/press_moved，
     * 导致长折线飞线 + 画不过某些"界限"）。锁死后手指拖出边界事件仍归 canvas。*/
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_clear_flag(s_canvas, LV_OBJ_FLAG_SCROLLABLE);   /* 画板不滚动，避免 scroll_obj 抢占 */

    /* 触摸：画线靠 PRESSED/PRESSING/RELEASED。
     * 退出靠 BOOT 键单击（见 DrawUi_SaveAndExit + main.cpp boot_key_task），
     * 不用长按——LONG_PRESSED 在任意 >=400ms 按压时必触发，与画线冲突。*/
    lv_obj_add_event_cb(s_canvas, draw_pressed_cb,  LV_EVENT_PRESSED,   NULL);
    lv_obj_add_event_cb(s_canvas, draw_pressing_cb, LV_EVENT_PRESSING,  NULL);
    lv_obj_add_event_cb(s_canvas, draw_released_cb, LV_EVENT_RELEASED,  NULL);

    canvas_clear();   /* 进入即空白 */
    ESP_LOGI(TAG, "draw_ui init done");
    return ESP_OK;
}

lv_obj_t *DrawUi_GetScreen(void)
{
    return s_draw_screen;
}

void DrawUi_Enter(void)
{
    if (!s_draw_screen) return;
    /* 每次进入即清空，不恢复上次作品（按"进入即空白"决策）*/
    canvas_clear();
}

void DrawUi_SetBackCallback(void (*cb)(void))
{
    s_back_cb = cb;
}

/* ── indev 采样提速 ──
 * LVGL pointer indev 默认 33ms 读一次（CONFIG_LV_DEF_REFR_PERIOD=33），
 * 画线时手指在两次采样间移动很多像素，Bresenham 只能直线相连 → 曲线变折线。
 * 画板期间把读取周期降到 8ms（~125Hz），离开时恢复 33ms。*/
static lv_indev_t *s_fast_indev = NULL;

void DrawUi_SetIndevFast(lv_indev_t *indev)
{
    if (!indev) return;
    s_fast_indev = indev;
    lv_timer_t *t = lv_indev_get_read_timer(indev);
    if (t) lv_timer_set_period(t, 8);
}

void DrawUi_RestoreIndevPeriod(void)
{
    if (!s_fast_indev) return;
    lv_timer_t *t = lv_indev_get_read_timer(s_fast_indev);
    if (t) lv_timer_set_period(t, 33);   /* 恢复 CONFIG_LV_DEF_REFR_PERIOD 默认值 */
    s_fast_indev = NULL;
}
