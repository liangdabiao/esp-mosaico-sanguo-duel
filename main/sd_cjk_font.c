// SPDX-License-Identifier: Apache-2.0
// main/sd_cjk_font.c —— 中文位图字模 blit。
//
// raylib-lite 的 DrawText 只覆盖 5x7 ASCII；这里把 gen_cjk_font.py 光栅化好的
// 16x16 二值字模逐像素画到 RGB565 表面。只支持字库内的汉字（与页面文案一一对应）；
// 不在字库的码点被跳过，ASCII 请继续用 DrawText。
#include "sd_cjk_font.h"

#include <stddef.h>

#include "sd_cjk_glyph_data.h"

/* 字间距（像素）。 */
#define SD_CJK_PITCH (SD_CJK_CELL + 2)

static const sd_cjk_glyph_t *find_glyph(uint32_t cp)
{
    for (uint16_t i = 0; i < SD_CJK_GLYPH_COUNT; i++) {
        if (SD_CJK_GLYPHS[i].cp == cp) return &SD_CJK_GLYPHS[i];
    }
    return NULL;
}

/* 解一个 UTF-8 码点；返回字节数，*cp 写出。非法按 1 字节处理。 */
static int utf8_next(const char *s, uint32_t *cp)
{
    const unsigned char *u = (const unsigned char *)s;
    if (u[0] < 0x80) {
        *cp = u[0];
        return 1;
    }
    if ((u[0] & 0xE0) == 0xC0) {
        *cp = ((uint32_t)(u[0] & 0x1F) << 6) | (uint32_t)(u[1] & 0x3F);
        return 2;
    }
    if ((u[0] & 0xF0) == 0xE0) {
        *cp = ((uint32_t)(u[0] & 0x0F) << 12) | ((uint32_t)(u[1] & 0x3F) << 6) |
              (uint32_t)(u[2] & 0x3F);
        return 3;
    }
    *cp = u[0];
    return 1;
}

/* 以左上角 (x,y) 绘制中文串，返回绘制后的下一像素 x。 */
int sd_cjk_draw_scaled(const char *utf8, int x, int y, Color color, int scale)
{
    if (!utf8) return x;
    if (scale < 1) scale = 1;
    const char *s = utf8;
    int pen = x;
    while (*s) {
        uint32_t cp = 0;
        const int step = utf8_next(s, &cp);
        s += step;
        const sd_cjk_glyph_t *g = find_glyph(cp);
        if (!g) continue; // 不在字库（含 ASCII）：跳过，交给 DrawText。
        for (int r = 0; r < SD_CJK_CELL; r++) {
            const uint16_t row = g->rows[r];
            if (!row) continue;
            for (int c = 0; c < SD_CJK_CELL; c++) {
                if (row & (1 << (15 - c))) {
                    /* 整数倍放大 = 每个字模像素画成 scale x scale 的方块。 */
                    DrawRectangle(pen + c * scale, y + r * scale, scale, scale, color);
                }
            }
        }
        pen += SD_CJK_PITCH * scale;
    }
    return pen;
}

int sd_cjk_draw(const char *utf8, int x, int y, Color color)
{
    return sd_cjk_draw_scaled(utf8, x, y, color, 1);
}

int sd_cjk_pitch(int scale)
{
    if (scale < 1) scale = 1;
    return SD_CJK_PITCH * scale;
}

/* 量宽：每字一个 pitch，ASCII 不计入（由调用方另行用 DrawText 排）。 */
int sd_cjk_width_scaled(const char *utf8, int scale)
{
    if (!utf8) return 0;
    if (scale < 1) scale = 1;
    int n = 0;
    const char *s = utf8;
    while (*s) {
        uint32_t cp = 0;
        const int step = utf8_next(s, &cp);
        s += step;
        if (find_glyph(cp)) n++;
    }
    return n * SD_CJK_PITCH * scale;
}

int sd_cjk_width(const char *utf8)
{
    return sd_cjk_width_scaled(utf8, 1);
}
