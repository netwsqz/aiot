#include "wifi_ui.h"
#include "lvgl_port.h"      /* EPD_WIDTH, EPD_HEIGHT */
#include "font_cn.h"        /* lv_font_chinese_14 */
#include "wifi_ap.h"        /* WIFI_AP_SSID (用于 AP SSID 默认显示) */
#include <stdio.h>
#include <string.h>

/* ── 控件句柄 ── */
static lv_obj_t *s_screen = NULL;

static lv_obj_t *s_lbl_title;            /* "WiFi 管理" */

/* AP 状态块 */
static lv_obj_t *s_lbl_ap_label;         /* "热点" */
static lv_obj_t *s_lbl_ap_state;         /* "关闭" / "开启" */
static lv_obj_t *s_lbl_ap_ssid;          /* "AIoT-TODO" */
static lv_obj_t *s_lbl_ap_stas;          /* "已连 0 台" */

/* STA 状态块 */
static lv_obj_t *s_lbl_sta_label;        /* "网络" */
static lv_obj_t *s_lbl_sta_state;        /* "未连接" / "已连接" */
static lv_obj_t *s_lbl_sta_ssid;         /* "SSID: xxx" */
static lv_obj_t *s_lbl_sta_ip;           /* "IP: 192.168.1.10" */

/* 按钮区 */
static lv_obj_t *s_btn_toggle_ap;        /* "开启 AP" / "关闭 AP" */
static lv_obj_t *s_btn_reconnect;        /* "重连网络" */
static lv_obj_t *s_btn_refresh;          /* "刷新天气" */
static lv_obj_t *s_btn_back;             /* "返回" */

/* 当前 AP 状态（决定 toggle 按钮文案） */
static bool s_ap_active_cached = false;

/* ── 回调 ── */
static void (*s_back_cb)(void) = NULL;
static void (*s_toggle_ap_cb)(void) = NULL;
static void (*s_reconnect_cb)(void) = NULL;
static void (*s_refresh_cb)(void) = NULL;

/* ── 手势返回（左滑 → dashboard） ── */
static void swipe_gesture_cb(lv_event_t *e)
{
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
    if (dir == LV_DIR_RIGHT && s_back_cb) s_back_cb();
}

/* ── 按钮回调 ── */
static void toggle_ap_btn_cb(lv_event_t *e)
{
    (void)e;
    if (s_toggle_ap_cb) s_toggle_ap_cb();
}

static void reconnect_btn_cb(lv_event_t *e)
{
    (void)e;
    if (s_reconnect_cb) s_reconnect_cb();
}

static void refresh_btn_cb(lv_event_t *e)
{
    (void)e;
    if (s_refresh_cb) s_refresh_cb();
}

static void back_btn_cb(lv_event_t *e)
{
    (void)e;
    if (s_back_cb) s_back_cb();
}

/* ── 通用文本按钮创建（带边框，e-ink 友好） ──
 * size: w×h，位置 (x,y)，文案 text，回调 cb。 */
static lv_obj_t *create_text_btn(lv_obj_t *parent, const char *text,
                                 lv_event_cb_t cb, int x, int y, int w, int h)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_black(), 0);
    lv_obj_set_style_radius(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);   /* e-ink 无阴影 */
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_chinese_14, 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_center(lbl);
    /* 让按钮上的手势冒泡到屏幕，便于左滑返回 */
    lv_obj_add_flag(btn, LV_OBJ_FLAG_GESTURE_BUBBLE);
    return btn;
}

/* 通用标签创建：纯黑中文 14，左对齐 */
static lv_obj_t *create_label(lv_obj_t *parent, int x, int y, int w, int h,
                              lv_text_align_t align)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_set_pos(lbl, x, y);
    lv_obj_set_size(lbl, w, h);
    lv_obj_set_style_text_font(lbl, &lv_font_chinese_14, 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_set_style_text_align(lbl, align, 0);
    lv_label_set_text(lbl, "");
    return lbl;
}

