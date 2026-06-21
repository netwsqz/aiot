#ifndef STANDBY_SCREEN_H
#define STANDBY_SCREEN_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct epaper_driver_display;

void StandbyScreen_Draw(struct epaper_driver_display *epd);

/* Save raw 1bpp pixel data (EPD format: 1=white,0=black) as a BMP file in SPIFFS.
 * pixel_data must be exactly 5000 bytes (200×200 @ 1bpp). */
bool StandbyScreen_SaveBmp(const uint8_t *pixel_data, int len);

/* Delete custom standby image from SPIFFS. Returns true if file existed and was removed. */
bool StandbyScreen_DeleteBmp(void);

#ifdef __cplusplus
}
#endif

#endif
