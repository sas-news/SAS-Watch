#!/usr/bin/env bash
# T49 spike: SDK の chat/Noise 層を ESP-IDF 5.5.1 (workspace の .toolchain) でビルドする。
#
#   ./build.sh              # ビルド + サイズ表示
#   ./build.sh size         # ビルドせずサイズだけ
#   ./build.sh fullclean    # 生成物を消す
#
# 既存の SAS-Watch firmware/build や .toolchain/esp-idf-v6.0.3 には触らない。
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
# spike = SAS-Watch/tools/muse-gadget/spike → 4 つ上が workspace
ws=$(cd "$here/../../../.." && pwd)

export IDF_PATH="${IDF_PATH:-$ws/.toolchain/esp-idf}"
export IDF_TOOLS_PATH="${IDF_TOOLS_PATH:-$ws/.toolchain/espressif}"

if [ ! -f "$IDF_PATH/export.sh" ]; then
    echo "ESP-IDF が見つからない: $IDF_PATH" >&2
    exit 1
fi
# shellcheck disable=SC1090
. "$IDF_PATH/export.sh" >/dev/null

version=$(idf.py --version 2>/dev/null || true)
echo "== $version / IDF_PATH=$IDF_PATH"

cd "$here"
case "${1:-build}" in
    size)
        idf.py -B build size
        ;;
    fullclean)
        idf.py -B build fullclean
        ;;
    *)
        mkdir -p build
        idf.py -B build build 2>&1 | tee build/last_build.log
        echo
        echo "== app size"
        grep -E 'binary size' build/last_build.log || true
        ;;
esac