/* 1px 分隔线（Swiss 风格水平细线） */
static lv_obj_t *create_rule(lv_obj_t *parent, int x, int y, int w)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_set_pos(r, x, y);
    lv_obj_set_size(r, w, 1);
    lv_obj_set_style_bg_color(r, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_radius(r, 0, 0);
    lv_obj_set_style_pad_all(r, 0, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    return r;
}

void WifiUi_Init(void)
{
    if (s_screen) return;

    /* 200×200 白底根屏 — 同 todo_ui / weather_ui 七行设置 */
    s_screen = lv_obj_create(NULL);
    lv_obj_set_size(s_screen, EPD_WIDTH, EPD_HEIGHT);
    lv_obj_set_scrollbar_mode(s_screen, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(s_screen, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_set_style_radius(s_screen, 0, 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);

    /* 标题 (y=6, h=16) */
    s_lbl_title = create_label(s_screen, 0, 6, EPD_WIDTH, 16, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(s_lbl_title, "WiFi 管理");

    /* 顶部分隔线 y=26 */
    create_rule(s_screen, 16, 26, EPD_WIDTH - 32);

    /* ── AP 块 (y=32..70) ── */
    s_lbl_ap_label = create_label(s_screen, 12, 32, 40, 16, LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(s_lbl_ap_label, "热点");

    s_lbl_ap_state = create_label(s_screen, 52, 32, 80, 16, LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(s_lbl_ap_state, "关闭");

    s_lbl_ap_ssid = create_label(s_screen, 12, 50, EPD_WIDTH - 24, 16, LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(s_lbl_ap_ssid, WIFI_AP_SSID);

    s_lbl_ap_stas = create_label(s_screen, 12, 68, EPD_WIDTH - 24, 14, LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(s_lbl_ap_stas, "已连 0 台");

    /* 中分隔线 y=88 */
    create_rule(s_screen, 16, 88, EPD_WIDTH - 32);

    /* ── STA 块 (y=94..130) ── */
    s_lbl_sta_label = create_label(s_screen, 12, 94, 40, 16, LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(s_lbl_sta_label, "网络");

    s_lbl_sta_state = create_label(s_screen, 52, 94, 100, 16, LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(s_lbl_sta_state, "未连接");

    s_lbl_sta_ssid = create_label(s_screen, 12, 114, EPD_WIDTH - 24, 16, LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(s_lbl_sta_ssid, "SSID: -");

    s_lbl_sta_ip = create_label(s_screen, 12, 130, EPD_WIDTH - 24, 14, LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(s_lbl_sta_ip, "IP: -");

    /* 底部分隔线 y=148 */
    create_rule(s_screen, 16, 148, EPD_WIDTH - 32);

    /* ── 按钮区 (y=156..180) ──
     * 两行：上行 AP 启停 + STA 重连；下行 刷新天气 + 返回 */
    s_btn_toggle_ap = create_text_btn(s_screen, "开启 AP",
                                      toggle_ap_btn_cb, 10, 156, 86, 22);
    s_btn_reconnect = create_text_btn(s_screen, "重连网络",
                                      reconnect_btn_cb, 104, 156, 86, 22);
    s_btn_refresh = create_text_btn(s_screen, "刷新天气",
                                    refresh_btn_cb, 10, 180, 86, 22);
    s_btn_back = create_text_btn(s_screen, "返回",
                                 back_btn_cb, 104, 180, 86, 22);

    /* 整屏左滑返回 */
    lv_obj_add_event_cb(s_screen, swipe_gesture_cb, LV_EVENT_GESTURE, NULL);
}

lv_obj_t *WifiUi_GetScreen(void)
{
    return s_screen;
}

void WifiUi_UpdateStatus(bool sta_connected, bool ap_active,
                         const char *sta_ssid, const char *sta_ip,
                         const char *ap_ssid, int ap_stas)
{
    if (!s_screen) return;

    /* AP 状态文案 */
    lv_label_set_text(s_lbl_ap_state, ap_active ? "开启" : "关闭");
    s_ap_active_cached = ap_active;

    /* AP SSID（外部可覆盖默认） */
    lv_label_set_text(s_lbl_ap_ssid, ap_ssid ? ap_ssid : WIFI_AP_SSID);

    /* AP 站点数 */
    char stas_buf[24];
    snprintf(stas_buf, sizeof(stas_buf), "已连 %d 台", ap_stas);
    lv_label_set_text(s_lbl_ap_stas, stas_buf);

    /* STA 状态文案 */
    lv_label_set_text(s_lbl_sta_state, sta_connected ? "已连接" : "未连接");

    /* STA SSID */
    char ssid_buf[48];
    if (sta_connected && sta_ssid && sta_ssid[0]) {
        snprintf(ssid_buf, sizeof(ssid_buf), "SSID: %s", sta_ssid);
    } else {
        snprintf(ssid_buf, sizeof(ssid_buf), "SSID: -");
    }
    lv_label_set_text(s_lbl_sta_ssid, ssid_buf);

    /* STA IP */
    char ip_buf[24];
    if (sta_connected && sta_ip && sta_ip[0]) {
        snprintf(ip_buf, sizeof(ip_buf), "IP: %s", sta_ip);
    } else {
        snprintf(ip_buf, sizeof(ip_buf), "IP: -");
    }
    lv_label_set_text(s_lbl_sta_ip, ip_buf);

    /* AP 启停按钮文案随状态切换 */
    lv_obj_t *lbl = lv_obj_get_child(s_btn_toggle_ap, 0);
    if (lbl) {
        lv_label_set_text(lbl, ap_active ? "关闭 AP" : "开启 AP");
    }
}

void WifiUi_SetBackCallback(void (*cb)(void))
{
    s_back_cb = cb;
}

void WifiUi_SetToggleApCallback(void (*cb)(void))
{
    s_toggle_ap_cb = cb;
}

void WifiUi_SetReconnectCallback(void (*cb)(void))
{
    s_reconnect_cb = cb;
}

void WifiUi_SetRefreshWeatherCallback(void (*cb)(void))
{
    s_refresh_cb = cb;
}
