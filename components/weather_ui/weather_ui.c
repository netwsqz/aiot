#include "weather_ui.h"
#include "lvgl_port.h"      /* EPD_WIDTH, EPD_HEIGHT */
#include "font_cn.h"        /* lv_font_chinese_14 */
#include "weather_cache.h"  /* CONFIG_WEATHER_CACHE_TTL_MIN (via Kconfig) */
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ── Icon placeholders ──
 * Real 24×24 RGB565A8 bitmaps are a follow-up task. For now the icon area
 * shows a short Chinese text label so the layout is verifiable on hardware
 * without committing ~5.7KB of placeholder bytes. */
static const char *icon_label(int category)
{
    switch (category) {
        case 0: return "晴";
        case 1: return "云";
        case 2: return "雨";
        case 3: return "雪";
        case 4: return "雾";
        default: return "云";
    }
}

/* ── Screen state ── */
static lv_obj_t *s_screen = NULL;
static weather_snapshot_t s_last_snapshot;   /* last rendered snapshot (for re-render on page entry) */
static bool s_has_rendered = false;          /* whether s_last_snapshot is valid */
static lv_obj_t *s_lbl_temp;        /* big anchor temperature 28px (montserrat_28 has °) */
static lv_obj_t *s_lbl_text;        /* e.g. "多云" */
static lv_obj_t *s_lbl_city;        /* e.g. "北京" — pure black (e-ink has no gray) */
static lv_obj_t *s_divider;         /* thin 1px Swiss rule separating now / forecast */
static lv_obj_t *s_lbl_h_icon[4];   /* hourly column: icon glyph (chinese_14) */
static lv_obj_t *s_lbl_h_temp[4];   /* hourly column: temp "28°" (montserrat_14 has °) */
static lv_obj_t *s_lbl_h_hour[4];   /* hourly column: hour "14" (chinese_14) */
static lv_obj_t *s_lbl_daily;       /* "明日 雨 16/22" */
static lv_obj_t *s_lbl_updated;     /* "14:30更新 / 缓存" */

static void (*s_back_cb)(void) = NULL;

/* Swipe gesture on weather screen → back to dashboard (mirrors todo_ui). */
static void swipe_gesture_cb(lv_event_t *e)
{
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
    if (dir == LV_DIR_RIGHT && s_back_cb) s_back_cb();
}

