#!/usr/bin/env bash
# Muse Gadget SDK (facebookincubator/muse-gadget-sdk) の公式 ESP32 ファームを、
# SAS-Watch と同じ基板 Waveshare ESP32-S3-Touch-AMOLED-2.06 (SDK の s3-206
# プロファイル) 向けにビルドする。
#
# これは SAS-Watch のファームウェアではない。書き込むと SAS-Watch の
# ファームは消える (tools/muse-gadget/flash.sh が事前バックアップを取る)。
# 手順と制約は tools/muse-gadget/README.md と docs/muse-gadget.md を読むこと。
#
# 使い方:
#   tools/muse-gadget/build.sh                    # ビルドして out/ に成果物をコピー
#   MUSE_SDK_TOKEN=mgst_... tools/muse-gadget/build.sh   # SDK トークンを埋め込む
#   tools/muse-gadget/build.sh --clean            # SDK のビルドディレクトリを作り直す
#   tools/muse-gadget/build.sh --no-copy          # ビルドだけ (out/ へコピーしない)
#   tools/muse-gadget/build.sh --help
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)
WORKSPACE=$(cd "$REPO_ROOT/.." && pwd)
OUT_DIR="$SCRIPT_DIR/out"

# SDK / ESP-IDF の場所。ワークスペース直下の .toolchain を既定にし、
# 環境変数で差し替えられるようにする (SDK を別の場所に置いた場合など)。
SDK_DIR=${MUSE_GADGET_SDK_DIR:-"$WORKSPACE/.toolchain/muse-gadget-sdk"}
IDF_EXPORT_FILE=${MUSE_IDF_EXPORT:-"$WORKSPACE/.toolchain/esp-idf-v6.0.3/export.sh"}
IDF_TOOLS_DIR=${MUSE_IDF_TOOLS_PATH:-"$WORKSPACE/.toolchain/espressif-v6"}
COMPONENT_CACHE=${IDF_COMPONENT_CACHE_PATH:-"$WORKSPACE/.toolchain/comp-cache-v6"}

BOARD=s3-206
PROFILE=waveshare-s3-206
BUILD_NAME="build-muse-waveshare-s3-206"
# SDK の README が s3-206 に指定するオーバーレイ (この順に読み込む)。
SDKCONFIG_DEFAULTS="sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-$PROFILE"
# リポジトリのミラーは git 管理外なので、固定コミットをここに書いておく。
SDK_PINNED_COMMIT=ec976f0537db594b0947a2d1a78926575f97a895

clean=0
copy=1

usage() {
    cat <<'EOS'
Muse Gadget SDK の s3-206 ファームウェアをビルドする。

Muse Gadget SDK は Meta (facebookincubator/muse-gadget-sdk, Apache-2.0) の
公式 ESP32 ファームウェアで、s3-206 プロファイルが SAS-Watch と同じ
Waveshare ESP32-S3-Touch-AMOLED-2.06 に対応している。このスクリプトは
ESP-IDF v6.0.3 と SDK のミラーを使ってビルドし、生成物を tools/muse-gadget/out/
にコピーする。

使い方:
  tools/muse-gadget/build.sh [オプション]

オプション:
  -h, --help     このヘルプを表示する
  --clean        SDK の build-muse-waveshare-s3-206/ と managed_components/ を
                 削除してから作り直す (sdkconfig も初期化される)
  --no-copy      ビルドだけ行い、out/ へのコピーをしない

環境変数:
  MUSE_SDK_TOKEN       gadgets.muse.ai で発行した SDK トークン (mgst_...)。
                       設定するとビルドの sdkconfig に埋め込む。未設定なら空で
                       ビルドする (ペアリングできない。後でシリアルから
                       >sdk_token=mgst_... を送るか、この変数を設定して再ビルドする)。
                       **値は表示も保存もしない**。
  MUSE_GADGET_SDK_DIR  SDK の場所 (既定: <workspace>/.toolchain/muse-gadget-sdk)
  MUSE_IDF_EXPORT      ESP-IDF v6.0.3 の export.sh
                       (既定: <workspace>/.toolchain/esp-idf-v6.0.3/export.sh)
  MUSE_IDF_TOOLS_PATH  IDF_TOOLS_PATH (既定: <workspace>/.toolchain/espressif-v6)
  IDF_COMPONENT_CACHE_PATH  コンポーネントのキャッシュ
                       (既定: <workspace>/.toolchain/comp-cache-v6)

出力:
  tools/muse-gadget/out/muse-gadget.bin        アプリ (0x20000 に書く)
  tools/muse-gadget/out/bootloader.bin         (0x0)
  tools/muse-gadget/out/partition-table.bin    (0x10000)
  tools/muse-gadget/out/ota_data_initial.bin   (0x1d000)
  tools/muse-gadget/out/flash_args             flash.sh が使う書き込み引数
  tools/muse-gadget/out/SHA256SUMS / build-info.txt / build.log

書き込みは tools/muse-gadget/flash.sh を使う (実機のバックアップを取ってから書く)。
EOS
}

