#include "bmp1bpp.h"

#include <string.h>
#include "esp_log.h"

static const char *TAG = "bmp1bpp";

/* BMP 头字段偏移（BITMAPFILEHEADER + BITMAPINFOHEADER，小端） */
#define OFF_FILESIZE  2
#define OFF_PIXELDATA 10
#define OFF_DIBHDR    14
#define OFF_WIDTH     18
#define OFF_HEIGHT    22
#define OFF_PLANES    26
#define OFF_BPP       28
#define OFF_COMPRESS  30
#define OFF_IMAGESIZE 34
#define OFF_XPPM      38
#define OFF_YPPM      42
#define OFF_COLORS    46

#define XPPM_72DPI    0x0B13   /* 72 DPI ≈ 2835 像素/米 */

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

/* 读 54 字节头。返回 false 表示读不全。不做语义校验。 */
static bool read_header(FILE *f, uint8_t *hdr)
{
    return fread(hdr, 1, BMP1BPP_HEADER, f) == BMP1BPP_HEADER;
}

/* 语义校验。失败返回 false，cause 指向静态字符串说明哪一项不对。 */
static bool check_header(const uint8_t *hdr, const char **cause)
{
    if (hdr[0] != 'B' || hdr[1] != 'M') {
        *cause = "not a BMP ('BM' signature missing)";
        return false;
    }
    if (get_u16(hdr + OFF_BPP) != 1) {
        *cause = "not 1bpp";
        return false;
    }
    if ((int32_t)get_u32(hdr + OFF_WIDTH)  != BMP1BPP_W ||
        (int32_t)get_u32(hdr + OFF_HEIGHT) != BMP1BPP_H) {
        *cause = "not 200x200";
        return false;
    }
    /* 像素数据偏移必须落在头 + 调色板之后一小段内（1bpp 时调色板 8 字节） */
    uint32_t off = get_u32(hdr + OFF_PIXELDATA);
    if (off < BMP1BPP_HEADER || off > BMP1BPP_HEADER + 256) {
        *cause = "pixel data offset out of range";
        return false;
    }
    return true;
}

bool Bmp1Bpp_Validate(FILE *f)
{
    if (!f) return false;

    uint8_t hdr[BMP1BPP_HEADER];
    if (!read_header(f, hdr)) {
        ESP_LOGW(TAG, "BMP header read failed");
        return false;
    }

    const char *cause = NULL;
    if (!check_header(hdr, &cause)) {
        ESP_LOGW(TAG, "BMP rejected: %s", cause);
        return false;
    }
    return true;
}

bool Bmp1Bpp_Read(FILE *f, uint8_t *buf)
{
    if (!f || !buf) return false;

    uint8_t hdr[BMP1BPP_HEADER];
    if (!read_header(f, hdr)) return false;

    const char *cause = NULL;
    if (!check_header(hdr, &cause)) return false;

    const uint32_t off = get_u32(hdr + OFF_PIXELDATA);

    /* BMP 行自底向上；我们按图像顺序 top-down 输出。 */
    uint8_t row[BMP1BPP_ROW_PADDED];
    for (int y = 0; y < BMP1BPP_H; y++) {
        const int src_row = BMP1BPP_H - 1 - y;
        if (fseek(f, (long)(off + (uint32_t)src_row * BMP1BPP_ROW_PADDED), SEEK_SET) != 0)
            return false;
        if (fread(row, 1, BMP1BPP_ROW_BYTES, f) != BMP1BPP_ROW_BYTES)
            return false;
        memcpy(buf + (size_t)y * BMP1BPP_ROW_BYTES, row, BMP1BPP_ROW_BYTES);
    }
    return true;
}

bool Bmp1Bpp_Write(FILE *f, const uint8_t *buf)
{
    if (!f || !buf) return false;

    uint8_t hdr[BMP1BPP_HEADER];
    memset(hdr, 0, sizeof(hdr));

    hdr[0] = 'B';
    hdr[1] = 'M';
    put_u32(hdr + OFF_FILESIZE,
            BMP1BPP_HEADER + BMP1BPP_COLORTABLE +
            (uint32_t)BMP1BPP_ROW_PADDED * BMP1BPP_H);
    put_u32(hdr + OFF_PIXELDATA, BMP1BPP_HEADER + BMP1BPP_COLORTABLE);
    put_u32(hdr + OFF_DIBHDR, 40);                 /* BITMAPINFOHEADER */
    put_u32(hdr + OFF_WIDTH,  BMP1BPP_W);
    put_u32(hdr + OFF_HEIGHT, BMP1BPP_H);          /* 正数 = 自底向上 */
    hdr[OFF_PLANES] = 1;
    hdr[OFF_BPP]    = 1;
    put_u32(hdr + OFF_COMPRESS, 0);                /* BI_RGB */
    put_u32(hdr + OFF_IMAGESIZE, 0);               /* BI_RGB 时可为 0 */
    put_u32(hdr + OFF_XPPM, XPPM_72DPI);
    put_u32(hdr + OFF_YPPM, XPPM_72DPI);
    put_u32(hdr + OFF_COLORS, 2);                  /* 调色板项数 */

    /* 调色板：index0 = 白，index1 = 黑 */
    static const uint8_t color_table[BMP1BPP_COLORTABLE] = {
        0xFF, 0xFF, 0xFF, 0x00,
        0x00, 0x00, 0x00, 0x00,
    };

    if (fwrite(hdr, 1, BMP1BPP_HEADER, f) != BMP1BPP_HEADER) return false;
    if (fwrite(color_table, 1, BMP1BPP_COLORTABLE, f) != BMP1BPP_COLORTABLE) return false;

    static const uint8_t pad[BMP1BPP_ROW_PADDED - BMP1BPP_ROW_BYTES] = {0};

    for (int y = BMP1BPP_H - 1; y >= 0; y--) {
        uint8_t row[BMP1BPP_ROW_BYTES];
        memcpy(row, buf + (size_t)y * BMP1BPP_ROW_BYTES, BMP1BPP_ROW_BYTES);
        if (fwrite(row, 1, BMP1BPP_ROW_BYTES, f) != BMP1BPP_ROW_BYTES) return false;
        if (fwrite(pad, 1, sizeof(pad), f) != sizeof(pad)) return false;
    }
    return true;
}
