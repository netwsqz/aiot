/* bmp1bpp 往返自检 —— 一次性主机检查，不进固件。
 * 编译： gcc -I E:/aiot/tools/pc_sim/include -I E:/aiot/components/draw_ui \
 *            E:/aiot/components/draw_ui/bmp1bpp.c <这个文件> -o bmpcheck.exe
 * 跑：   ./bmpcheck.exe
 * 验证：写出的 BMP 能被自己读回，且与写入的像素完全一致。 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "bmp1bpp.h"

static uint8_t src[BMP1BPP_BUF_SIZE];
static uint8_t back[BMP1BPP_BUF_SIZE];

int main(void)
{
    const char *path = "bmp1bpp_selftest.bmp";

    /* 造一个非平凡图案：棋盘 + 一条横线 + 一条竖线，保证每个 bit 都参与。 */
    for (int y = 0; y < BMP1BPP_H; y++) {
        for (int x = 0; x < BMP1BPP_W; x++) {
            int idx = y * BMP1BPP_ROW_BYTES + (x >> 3);
            uint8_t bit = (uint8_t)(0x80 >> (x & 7));
            int on = ((x / 3 + y / 3) % 2) || y == 7 || x == 199;
            if (on) src[idx] |= bit;
            else    src[idx] &= (uint8_t)~bit;
        }
    }

    FILE *f = fopen(path, "wb");
    assert(f);
    assert(Bmp1Bpp_Write(f, src));
    fclose(f);

    /* 文件大小必须是 54 + 8 + 28*200 = 5662 */
    f = fopen(path, "rb");
    assert(f);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    printf("file size = %ld (expected %d)\n", size,
           BMP1BPP_HEADER + BMP1BPP_COLORTABLE + BMP1BPP_ROW_PADDED * BMP1BPP_H);
    assert(size == BMP1BPP_HEADER + BMP1BPP_COLORTABLE + BMP1BPP_ROW_PADDED * BMP1BPP_H);

    rewind(f);   /* 刚 seek 到尾部测大小，读之前必须回到开头 */
    assert(Bmp1Bpp_Validate(f));
    rewind(f);
    assert(Bmp1Bpp_Read(f, back));
    fclose(f);

    int diff = 0;
    for (int i = 0; i < BMP1BPP_BUF_SIZE; i++) {
        if (src[i] != back[i]) {
            if (diff < 5)
                printf("  mismatch at byte %d: wrote 0x%02X read 0x%02X\n",
                       i, src[i], back[i]);
            diff++;
        }
    }
    printf("byte mismatches = %d\n", diff);
    assert(diff == 0);

    /* 位约定：调色板 index0=白 index1=黑，故源里 1 的 bit 在 BMP 里也应是 1 */
    f = fopen(path, "rb");
    assert(f);
    uint8_t hdr[BMP1BPP_HEADER];
    assert(fread(hdr, 1, BMP1BPP_HEADER, f) == BMP1BPP_HEADER);
    fclose(f);
    printf("bpp=%u w=%d h=%d offset=%u\n",
           (unsigned)(hdr[28] | (hdr[29] << 8)),
           hdr[18] | (hdr[19] << 8), hdr[22] | (hdr[23] << 8),
           (unsigned)(hdr[10] | (hdr[11] << 8)));
    assert(hdr[0] == 'B' && hdr[1] == 'M');
    assert((hdr[28] | (hdr[29] << 8)) == 1);
    assert((hdr[18] | (hdr[19] << 8)) == 200);
    assert((hdr[22] | (hdr[23] << 8)) == 200);
    assert((hdr[10] | (hdr[11] << 8)) == 62);

    printf("OK: bmp1bpp round-trip passes\n");
    return 0;
}
