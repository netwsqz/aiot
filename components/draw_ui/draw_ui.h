#ifndef DRAW_UI_H
#define DRAW_UI_H

#include <lvgl.h>
#include <esp_err.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化画板屏幕。须在 LVGL 锁内调用。幂等。
 * 进入画板即空白画布（不从上次作品恢复——重画靠退出再进入）。*/
esp_err_t DrawUi_Init(void);

/* 获取画板屏幕，供 ui_switch_page 懒加载。*/
lv_obj_t *DrawUi_GetScreen(void);

/* 进入画板时清空画布为全白（每次进入即空白，不恢复上次作品）。
 * 须在 LVGL 锁内调用。由 ui_switch_page(PAGE_DRAW) 触发。*/
void DrawUi_Enter(void);

/* 退出画板前保存当前笔迹到 SPIFFS（/spiffs/draw.bmp）。
 * 供 BOOT 键"单击退出"路径调用——不再用长按退出，因为 LVGL 的
 * LONG_PRESSED 在任意 >=400ms 按压时必触发，与画线不可共存。*/
void DrawUi_SaveAndExit(void);

/* 把触摸 indev 的读取周期临时降到 8ms（画板期间提高采样密度，避免
 * 曲线变折线）。退出画板时调 DrawUi_RestoreIndevPeriod 恢复默认 33ms。
 * 二者都须在 LVGL 锁内调用。NULL indev 时为 no-op。*/
void DrawUi_SetIndevFast(lv_indev_t *indev);
void DrawUi_RestoreIndevPeriod(void);

/* 退出画板前的回调（由 ui_init_all() 注入，设为 ui_back_to_dashboard）。
 * 运行在 LVGL 锁内。DrawUi 在回调触发前先把当前笔迹保存到 SPIFFS。*/
void DrawUi_SetBackCallback(void (*cb)(void));

/* 检查 SPIFFS 里是否有已保存的画板作品。
 * 供 dashboard 右下角占位图判断"显示作品还是文字占位"。*/
bool DrawUi_HasSavedImage(void);

/* 把已保存的作品降采样（200×200 → 100×100，2×2 多数投票）渲染到 dst ARGB8888 buffer。
 * 供 dashboard 右下角缩略图。dst_buf 须至少 100*100*4=40000 字节。
 * 成功返回 true，无文件或校验失败返回 false。须在 LVGL 锁内调用。*/
bool DrawUi_RenderThumbnail(uint8_t *dst_buf, int len);

#ifdef __cplusplus
}
#endif

#endif /* DRAW_UI_H */