void WeatherUI_Init(void)
{
    if (s_screen) return;

    s_screen = lv_obj_create(NULL);
    lv_obj_set_size(s_screen, EPD_WIDTH, EPD_HEIGHT);
    lv_obj_set_scrollbar_mode(s_screen, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(s_screen, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_set_style_radius(s_screen, 0, 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);

    /* 刷新按钮已移除（入口搬到 WiFi 管理页）。整屏左滑返回 dashboard。 */
    lv_obj_add_event_cb(s_screen, swipe_gesture_cb, LV_EVENT_GESTURE, NULL);

    /* ── Layout A (Swiss editorial): anchor temp + horizontal rule +
     * 4 hourly mini-columns + daily/updated footer ──
     * Hierarchy: one 28px anchor temp dominates; everything else 14-16px.
     * chinese_14 labels never carry ° (no glyph); montserrat labels do.
     * Detail line (humidity/wind) dropped to relieve crowding — data is
     * still in the snapshot if a future detail screen wants it.
     * All text is pure black: this is a B/W e-ink panel — any OPA<100%
     * would dither to nothing or a noisy mid-gray on the 1bpp framebuffer.
     *
     * 顶部刷新按钮移除后，整体内容上移填充顶部留白（原 y=38 起 → 现 y=14 起），
     * 底部 updated 下移到 y=190，让纵向更均衡。 */

    /* Anchor temp (y=14, montserrat_28 — has ° glyph) */
    s_lbl_temp = lv_label_create(s_screen);
    lv_obj_set_pos(s_lbl_temp, 0, 14);
    lv_obj_set_size(s_lbl_temp, 200, 32);
    lv_obj_set_style_text_font(s_lbl_temp, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_lbl_temp, lv_color_black(), 0);
    lv_obj_set_style_text_align(s_lbl_temp, LV_TEXT_ALIGN_CENTER, 0);

    /* Now text (y=50) */
    s_lbl_text = lv_label_create(s_screen);
    lv_obj_set_pos(s_lbl_text, 0, 50);
    lv_obj_set_size(s_lbl_text, 200, 16);
    lv_obj_set_style_text_font(s_lbl_text, &lv_font_chinese_14, 0);
    lv_obj_set_style_text_color(s_lbl_text, lv_color_black(), 0);
    lv_obj_set_style_text_align(s_lbl_text, LV_TEXT_ALIGN_CENTER, 0);

    /* City (y=68) — pure black: e-ink is B/W only, OPA<100% dithers to nothing */
    s_lbl_city = lv_label_create(s_screen);
    lv_obj_set_pos(s_lbl_city, 0, 68);
    lv_obj_set_size(s_lbl_city, 200, 16);
    lv_obj_set_style_text_font(s_lbl_city, &lv_font_chinese_14, 0);
    lv_obj_set_style_text_color(s_lbl_city, lv_color_black(), 0);
    lv_obj_set_style_text_align(s_lbl_city, LV_TEXT_ALIGN_CENTER, 0);

    /* Thin Swiss horizontal rule at y=90 — separates now / forecast */
    s_divider = lv_obj_create(s_screen);
    lv_obj_set_pos(s_divider, 24, 90);
    lv_obj_set_size(s_divider, 152, 1);
    lv_obj_set_style_bg_color(s_divider, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_divider, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_divider, 0, 0);
    lv_obj_set_style_radius(s_divider, 0, 0);
    lv_obj_set_style_pad_all(s_divider, 0, 0);
    lv_obj_remove_flag(s_divider, LV_OBJ_FLAG_SCROLLABLE);

    /* Hourly caption removed per request — the 4 columns are self-evident. */

    /* Hourly: 4 mini columns. Each column = icon(top) / temp(mid) / hour(bot).
     * Columns at x = 18, 62, 106, 150 (width 32, gap 12) — spreads the full
     * 200px width instead of stacking 4 entries in 2 text strings.
     * Tucked up to y=96 right under the divider (no caption row). */
    for (int i = 0; i < 4; i++) {
        int cx = 18 + i * 44;

        s_lbl_h_icon[i] = lv_label_create(s_screen);
        lv_obj_set_pos(s_lbl_h_icon[i], cx, 96);
        lv_obj_set_size(s_lbl_h_icon[i], 32, 14);
        lv_obj_set_style_text_font(s_lbl_h_icon[i], &lv_font_chinese_14, 0);
        lv_obj_set_style_text_color(s_lbl_h_icon[i], lv_color_black(), 0);
        lv_obj_set_style_text_align(s_lbl_h_icon[i], LV_TEXT_ALIGN_CENTER, 0);

        s_lbl_h_temp[i] = lv_label_create(s_screen);
        lv_obj_set_pos(s_lbl_h_temp[i], cx, 112);
        lv_obj_set_size(s_lbl_h_temp[i], 32, 14);
        lv_obj_set_style_text_font(s_lbl_h_temp[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(s_lbl_h_temp[i], lv_color_black(), 0);
        lv_obj_set_style_text_align(s_lbl_h_temp[i], LV_TEXT_ALIGN_CENTER, 0);

        s_lbl_h_hour[i] = lv_label_create(s_screen);
        lv_obj_set_pos(s_lbl_h_hour[i], cx, 128);
        lv_obj_set_size(s_lbl_h_hour[i], 32, 14);
        lv_obj_set_style_text_font(s_lbl_h_hour[i], &lv_font_chinese_14, 0);
        lv_obj_set_style_text_color(s_lbl_h_hour[i], lv_color_black(), 0);
        lv_obj_set_style_text_align(s_lbl_h_hour[i], LV_TEXT_ALIGN_CENTER, 0);
    }

    /* Daily footer (y=156) */
    s_lbl_daily = lv_label_create(s_screen);
    lv_obj_set_pos(s_lbl_daily, 0, 156);
    lv_obj_set_size(s_lbl_daily, 200, 16);
    lv_obj_set_style_text_font(s_lbl_daily, &lv_font_chinese_14, 0);
    lv_obj_set_style_text_color(s_lbl_daily, lv_color_black(), 0);
    lv_obj_set_style_text_align(s_lbl_daily, LV_TEXT_ALIGN_CENTER, 0);

    /* Updated timestamp (y=178) — pure black */
    s_lbl_updated = lv_label_create(s_screen);
    lv_obj_set_pos(s_lbl_updated, 0, 178);
    lv_obj_set_size(s_lbl_updated, 200, 14);
    lv_obj_set_style_text_font(s_lbl_updated, &lv_font_chinese_14, 0);
    lv_obj_set_style_text_color(s_lbl_updated, lv_color_black(), 0);
    lv_obj_set_style_text_align(s_lbl_updated, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_lbl_updated, "");
}

lv_obj_t *WeatherUI_GetScreen(void)
{
    return s_screen;
}

void WeatherUI_Render(const weather_snapshot_t *snapshot, bool is_cached)
{
    if (!s_screen || !snapshot) return;

    /* Stash the snapshot so WeatherUI_RenderIfCached() can repaint on
     * page entry without data_task having to re-fetch. */
    s_last_snapshot = *snapshot;
    s_has_rendered = true;

    /* Anchor temp — montserrat_28 has the ° glyph */
    char buf[32];
    snprintf(buf, sizeof(buf), "%d" "\xC2\xB0" "C", snapshot->now.temp);
    lv_label_set_text(s_lbl_temp, buf);

    lv_label_set_text(s_lbl_text, snapshot->now.text);
    lv_label_set_text(s_lbl_city, snapshot->now.city_name);

    /* Hourly: 4 mini columns. icon-glyph (chinese_14, no °), temp "28°"
     * (montserrat_14, has °), hour "14" (chinese_14). All pure black. */
    char tbuf[8], hbuf[8];
    for (int i = 0; i < 4; i++) {
        int cat = WeatherFetch_IconCategory(snapshot->hourly[i].icon);
        lv_label_set_text(s_lbl_h_icon[i], icon_label(cat));

        snprintf(tbuf, sizeof(tbuf), "%d" "\xC2\xB0", snapshot->hourly[i].temp);
        lv_label_set_text(s_lbl_h_temp[i], tbuf);

        snprintf(hbuf, sizeof(hbuf), "%02d", snapshot->hourly[i].hour);
        lv_label_set_text(s_lbl_h_hour[i], hbuf);
    }

    /* Daily — only 明日 (后日 dropped). ASCII digits render fine in chinese_14.
     * Guard against an empty daily[] (QWeather 3d may return only today,
     * which parse_daily skips), otherwise we'd render "明日  0/0". */
    if (WEATHER_DAILY_COUNT > 0 && snapshot->daily[0].text_day[0] != '\0') {
        snprintf(buf, sizeof(buf), "明日 %s %d/%d",
                 snapshot->daily[0].text_day,
                 snapshot->daily[0].temp_min, snapshot->daily[0].temp_max);
    } else {
        snprintf(buf, sizeof(buf), "明日 --");
    }
    lv_label_set_text(s_lbl_daily, buf);

    /* Updated timestamp + cache annotation */
    time_t ft = (time_t)snapshot->fetch_time;
    struct tm t_buf;
    struct tm *t = localtime_r(&ft, &t_buf);
    char ubuf[40];
    if (t) {
        snprintf(ubuf, sizeof(ubuf), "%02d:%02d更新", t->tm_hour, t->tm_min);
    } else {
        snprintf(ubuf, sizeof(ubuf), "--:--更新");
    }
    if (is_cached) {
        int age_min = (int)(((int64_t)time(NULL) - snapshot->fetch_time) / 60);
        if (age_min > CONFIG_WEATHER_CACHE_TTL_MIN) {
            snprintf(ubuf + strlen(ubuf), sizeof(ubuf) - strlen(ubuf), " / 数据过旧");
        } else {
            snprintf(ubuf + strlen(ubuf), sizeof(ubuf) - strlen(ubuf), " / 缓存");
        }
    }
    lv_label_set_text(s_lbl_updated, ubuf);
}

void WeatherUI_RenderIfCached(void)
{
    /* Re-render the last snapshot onto the (now visible) weather screen.
     * Called from ui_switch_page on PAGE_WEATHER entry. Marks as cached
     * so the "缓存" annotation shows, since this is not a fresh fetch. */
    if (s_has_rendered) {
        WeatherUI_Render(&s_last_snapshot, true);
    }
}

void WeatherUI_SetBackCallback(void (*cb)(void))
{
    s_back_cb = cb;
}
