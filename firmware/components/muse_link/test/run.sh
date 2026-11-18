#!/usr/bin/env bash
# T52/T54: muse_link の純 C パーサ/音声変換のホストテスト。ESP-IDF も実機も要らない。
#
#   firmware/components/muse_link/test/run.sh
#
# - test_muse_console.c : シリアル `muse ...` 行のパーサ
# - test_muse_voice.c   : 16kHz PCM -> WAV / base64 の変換
# - test_muse_session.c : セッション状態機械 / 開始前チェック / status JSON
# いずれも ESP_IDF 非依存 (src/muse_console_parse.c, src/muse_voice_note.c)。
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
cc=${CC:-cc}
inc="$here/../include"
cflags=(-std=c99 -Wall -Wextra -Werror -I"$inc" -I"$here/../src" -I"$here/stub")

tmpdir=$(mktemp -d -t muse_link_tests.XXXXXX)
trap 'rm -rf "$tmpdir"' EXIT

echo "== test_muse_console"
"$cc" "${cflags[@]}" \
    "$here/../src/muse_console_parse.c" \
    "$here/test_muse_console.c" \
    -o "$tmpdir/test_muse_console"
"$tmpdir/test_muse_console"

echo "== test_muse_voice"
"$cc" "${cflags[@]}" \
    "$here/../src/muse_voice_note.c" \
    "$here/test_muse_voice.c" \
    -o "$tmpdir/test_muse_voice"
"$tmpdir/test_muse_voice"

echo "== test_muse_session"
"$cc" "${cflags[@]}" \
    "$here/../src/session_logic.c" \
    "$here/test_muse_session.c" \
    -o "$tmpdir/test_muse_session"
"$tmpdir/test_muse_session"

echo "all host tests passed"