die() {
    echo "エラー: $*" >&2
    exit 1
}

info() {
    echo "==> $*"
}

for arg in "$@"; do
    case "$arg" in
        -h|--help)
            usage
            exit 0
            ;;
        --clean)
            clean=1
            ;;
        --no-copy)
            copy=0
            ;;
        *)
            echo "不明なオプション: $arg" >&2
            echo "  tools/muse-gadget/build.sh --help で使い方を表示します。" >&2
            exit 2
            ;;
    esac
done

# --- 前提チェック ---------------------------------------------------------
[ -d "$SDK_DIR/esp32" ] || die "Muse Gadget SDK が見つかりません: $SDK_DIR
  git clone https://github.com/facebookincubator/muse-gadget-sdk で取得し、
  置き場所を MUSE_GADGET_SDK_DIR で指定してください (再 clone は不要なら不要)。"
[ -f "$SDK_DIR/esp32/tools/muse/board.sh" ] || die "SDK の内容が想定と違います: $SDK_DIR/esp32"
[ -f "$SDK_DIR/esp32/devices/sdkconfig.muse-$PROFILE" ] || die "s3-206 の sdkconfig オーバーレイがありません: devices/sdkconfig.muse-$PROFILE"

# SDK トークン。値は表示しない。書式は SDK の cmake/validate_config.cmake と同じ
# (mgst_ + base64url 43 文字 = 48 文字)。空でもビルドできる。
token="${MUSE_SDK_TOKEN:-}"
token="${token//$'\r'/}"
token="${token//$'\n'/}"
# 書式は SDK の cmake/validate_config.cmake と同じ: mgst_ + base64url 43 文字
# (最後の 1 文字は 256bit の下位 4bit しか使わないため AEIMQUYcgkosw048 のどれか)。
if [ -n "$token" ] && ! printf '%s' "$token" | grep -Eq '^mgst_[A-Za-z0-9_-]{42}[AEIMQUYcgkosw048]$'; then
    die "MUSE_SDK_TOKEN の形式が想定と違います (mgst_ で始まる 48 文字)。
  gadgets.muse.ai で発行したトークンをそのまま設定してください。値は表示しません。"
fi

# --- ESP-IDF v6.0.3 を有効にする ------------------------------------------
if [ -f "$IDF_EXPORT_FILE" ]; then
    # IDF_TOOLS_PATH は export.sh より先に必要。既存の環境変数は尊重する。
    if [ -z "${IDF_TOOLS_PATH:-}" ] && [ -d "$IDF_TOOLS_DIR" ]; then
        export IDF_TOOLS_PATH="$IDF_TOOLS_DIR"
    fi
    export IDF_COMPONENT_CACHE_PATH="$COMPONENT_CACHE"
    mkdir -p "$IDF_COMPONENT_CACHE_PATH"
    # shellcheck disable=SC1090
    . "$IDF_EXPORT_FILE" >/dev/null 2>&1 || die "ESP-IDF の export.sh に失敗しました: $IDF_EXPORT_FILE"
