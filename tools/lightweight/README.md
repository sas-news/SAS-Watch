# tools/lightweight — 軽量化監査の補助スクリプト

`docs/lightweight.md` (T41/T43) の計測・検証に使ったスクリプト。生成物は
コミットしない (AGENTS.md)。必要なら以下のコマンドで再生成する。

## スクリプト

| ファイル | 用途 |
|---|---|
| `flash_report.sh` | ビルド済み `firmware/build` から app の flash 内訳を読み取り専用で出す。`idf.py size` 相当 + app 空き%。`size-components` の merged `.rodata` 二重計上 (per-archive 合計が実 image より約 190KB 大きい) の注意も表示する |
| `font_subset_estimate.py` | 文字盤フォントのグリフ削減見積り。生成済み `font_fc_*.c` の glyph_dsc/bitmap と map の `.rodata.glyph_bitmap` から、残す範囲のバイト数・削減量を出す |
| `sim_font_diff.py` | sim スクショの before/after 差分 (PIL)。差分 0 なら exit 0。T43 は 75 画面で 0 を確認 |
| なし (手順は docs 参照) | 実機 `b` ベンチは docs/lightweight.md §7.2 |

## 使い方

```bash
# flash 内訳 (app 空き % とメモリ種別)
tools/lightweight/flash_report.sh

# フォント削減見積り
python3 tools/lightweight/font_subset_estimate.py

# sim before/after 差分
cmake -S sim -B sim/build && cmake --build sim/build -j
./sim/build/snapshot --out /tmp/sim_before   # 変更前ツリーで
./sim/build/snapshot --out /tmp/sim_after    # 変更後ツリーで
python3 tools/lightweight/sim_font_diff.py /tmp/sim_before /tmp/sim_after
```

## T43 (文字盤フォント 95→14 グリフ) の検証結果 (2026-11-17)

- 生成: `python3 tools/gen_fonts.py --faces`。`FACE_FONT_RANGE=0x2D-0x3A`
  (14 グリフ)。同じ入力から 2 回生成して sha256 一致 (決定的)。
- コンパイル後 (最終 build): `font_fc_*` 26 個 **862,119 → 123,347 B
  (−738,772 B, −85.7%)**。app bin 2,917,104 → 2,183,984 B、
  app 空き **30.4% → 47.9%**。
- sim: 監査 (safe-area・overlap・glyph) **0 件**、before/after **75 画面差分 0**。
- core: **273/273 pass**。
- 実機確認 (未実施): 書き込み後に Home と各文字盤 (`t` でテーマ巡回、
  設定の文字盤/時計フォント切替) で数字・`:`・`-` が欠けないこと。
  可能なら `b` ベンチの before/after を docs/perf.md へ。
