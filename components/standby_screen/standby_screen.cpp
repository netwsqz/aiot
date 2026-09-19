#include "standby_screen.h"
#include "standby_bitmap.h"
#include "epaper_driver_bsp.h"
#include "bmp1bpp.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <esp_log.h>

static const char *TAG = "standby";

#define STANDBY_SPIFFS_PATH  "/spiffs/standby.bmp"

/* Try loading a custom standby image from SPIFFS.
 * Returns true if a valid 200×200 1bpp BMP was loaded into buf. */
static bool standby_load_from_spiffs(uint8_t *buf, int buf_len)
{
    if (buf_len < BMP1BPP_BUF_SIZE) return false;

    FILE *f = fopen(STANDBY_SPIFFS_PATH, "rb");
    if (!f) return false;

    /* BMP 1bpp: 1=黑；EPD 缓冲: 1=白。读进中转缓冲后逐字节取反。 */
    uint8_t *bmp = (uint8_t *)malloc(BMP1BPP_BUF_SIZE);
    if (!bmp) {
        fclose(f);
        return false;
    }
    bool ok = Bmp1Bpp_Read(f, bmp);
    fclose(f);
    if (!ok) {
        free(bmp);
        return false;
    }
    for (int i = 0; i < BMP1BPP_BUF_SIZE; i++) buf[i] = (uint8_t)~bmp[i];
    free(bmp);
    ESP_LOGI(TAG, "Custom standby image loaded from SPIFFS");
    return true;
}

extern "C" void StandbyScreen_Draw(struct epaper_driver_display *epd)
{
    if (!epd) return;

    epd->EPD_Init();

    uint8_t *buf = epd->GetBuffer();

    /* Try custom image from SPIFFS first */
    if (!standby_load_from_spiffs(buf, 5000)) {
        /* Fallback to compiled-in default bitmap */
        extern const uint8_t standby_bitmap[5000];
        memcpy(buf, standby_bitmap, 5000);
    }

    epd->EPD_Display();
}

/* Save raw 1bpp pixel data to SPIFFS as a BMP file.
 * Called by the web upload handler. */
extern "C" bool StandbyScreen_SaveBmp(const uint8_t *pixel_data, int len)
{
    if (len != BMP1BPP_BUF_SIZE) return false;

    FILE *f = fopen(STANDBY_SPIFFS_PATH, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open SPIFFS for write");
        return false;
    }

    /* EPD 缓冲 1=白 → BMP 1bpp 1=黑，先取反再交给共用写入器。 */
    uint8_t *bmp = (uint8_t *)malloc(BMP1BPP_BUF_SIZE);
    if (!bmp) {
        fclose(f);
        return false;
    }
    for (int i = 0; i < BMP1BPP_BUF_SIZE; i++) bmp[i] = (uint8_t)~pixel_data[i];

    bool ok = Bmp1Bpp_Write(f, bmp);
    fclose(f);
    free(bmp);

    if (ok) ESP_LOGI(TAG, "Custom standby BMP saved to SPIFFS");
    else    ESP_LOGE(TAG, "Custom standby BMP write failed");
    return ok;
}

/* Delete custom standby image from SPIFFS. */
extern "C" bool StandbyScreen_DeleteBmp(void)
{
    if (remove(STANDBY_SPIFFS_PATH) == 0) {
        ESP_LOGI(TAG, "Custom standby image deleted");
        return true;
    }
    ESP_LOGW(TAG, "No custom standby image to delete");
    return false;
}