elif ! command -v idf.py >/dev/null 2>&1; then
    die "ESP-IDF v6.0.3 が見つかりません。
  $IDF_EXPORT_FILE が無く、idf.py も PATH にありません。
  セットアップ手順は docs/muse-gadget.md を参照してください。"
fi

idf_version=$(idf.py --version 2>/dev/null | head -1 || true)
case "$idf_version" in
    *v6.0.3*) ;;
    *)
        if [ "${MUSE_ALLOW_OTHER_IDF:-0}" = "1" ]; then
            echo "警告: ESP-IDF が v6.0.3 ではありません ($idf_version)。公式サポート外です。" >&2
        else
            die "ESP-IDF が v6.0.3 ではありません ($idf_version)。
  SDK は v6.0.3 前提です。どうしても別版で試すときは MUSE_ALLOW_OTHER_IDF=1 を付けてください。"
        fi
        ;;
esac

cd "$SDK_DIR/esp32"

if [ "$clean" = "1" ]; then
    info "--clean: $BUILD_NAME/ と managed_components/ を削除します"
    rm -rf "$BUILD_NAME" managed_components dependencies.lock
fi

mkdir -p "$OUT_DIR"

# --- sdkconfig の生成と設定 ------------------------------------------------
# 既存の build ディレクトリを使い回す。初回だけ公式ヘルパーと同じ引数で configure する。
if [ ! -f "$BUILD_NAME/sdkconfig" ]; then
    info "sdkconfig を生成します ($BUILD_NAME/sdkconfig)"
    # ほかのボードの managed_components が残っていると衝突するので消してから作る。
    rm -rf managed_components dependencies.lock
    idf.py -B "$BUILD_NAME" -DIDF_TARGET=esp32s3 \
        -DSDKCONFIG="$BUILD_NAME/sdkconfig" \
        -DSDKCONFIG_DEFAULTS="$SDKCONFIG_DEFAULTS" \
        reconfigure
fi

# sdkconfig にこのプロジェクトの既定を入れる:
#   日本語字幕 (Unifont) = on  / Noto 版 = off / リアルタイム音声通話 = off
#   SDK トークン = MUSE_SDK_TOKEN があれば埋め込む (無ければ既存値を保持)
info "sdkconfig を更新します (日本語字幕 on / realtime voice off)"
MUSE_SDK_TOKEN="$token" python3 - "$BUILD_NAME/sdkconfig" <<'PY'
import os
import re
import sys

path = sys.argv[1]
with open(path, "r", encoding="utf-8") as f:
    lines = f.read().splitlines()

def set_bool(name, value):
    """CONFIG_<name> を value (True/False) にする。# ... is not set も扱う。"""
    global lines
    new = ("CONFIG_%s=y" % name) if value else ("# CONFIG_%s is not set" % name)
    pat = re.compile(r"^(?:CONFIG_%s=.*|# CONFIG_%s is not set)$" % (re.escape(name), re.escape(name)))
    out, found = [], False
    for line in lines:
        if pat.match(line):
            if not found:
                out.append(new)
                found = True
            continue
        out.append(line)
    if not found:
        out.append(new)
    lines = out

def set_string(name, value):
    global lines
    quoted = '"%s"' % value
    pat = re.compile(r"^CONFIG_%s=.*$" % re.escape(name))
    out, found = [], False
    for line in lines:
        if pat.match(line):
            if not found:
                out.append("CONFIG_%s=%s" % (name, quoted))
                found = True
            continue
        out.append(line)
    if not found:
        out.append("CONFIG_%s=%s" % (name, quoted))
    lines = out

set_bool("MUSE_CJK_FONT", True)        # 日本語・中国語・韓国語の字幕 (Unifont, flash +約1.2MB)
set_bool("MUSE_CJK_FONT_NOTO", False)  # Noto Sans SC 版は使わない
set_bool("MUSE_REALTIME_VOICE", False) # 既知の HTTP 404 問題があるため既定で off

token = os.environ.get("MUSE_SDK_TOKEN", "")
if token:
    set_string("GADGET_SDK_TOKEN", token)

