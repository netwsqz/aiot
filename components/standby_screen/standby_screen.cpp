#include "standby_screen.h"
#include "standby_bitmap.h"
#include "epaper_driver_bsp.h"
#include <string.h>
#include <stdio.h>
#include <esp_log.h>

static const char *TAG = "standby";

#define STANDBY_SPIFFS_PATH  "/spiffs/standby.bmp"
#define BMP_HEADER_SIZE      54
#define BMP_PIXEL_DATA_SIZE  5000   /* 200×200 @ 1bpp = 5000 bytes */

/* Try loading a custom standby image from SPIFFS.
 * Returns true if a valid 200×200 1bpp BMP was loaded into buf. */
static bool standby_load_from_spiffs(uint8_t *buf, int buf_len)
{
    FILE *f = fopen(STANDBY_SPIFFS_PATH, "rb");
    if (!f) return false;

    /* Read BMP header */
    uint8_t hdr[BMP_HEADER_SIZE];
    if (fread(hdr, 1, BMP_HEADER_SIZE, f) != BMP_HEADER_SIZE) {
        ESP_LOGW(TAG, "BMP header read failed");
        fclose(f);
        return false;
    }

    /* Validate: 'BM' signature */
    if (hdr[0] != 'B' || hdr[1] != 'M') {
        ESP_LOGW(TAG, "Not a BMP file");
        fclose(f);
        return false;
    }

    /* Validate: 1bpp (biBitCount=1) */
    uint16_t bpp = hdr[28] | (hdr[29] << 8);
    if (bpp != 1) {
        ESP_LOGW(TAG, "BMP not 1bpp (got %u)", bpp);
        fclose(f);
        return false;
    }

    /* Validate: width=200, height=200 (bottom-up, so height is positive) */
    int32_t w = hdr[18] | (hdr[19] << 8) | (hdr[20] << 16) | (hdr[21] << 24);
    int32_t h = hdr[22] | (hdr[23] << 8) | (hdr[24] << 16) | (hdr[25] << 24);
    if (w != 200 || h != 200) {
        ESP_LOGW(TAG, "BMP size %ldx%ld, need 200x200", (long)w, (long)h);
        fclose(f);
        return false;
    }

    /* BMP 1bpp rows are padded to 4-byte boundary: 200/8 = 25 bytes → already 4-byte aligned */
    int row_bytes = (w + 7) / 8;   /* 25 */
    int row_padded = (row_bytes + 3) & ~3;  /* 28 with padding */

    /* Validate: pixel data offset (must account for color table between header and data).
     * For 1bpp BMP with BITMAPINFOHEADER: offset = 54 (header) + 8 (2-color table × 4 bytes).
     * We read it from the header's bfOffBits field to be robust. */
    uint32_t pixel_offset = hdr[10] | (hdr[11] << 8) | (hdr[12] << 16) | (hdr[13] << 24);
    if (pixel_offset < BMP_HEADER_SIZE || pixel_offset > BMP_HEADER_SIZE + 256) {
        ESP_LOGW(TAG, "BMP pixel offset %lu looks wrong", (unsigned long)pixel_offset);
        fclose(f);
        return false;
    }

    /* BMP stores rows bottom-up; EPD buffer is top-down.
     * Read rows in reverse order and flip into the EPD buffer. */
    uint8_t row_buf[32];  /* enough for one padded row */
    for (int y = 0; y < 200; y++) {
        int src_row = 199 - y;  /* BMP row 199 = top of image */
        fseek(f, pixel_offset + src_row * row_padded, SEEK_SET);
        if (fread(row_buf, 1, row_bytes, f) != (size_t)row_bytes) {
            ESP_LOGW(TAG, "BMP pixel read failed at row %d", y);
            fclose(f);
            return false;
        }
        /* BMP 1bpp: bit=1 means black (palette index 1), bit=0 means white (palette index 0).
         * EPD buffer: bit=1 means white, bit=0 means black.
         * So we need to invert the bits. */
        int dst_offset = y * 25;
        for (int b = 0; b < 25; b++) {
            buf[dst_offset + b] = ~row_buf[b];
        }
    }

    fclose(f);
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
    if (len != BMP_PIXEL_DATA_SIZE) return false;

    /* Build a minimal 1bpp BMP header for 200×200 */
    uint8_t hdr[BMP_HEADER_SIZE];
    memset(hdr, 0, BMP_HEADER_SIZE);

    /* Signature */
    hdr[0] = 'B'; hdr[1] = 'M';
    /* File size: header + row_padded * height = 54 + 28*200 = 5654 */
    uint32_t file_size = BMP_HEADER_SIZE + 28 * 200;
    hdr[2] = file_size & 0xFF;
    hdr[3] = (file_size >> 8) & 0xFF;
    hdr[4] = (file_size >> 16) & 0xFF;
    hdr[5] = (file_size >> 24) & 0xFF;
    /* Pixel data offset: header (54) + color table (8) = 62.
     * With 1bpp BMP using BITMAPINFOHEADER, the color table sits between
     * the header and pixel data. */
    uint32_t pixel_offset = BMP_HEADER_SIZE + 8;  /* 62 */
    hdr[10] = pixel_offset & 0xFF;
    hdr[11] = (pixel_offset >> 8) & 0xFF;
    hdr[12] = (pixel_offset >> 16) & 0xFF;
    hdr[13] = (pixel_offset >> 24) & 0xFF;
    /* DIB header size (BITMAPCOREHEADER = 12, but use BITMAPINFOHEADER = 40) */
    hdr[14] = 40;
    /* Width */
    hdr[18] = 200; hdr[19] = 0; hdr[20] = 0; hdr[21] = 0;
    /* Height (positive = bottom-up) */
    hdr[22] = 200; hdr[23] = 0; hdr[24] = 0; hdr[25] = 0;
    /* Planes */
    hdr[26] = 1; hdr[27] = 0;
    /* Bits per pixel */
    hdr[28] = 1; hdr[29] = 0;
    /* Compression = BI_RGB */
    hdr[30] = 0; hdr[31] = 0; hdr[32] = 0; hdr[33] = 0;
    /* Image size (can be 0 for BI_RGB) */
    hdr[34] = 0; hdr[35] = 0; hdr[36] = 0; hdr[37] = 0;
    /* Resolution (72 DPI ≈ 2835 pixels/meter) */
    hdr[38] = 0x13; hdr[39] = 0x0B;
    hdr[42] = 0x13; hdr[43] = 0x0B;
    /* Colors used = 2 */
    hdr[46] = 2; hdr[47] = 0;
    /* Important colors = 0 */

    /* Color table: index 0 = white (FF FF FF), index 1 = black (00 00 00) */
    uint8_t color_table[8] = {
        0xFF, 0xFF, 0xFF, 0x00,  /* index 0: white */
        0x00, 0x00, 0x00, 0x00,  /* index 1: black */
    };

    FILE *f = fopen(STANDBY_SPIFFS_PATH, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open SPIFFS for write");
        return false;
    }

    fwrite(hdr, 1, BMP_HEADER_SIZE, f);
    fwrite(color_table, 1, 8, f);

    /* Write rows bottom-up, inverting bits (EPD: 1=white, BMP 1bpp: 1=black) */
    int row_bytes = 25;
    int row_padded = 28;
    uint8_t pad[3] = {0};
    for (int y = 199; y >= 0; y--) {
        uint8_t inv[25];
        for (int b = 0; b < 25; b++) inv[b] = ~pixel_data[y * 25 + b];
        fwrite(inv, 1, row_bytes, f);
        if (row_padded > row_bytes) fwrite(pad, 1, row_padded - row_bytes, f);
    }

    fclose(f);
    ESP_LOGI(TAG, "Custom standby BMP saved to SPIFFS");
    return true;
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
