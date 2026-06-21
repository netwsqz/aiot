#ifndef LVGL_PORT_H
#define LVGL_PORT_H

#include "lvgl.h"

/* Forward declare EPD driver (C++ class) */
#ifdef __cplusplus
class epaper_driver_display;
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ── Display dimensions ────────────────────────────── */
#define EPD_WIDTH               200
#define EPD_HEIGHT              200

/* ── LVGL timing ──────────────────────────────────── */
#define LVGL_TICK_PERIOD_MS     5
#define LVGL_TASK_MAX_DELAY_MS  500
#define LVGL_TASK_MIN_DELAY_MS  50

/**
 * Init LVGL: create display, allocate PSRAM buffers, start tick timer
 * and handler task.  Uses mock flush callback initially.
 */
void Lvgl_PortInit(void);

#ifdef __cplusplus
/**
 * Set the EPD driver instance for real display flush.
 * Must be called before any UI rendering.  C++ only.
 */
void Lvgl_SetEpd(epaper_driver_display *epd);
#endif

/** Acquire LVGL mutex.  timeout_ms = -1 for infinite wait. */
bool Lvgl_Lock(int timeout_ms);

/** Release LVGL mutex. */
void Lvgl_Unlock(void);

/** Stop tick timer, delete handler task and mutex. Call before deep sleep. */
void Lvgl_PortDeinit(void);

#ifdef __cplusplus
}
#endif

#endif /* LVGL_PORT_H */
