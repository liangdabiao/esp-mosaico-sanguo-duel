// SPDX-License-Identifier: Apache-2.0
// main/sd_cjk_font.h —— 中文位图字模 blit 的对外接口。
//
// 字模表由 tools/weiqi/gen_cjk_font.py 生成到 sd_cjk_glyph_data.h（勿手改），
// 只在 sd_cjk_font.c 内部包含；这里只暴露绘制接口，避免每个编译单元都带一份字表。
#pragma once

#include "mosaico_raylib_fast.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 以左上角 (x,y) 绘制中文串（16px 全角），返回绘制后的下一像素 x。
 * 不在字库的码点（含 ASCII）被跳过——ASCII 请用 DrawText。 */
int sd_cjk_draw(const char *utf8, int x, int y, Color color);

/* 同上，但每个字形按整数倍 scale 放大（scale<=1 等价 sd_cjk_draw）。
 * 每像素画成 scale x scale 的方块：16px 位图做整数倍复制，笔画是实的，
 * 不像插值那样发虚。BRIEF 题面正文用 2 倍。 */
int sd_cjk_draw_scaled(const char *utf8, int x, int y, Color color, int scale);

/* 中文串宽度（像素），不计入被跳过的 ASCII。 */
int sd_cjk_width(const char *utf8);
int sd_cjk_width_scaled(const char *utf8, int scale);

/* 单个全角字的步进（像素，含字间距）。scale=1 时为 18。
 * 版式预算（每行放几个字）用它算，避免调用方自己写死 16+2。 */
int sd_cjk_pitch(int scale);

#ifdef __cplusplus
}
#endif
