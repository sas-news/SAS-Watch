#!/bin/sh
# tools/lightweight/flash_report.sh — ビルド済み firmware/build から
# app の flash 内訳を読み取り専用で出す (T41/T43 の before/after 用)。
# 前提: firmware/build がビルド済み (sas_watch.map / sas_watch.bin がある)。
# 使い方: tools/lightweight/flash_report.sh [repo-root]
set -eu
ROOT=${1:-$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)}
FW="$ROOT/firmware"
PY="$ROOT/../.toolchain/espressif/python_env/idf5.5_py3.14_env/bin/python"
IDF="$ROOT/../.toolchain/esp-idf"
if [ ! -f "$FW/build/sas_watch.bin" ]; then
  echo "firmware/build/sas_watch.bin がありません (build してから実行)" >&2
  exit 1
fi
bin=$(wc -c < "$FW/build/sas_watch.bin")
part=4194304
free=$((part - bin))
echo "app bin: $bin B / $part B -> free $free B ($((free * 100 / part))%)"
echo "note: size-components は merged .rodata を二重計上する (per-archive 合計は実 image より約 192KB 大きい)。判定はこの bin サイズで行う。"
echo
"$PY" "$IDF/tools/idf_size.py" "$FW/build/sas_watch.map"
