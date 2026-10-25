#!/usr/bin/env python3
"""TTF の指定文字を等幅にする (数字の桁揺れを止める)。

等幅でない書体の数字を binfont にすると、分が変わるたびに 2 桁の幅が変わって
文字盤の数字が左右に揺れる。このスクリプトは hmtx の送り幅だけを書き換えて
(グリフは動かさず中央寄せして) 等幅化した TTF を作る。元の書体の見た目は保つ。

usage:
  python3 tools/font_mono.py in.ttf out.ttf [文字列=0123456789:] [--pad 6]

その後 binfont 化する:
  python3 tools/gen_fonts.py --binfont out.ttf --size 96 --bpp 4 \
      --range "0x20,0x25,0x2B-0x2F,0x30-0x3A" --out themes/<id>/digits_96.bin

fonttools が必要 (pip install fonttools)。
"""
import argparse

from fontTools.pens.transformPen import TransformPen
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib import TTFont


def stretch_vertical(path: str, ys: float) -> None:
    f = TTFont(path)
    glyf = f["glyf"]
    glyph_set = f.getGlyphSet()
    for name in f.getGlyphOrder():
        if name not in glyf:
            continue
        pen = TTGlyphPen(glyf)
        glyph_set[name].draw(TransformPen(pen, (1, 0, 0, ys, 0, 0)))
        glyf[name] = pen.glyph()
    for tag, field in (("hhea", "ascent"), ("hhea", "descent"),
                       ("OS/2", "sTypoAscender"), ("OS/2", "sTypoDescender"),
                       ("OS/2", "sTypoLineGap"), ("OS/2", "usWinAscent"),
                       ("OS/2", "usWinDescent")):
        if tag in f:
            setattr(f[tag], field, round(getattr(f[tag], field) * ys))
    for name in f.getGlyphOrder():
        if name in glyf:
            glyf[name].recalcBounds(glyf)
    f.save(path)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("chars", nargs="?", default="0123456789:")
    ap.add_argument("--pad", type=int, default=6,
                    help="共通送り幅 = 最大インク幅 + pad (font units)")
    ap.add_argument("--yscale", type=float, default=1.0,
                    help="グリフを縦に伸ばす倍率 (1.15 = 15%% 背高)")
    a = ap.parse_args()

    f = TTFont(a.src)
    cmap = f.getBestCmap() or {}
    glyf = f["glyf"]
    hmtx = f["hmtx"]

    names = []
    for ch in a.chars:
        g = cmap.get(ord(ch))
        if g is None:
            print(f"skip: {ch!r} は cmap に無い")
            continue
        names.append((ch, g))
    if not names:
        print("対象グリフが無い")
        return 1

    ink = {}
    for ch, g in names:
        gl = glyf[g]
        ink[g] = (gl.xMax - gl.xMin) if gl.numberOfContours else 0
    target = max(ink.values()) + a.pad

    # FreeType は輪郭を lsb の位置に置くので、lsb に「共通幅の中で
    # インクを中央に置く値」を入れれば見た目も中央に揃う。
    for ch, g in names:
        center = round((target - ink[g]) / 2)
        hmtx[g] = (target, center)
        print(f"{ch}: ink {ink[g]:4d} -> adv {target} lsb {center}")

    f.save(a.dst)
    if a.yscale != 1.0:
        stretch_vertical(a.dst, a.yscale)
    print(f"wrote {a.dst}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