with open(path, "w", encoding="utf-8") as f:
    f.write("\n".join(lines) + "\n")
PY

# トークンの有無だけを表示する (値は絶対に出さない)。
if [ -n "$token" ]; then
    info "SDK トークン: MUSE_SDK_TOKEN の値を sdkconfig に埋め込みました (表示しません)"
elif grep -q '^CONFIG_GADGET_SDK_TOKEN="mgst_' "$BUILD_NAME/sdkconfig"; then
    info "SDK トークン: 既存の sdkconfig の値を使用します (MUSE_SDK_TOKEN 未設定)"
else
    echo "警告: SDK トークンが空です。このビルドはペアリングできません。" >&2
    echo "  取得: https://gadgets.muse.ai/settings/sdk-tokens" >&2
    echo "  方法1: MUSE_SDK_TOKEN=mgst_... tools/muse-gadget/build.sh" >&2
    echo "  方法2: 書き込み後、シリアルで >sdk_token=mgst_... を送る (NVS に保存され再起動)" >&2
fi

# --- ビルド ---------------------------------------------------------------
log="$OUT_DIR/build.log"
mkdir -p "$OUT_DIR"
info "ビルドします (ログ: $log)"
# ESP-IDF の kconfgen は sdkconfig の既定値の差分を INFO で出し、そこに
# CONFIG_GADGET_SDK_TOKEN の値がそのまま載る。ログ・画面にトークンを残さないよう、
# 出力を mgst_[REDACTED] に置き換えてから tee する。
set +e
idf.py -B "$BUILD_NAME" -DIDF_TARGET=esp32s3 -DSDKCONFIG="$BUILD_NAME/sdkconfig" build 2>&1 \
    | sed -E 's/(mgst_)[A-Za-z0-9_-]+/\1[REDACTED]/g' \
    | tee "$log"
rc=${PIPESTATUS[0]}
set -e
if [ "$rc" -ne 0 ]; then
    die "ビルドに失敗しました (exit $rc)。ログ: $log"
fi

# 念のため、ログにトークンらしき文字列が残っていないか確認する (保険)。
# 上の sed が入れた "mgst_[REDACTED]" は除いて検査する。
if grep -Eq 'mgst_([^[]|$)' "$log"; then
    echo "警告: ビルドログにトークンらしき文字列が残っています。共有しないでください: $log" >&2
fi

# --- 成果物のコピーと記録 --------------------------------------------------
if [ "$copy" = "1" ]; then
    info "成果物を $OUT_DIR にコピーします"
    cp "$BUILD_NAME/muse-gadget.bin" "$OUT_DIR/muse-gadget.bin"
    cp "$BUILD_NAME/bootloader/bootloader.bin" "$OUT_DIR/bootloader.bin"
    cp "$BUILD_NAME/partition_table/partition-table.bin" "$OUT_DIR/partition-table.bin"
    cp "$BUILD_NAME/ota_data_initial.bin" "$OUT_DIR/ota_data_initial.bin"

    # build ディレクトリ内の flash_args はビルドディレクトリ相対のパスなので、
    # out/ でそのまま使えるよう、書き込み位置とファイル名を平坦化して作り直す。
    python3 - "$BUILD_NAME/flasher_args.json" "$OUT_DIR/flash_args" <<'PY'
import json
import os
import sys

with open(sys.argv[1], "r", encoding="utf-8") as f:
    args = json.load(f)

lines = [" ".join(args["write_flash_args"])]
for offset, path in sorted(args["flash_files"].items(), key=lambda kv: int(kv[0], 16)):
    lines.append("%s %s" % (offset, os.path.basename(path)))
with open(sys.argv[2], "w", encoding="utf-8") as f:
    f.write("\n".join(lines) + "\n")
