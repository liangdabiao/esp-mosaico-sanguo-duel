#!/usr/bin/env python3
"""生成 sanguo_duel 用的中文位图字模子集（sd_cjk_glyph_data.h）。

与 tools/weiqi/gen_cjk_font.py 同配方：Pillow + Noto Sans CJK SC 把 UI 文案与
卡牌文本光栅化成 16x16 二值字模。扫描来源：
  * main/game.c            —— 页面 UI 文案（字符串字面量自动去重收集）；
  * main/sd/sd_cards.c     —— 卡名；
  * main/sd/sd_text.c      —— 卡牌效果描述；
  * main/sd/sd_ai.c 不扫描（无用户可见文案）。

注意：<0x2E80 的符号（★ · … “ ” 等）必须显式列入 EXTRA_RANGES，否则设备上整块消失。
母字体缺失的码点直接报错退出——宁可在生成期失败，不在设备上留空白框。

用法：
    python tools/sanguo/gen_cjk_font.py          # 写出字模表
    python tools/sanguo/gen_cjk_font.py --check  # 只校验是否与源码同步
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "main" / "sd_cjk_glyph_data.h"
SCAN_FILES = [
    ROOT / "main" / "game.c",
    ROOT / "main" / "sd" / "sd_cards.c",
    ROOT / "main" / "sd" / "sd_text.c",
    ROOT / "main" / "sd" / "sd_session.c",
]
FONT_PATH = Path("D:/esp/fontsrc/NotoSansCJKsc-Regular.otf")

CELL = 16
SIZE = 16

EXTRA_RANGES = (
    (0x00B7, 0x00B7),   # ·
    (0x00D7, 0x00D7),   # ×
    (0x2010, 0x206F),   # 通用标点（“ ” … — 等）
    (0x2460, 0x24FF),   # 带圈数字
    (0x25A0, 0x25FF),   # 几何图形（▲ □ ○ ●）
    (0x2600, 0x26FF),   # 杂项符号（★ ☆）
)


def _is_wanted(c: str) -> bool:
    o = ord(c)
    if o >= 0x2E80:
        return True
    if c in "·★☆、，。（）：；？【】":
        return True
    return any(lo <= o <= hi for lo, hi in EXTRA_RANGES)


def collect_chars() -> list[str]:
    chars: set[str] = set()
    for f in SCAN_FILES:
        if not f.is_file():
            continue
        src = f.read_text(encoding="utf-8")
        for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', src):
            for c in lit:
                if _is_wanted(c):
                    chars.add(c)
    return sorted(chars)


def missing_from_font(chars: list[str]) -> list[str]:
    from fontTools.ttLib import TTFont
    with TTFont(str(FONT_PATH), fontNumber=0, lazy=True) as tt:
        cmap = set(tt.getBestCmap().keys())
    return [c for c in chars if ord(c) not in cmap]


def rasterize(font, ch: str) -> list[int]:
    from PIL import Image, ImageDraw
    im = Image.new("L", (CELL, CELL), 0)
    d = ImageDraw.Draw(im)
    bbox = font.getbbox(ch)
    w = bbox[2] - bbox[0]
    h = bbox[3] - bbox[1]
    ox = (CELL - w) // 2 - bbox[0]
    oy = (CELL - h) // 2 - bbox[1]
    d.text((ox, oy), ch, font=font, fill=255)
    px = im.load()
    rows = []
    for y in range(CELL):
        bits = 0
        for x in range(CELL):
            if px[x, y] > 96:
                bits |= 1 << (15 - x)
        rows.append(bits)
    return rows


def generate() -> str:
    if not FONT_PATH.is_file():
        raise SystemExit(f"找不到母字体：{FONT_PATH}")
    chars = collect_chars()
    if not chars:
        raise SystemExit("没有收集到任何字符——检查 SCAN_FILES")
    missing = missing_from_font(chars)
    if missing:
        raise SystemExit(
            "母字体缺少这些码点："
            + " ".join(f"{c}(U+{ord(c):04X})" for c in missing)
        )
    from PIL import ImageFont
    font = ImageFont.truetype(str(FONT_PATH), SIZE)

    glyphs = [(ch, rasterize(font, ch)) for ch in chars]
    lines = [
        "// SPDX-License-Identifier: Apache-2.0",
        "// 由 tools/sanguo/gen_cjk_font.py 自动生成 —— 勿手改。",
        f"// 母字体：Noto Sans CJK SC Regular（SIL OFL 1.1）；{len(chars)} 个字模，{CELL}x{CELL}。",
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        f"#define SD_CJK_CELL {CELL}",
        "",
        "typedef struct { uint16_t cp; uint16_t rows[SD_CJK_CELL]; } sd_cjk_glyph_t;",
        "",
        f"#define SD_CJK_GLYPH_COUNT {len(glyphs)}",
        "static const sd_cjk_glyph_t SD_CJK_GLYPHS[SD_CJK_GLYPH_COUNT] = {",
    ]
    for ch, rows in glyphs:
        cp = ord(ch)
        rows_str = ",".join(f"0x{r:04x}" for r in rows)
        lines.append(f"    {{ 0x{cp:04x}, {{ {rows_str} }} }},  /* {ch} */")
    lines.append("};")
    lines.append("")
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    text = generate()
    if args.check:
        if not OUT.is_file() or OUT.read_text(encoding="utf-8") != text:
            print(f"STALE: {OUT} —— 字符清单已变化，请重跑 gen_cjk_font.py", file=sys.stderr)
            return 1
        print(f"up to date: {OUT}")
        return 0
    OUT.write_text(text, encoding="utf-8", newline="\n")
    count = text.count("    { 0x")
    print(f"wrote {OUT}: {count} glyphs, {count * CELL * 2} bytes bitmap")
    return 0


if __name__ == "__main__":
    sys.exit(main())
