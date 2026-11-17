#!/usr/bin/env python3
"""tools/lightweight/sim_font_diff.py — sim スクショの before/after 差分。

フォント生成物 (.c) を差し替えたとき、数字盤の見た目が変わっていないことを
確認するための補助。PIL が必要 (pip install pillow)。

使い方:
  # before/after を別ディレクトリに撮る
  ./sim/build/snapshot --out /tmp/sim_before   # 変更前ツリー
  ./sim/build/snapshot --out /tmp/sim_after    # 変更後ツリー
  python3 tools/lightweight/sim_font_diff.py /tmp/sim_before /tmp/sim_after

出力: 差分のあったファイルと bbox / 最大差 / 差分配素数を表示し、
      同ディレクトリに sim-diff.txt を書く。差分 0 なら exit 0、あれば 1。
T43 (文字盤フォント 95→14 グリフ) では、この差分を 0 にするため
tools/gen_fonts.py が line_height/base_line とカーニングを当時の値に戻す。
"""
from __future__ import annotations

import sys
import pathlib

try:
    from PIL import Image, ImageChops
except ImportError:  # pragma: no cover
    sys.exit("Pillow が必要です: pip install pillow")


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print(__doc__)
        return 2
    before, after = pathlib.Path(argv[1]), pathlib.Path(argv[2])
    pngs = sorted(before.glob("*.png"))
    if not pngs:
        sys.exit(f"PNG がありません: {before}")
    diffs = []
    for p in pngs:
        q = after / p.name
        if not q.exists():
            diffs.append((p.name, "missing in after"))
            continue
        ib = Image.open(p).convert("RGB")
        ia = Image.open(q).convert("RGB")
        if ib.size != ia.size:
            diffs.append((p.name, f"size {ib.size} -> {ia.size}"))
            continue
        d = ImageChops.difference(ib, ia)
        bbox = d.getbbox()
        if not bbox:
            continue
        extrema = d.getextrema()
        max_diff = max(ch[1] for ch in extrema)
        npx = 0
        for px in d.getdata():
            if px != (0, 0, 0):
                npx += 1
        diffs.append((p.name, f"bbox={bbox} max={max_diff} px={npx}"))

    print(f"compared {len(pngs)} PNG, diffs={len(diffs)}")
    for name, desc in diffs:
        print(f"  DIFF {name}: {desc}")
    report = pathlib.Path(after) / "sim-diff.txt"
    report.write_text(
        f"before: {before}\nafter: {after}\n"
        f"compared={len(pngs)} diffs={len(diffs)}\n"
        + "".join(f"{n}\t{d}\n" for n, d in diffs),
        encoding="utf-8",
    )
    print(f"report -> {report}")
    return 0 if not diffs else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