PY

    # サイズ・パーティション・設定の記録。トークンの値は書かない。
    app_size=$(stat -c %s "$OUT_DIR/muse-gadget.bin")
    boot_size=$(stat -c %s "$OUT_DIR/bootloader.bin")
    part_size=$(stat -c %s "$OUT_DIR/partition-table.bin")
    ota_size=$(stat -c %s "$OUT_DIR/ota_data_initial.bin")
    sdk_rev=$(git -C "$SDK_DIR" rev-parse HEAD 2>/dev/null || true)
    [ -n "$sdk_rev" ] || sdk_rev="$SDK_PINNED_COMMIT (ミラーは git 管理外のため固定値を記録)"
    sdk_version=$(cat "$SDK_DIR/esp32/version.txt" 2>/dev/null || echo "unknown")
    token_embedded=no
    grep -q '^CONFIG_GADGET_SDK_TOKEN="mgst_' "$BUILD_NAME/sdkconfig" && token_embedded=yes
    # ota_0 のサイズ (partitions_muse.csv)。アプリが収まるかの確認に使う。
    slot_bytes=$(python3 - "$SDK_DIR/esp32/partitions_muse.csv" <<'PY'
import re
import sys

slot = 0
with open(sys.argv[1], "r", encoding="utf-8") as f:
    for line in f:
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        fields = [x.strip() for x in line.split(",")]
        if len(fields) >= 5 and fields[0] == "ota_0":
            m = re.match(r"^(0x[0-9a-fA-F]+|\d+)([KkMm]?)$", fields[4])
            if m:
                n = int(m.group(1), 0)
                unit = m.group(2).lower()
                slot = n * (1024 if unit == "k" else 1024 * 1024 if unit == "m" else 1)
print(slot)
PY
)
    {
        echo "board: Waveshare ESP32-S3-Touch-AMOLED-2.06 (Muse Gadget SDK s3-206)"
        echo "built_at: $(date -Iseconds)"
        echo "idf: $(idf.py --version 2>/dev/null | head -1)"
        echo "idf_path: ${IDF_PATH:-unknown}"
        echo "sdk_dir: $SDK_DIR"
        echo "sdk_rev: $sdk_rev"
        echo "sdk_version: $sdk_version"
        echo "sdkconfig_defaults: $SDKCONFIG_DEFAULTS"
        echo "options: CONFIG_MUSE_CJK_FONT=y CONFIG_MUSE_CJK_FONT_NOTO=n CONFIG_MUSE_REALTIME_VOICE=n"
        echo "sdk_token_embedded: $token_embedded"
        echo "flash_size: 32MB"
        echo "partition_table: partitions_muse.csv (table @ 0x10000, ota_0 @ 0x20000, slot $slot_bytes bytes)"
        echo "app_size: $app_size"
        echo "app_slot_free: $((slot_bytes - app_size))"
        echo "bootloader_size: $boot_size"
        echo "partition_table_size: $part_size"
        echo "ota_data_initial_size: $ota_size"
        echo "flash_offsets: bootloader=0x0 partition-table=0x10000 ota_data_initial=0x1d000 app=0x20000"
    } > "$OUT_DIR/build-info.txt"

    ( cd "$OUT_DIR" && sha256sum muse-gadget.bin bootloader.bin partition-table.bin \
        ota_data_initial.bin flash_args build-info.txt > SHA256SUMS )

    echo
    echo "============================== ビルド完了 =============================="
    echo "アプリ: $OUT_DIR/muse-gadget.bin"
    echo "  サイズ: $app_size バイト (app slot $slot_bytes バイト中、残り $((slot_bytes - app_size)) バイト)"
    echo "  sha256: $(sha256sum "$OUT_DIR/muse-gadget.bin" | cut -d' ' -f1)"
    echo "その他: bootloader.bin / partition-table.bin / ota_data_initial.bin / flash_args"
    echo "記録  : $OUT_DIR/build-info.txt"
    echo "ログ  : $log"
    echo "書き込み: tools/muse-gadget/flash.sh (先に実機のフラッシュをバックアップする)"
    echo "======================================================================"
else
    echo
    echo "ビルド完了 (--no-copy): $SDK_DIR/esp32/$BUILD_NAME/muse-gadget.bin"
fi
