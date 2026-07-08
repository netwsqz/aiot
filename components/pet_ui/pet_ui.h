#ifndef PET_UI_H
#define PET_UI_H

#include <lvgl.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 桌宠表情状态 — 由 data_task 根据设备状态聚合后传入。
 * 优先级（高→低）：SLEEP > LOW_BATT > FOCUS > ANXIOUS > BUSY > HAPPY */
typedef enum {
    PET_STATE_HAPPY = 0,    /* TODO 清空 */
    PET_STATE_BUSY,         /* 有未完成 TODO（1-4） */
    PET_STATE_ANXIOUS,      /* TODO 数量多（>=5） */
    PET_STATE_FOCUS,        /* 番茄钟/计时器运行中 */
    PET_STATE_LOW_BATT,     /* 电量 < 20% */
    PET_STATE_SLEEP,        /* 夜间（22:00-06:00） */
    PET_STATE_COUNT
} pet_state_t;

/* 初始化桌宠屏幕。须在 LVGL 锁内调用 */
esp_err_t PetUi_Init(void);

/* 获取桌宠屏幕 */
lv_obj_t *PetUi_GetScreen(void);

/* 直接设置表情状态并换帧。须在 LVGL 锁内调用 */
void PetUi_SetState(pet_state_t s);

/* data_task 每 30s 调一次：按 TODO 数 / Timer / 电量 / RTC 聚合算状态并换帧。
 * 须在 LVGL 锁内调用 */
void PetUi_Refresh(void);

/* 单击桌宠 → 跳 dashboard（由 main.cpp 设置）。运行在 LVGL 锁内 */
void PetUi_SetBackCallback(void (*cb)(void));

#ifdef __cplusplus
}
#endif

#endif /* PET_UI_H */
