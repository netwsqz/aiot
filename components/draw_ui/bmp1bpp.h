#ifndef AIOT_BMP1BPP_H
#define AIOT_BMP1BPP_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 200×200 1bpp BMP —— 本仓库里 standby 图和画板作品共用的格式。
 * 两处曾各写一份读写和校验（draw_ui.c 里甚至同文件内复制了一遍），
 * 这里是唯一一份。所有函数都用 FILE*，由调用方决定路径、是否落盘。 */

#define BMP1BPP_W          200
#define BMP1BPP_H          200
#define BMP1BPP_ROW_BYTES  ((BMP1BPP_W + 7) / 8)              /* 25 字节/行 */
#define BMP1BPP_ROW_PADDED ((BMP1BPP_ROW_BYTES + 3) & ~3)     /* 28，4 字节对齐 */
#define BMP1BPP_BUF_SIZE   (BMP1BPP_W * BMP1BPP_H / 8)        /* 5000 字节 */
#define BMP1BPP_HEADER     54
#define BMP1BPP_COLORTABLE 8

/* 位约定：像素数据里 1=黑、0=白（调色板 index0 白 / index1 黑）。
 * 墨水屏缓冲区是反的（1=白），调用方自行取反——本模块不管 EPD 语义。 */

/* 校验 200×200 1bpp 的 BMP。失败时打 ESP_LOGW。读盘或读文件都行。 */
bool Bmp1Bpp_Validate(FILE *f);

/* 读像素数据进 buf（须 >= BMP1BPP_BUF_SIZE 字节），按图像顺序 top-down。
 * 不负责 fopen/fclose。成功返回 true。不打印——调用方自己决定日志。 */
bool Bmp1Bpp_Read(FILE *f, uint8_t *buf);

/* 把 buf（BMP1BPP_BUF_SIZE 字节，top-down，1=黑）写成 200×200 1bpp BMP。
 * 不负责 fopen/fclose。成功返回 true。 */
bool Bmp1Bpp_Write(FILE *f, const uint8_t *buf);

#ifdef __cplusplus
}
#endif

#endif /* BMP1BPP_H */
