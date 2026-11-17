#!/usr/bin/env python3
"""tools/lightweight/font_subset_estimate.py — 文字盤フォントのグリフ削減見積り。

背景 (docs/lightweight.md 参照):
  firmware/components/ui/src/fonts/font_fc_*.c は `tools/gen_fonts.py --faces` が
  lv_font_conv で生成しており、FACE_FONT_RANGE = "0x20-0x7E" (ASCII 95 グリフ) を
  そのまま焼いている。しかし faces/ 以下の digits()/digits_thin() 呼び出しは
  「数字・コロン・マイナス」しか描かない (根拠は docs/lightweight.md §2.1)。
  範囲を絞り直したときの flash 削減量を、生成済み C と map から実測見積りする。

使い方:
  python3 tools/lightweight/font_subset_estimate.py
  python3 tools/lightweight/font_subset_estimate.py --map firmware/build/sas_watch.map

出力: フォントごとの glyph_bitmap 実サイズ、残す範囲のバイト数、削減見積り。
  * glyph_bitmap の実サイズは map の `.rodata.glyph_bitmap` から取る (C の
    16 進表記を数えるより確実)。
  * glyph のバイト数は glyph_dsc[].bitmap_index の差分で求める。
  * 削減量 = 落とすグリフの bitmap バイト + glyph_dsc 8B×落とす数
    (kern ペア表も減るが保守側に見て含めない)。
"""
from __future__ import annotations

import argparse
import pathlib
import re

REPO = pathlib.Path(__file__).resolve().parents[2]
FONT_DIR = REPO / "firmware/components/ui/src/fonts"
DEFAULT_MAP = REPO / "firmware/build/sas_watch.map"

# 残す Unicode。face の digits() が描くのは数字・コロン・マイナスだけだが、
# 将来の取りこぼしを避けて space/%/-./0-9/: を残す (docs/lightweight.md §2.1)。
KEEP_RANGES = [(0x20, 0x20), (0x25, 0x25), (0x2D, 0x3A)]  # 16 グリフ
# lv_font_conv の CMAP_FORMAT0_TINY: glyph_id = unicode - range_start + 1
RANGE_START = 0x20
GLYPH_DSC_BYTES = 8  # lv_font_fmt_txt_glyph_dsc_t (bitmap_index 4 + adv_w 2 + box 2*..)


def kept(uni: int) -> bool:
    return any(lo <= uni <= hi for lo, hi in KEEP_RANGES)


def bitmap_sizes_from_map(map_path: pathlib.Path) -> dict[str, int]:
    """map の `.rodata.glyph_bitmap` 行から obj 名→バイト数を拾う。"""
    sizes: dict[str, int] = {}
    sec = None
    for line in map_path.read_text(errors="replace").splitlines():
        m = re.match(r" \.rodata\.(glyph_bitmap|glyph_dsc|cmaps)", line)
        if m:
            sec = m.group(1)
            continue
        if sec is None:
            continue
        m = re.match(
            r"\s+0x[0-9a-f]+\s+0x([0-9a-f]+)\s+.*libui\.a\((font_fc_[^)]+)\)", line
        )
        if m and sec == "glyph_bitmap":
            obj = m.group(2).removesuffix(".c.obj")
            sizes[obj] = int(m.group(1), 16)
        if m is None or "libui.a" not in line:
            sec = None
    return sizes


def parse_font(path: pathlib.Path):
    text = path.read_text()
    idx = [
        int(m.group(1))
        for m in re.finditer(
            r"\{\.bitmap_index = (\d+), \.adv_w = \d+, \.box_w = \d+, \.box_h = \d+",
            text,
        )
    ]
    return idx


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--map", default=str(DEFAULT_MAP))
    ap.add_argument("--keep", default="",
                    help="残す範囲を上書き (例 0x2D-0x3A,0x30-0x39)")
    args = ap.parse_args()
    global KEEP_RANGES
    if args.keep:
        KEEP_RANGES = []
        for part in args.keep.split(","):
            lo, _, hi = part.partition("-")
            KEEP_RANGES.append((int(lo, 0), int(hi or lo, 0)))

    bm = bitmap_sizes_from_map(pathlib.Path(args.map))
    if not bm:
        print(f"map から glyph_bitmap を拾えません: {args.map}")
        return 1

    total_bm = total_keep = total_dsc = 0
    print(f"残す範囲: {args.keep or '0x20,0x25,0x2D-0x3A (16 glyph)'}")
    print(f"{'font':30s}{'bitmap':>9s}{'keep':>9s}{'save':>9s}")
    for path in sorted(FONT_DIR.glob("font_fc_*.c")):
        name = path.stem
        if name not in bm:
            continue
        idx = parse_font(path)
        n = len(idx)  # reserved + 95 glyphs
        bitmap_total = bm[name]
        spans = []
        for i in range(n):
            end = idx[i + 1] if i + 1 < n else bitmap_total
            spans.append(max(0, end - idx[i]))
        keep_b = 0
        keep_n = 0
        for i in range(n):
            if i == 0:
                continue  # reserved
            uni = RANGE_START + i - 1
            if kept(uni):
                keep_b += spans[i]
                keep_n += 1
        drop_n = (n - 1) - keep_n
        save = (bitmap_total - keep_b) + GLYPH_DSC_BYTES * drop_n
        total_bm += bitmap_total
        total_keep += keep_b
        total_dsc += save
        print(f"{name:30s}{bitmap_total:9d}{keep_b:9d}{save:9d}")
    print(f"{'TOTAL':30s}{total_bm:9d}{total_keep:9d}{total_dsc:9d}")
    print(f"\n推定削減: {total_dsc} B = {total_dsc/1024:.1f} KiB "
          f"(現状 bitmap の {(1-total_keep/total_bm)*100:.0f}% )")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
