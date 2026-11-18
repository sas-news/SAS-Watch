#!/usr/bin/env bash
# T52: muse コンソール行パーサのホストテスト。ESP-IDF も実機も要らない。
#
#   firmware/components/muse_link/test/run.sh
#
# 純 C の muse_console_parse.c だけを exec する。パス変換・値を扱うのは
# muse_link.cpp 側 (NVS/ログ) なので、ここでは行の解釈だけを検証する。
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
cc=${CC:-cc}
tmp=$(mktemp -t muse_console_test.XXXXXX)
trap 'rm -f "$tmp"' EXIT

"$cc" -std=c99 -Wall -Wextra -Werror \
    -I"$here/../src" \
    "$here/../src/muse_console_parse.c" \
    "$here/test_muse_console.c" \
    -o "$tmp"
"$tmp"
