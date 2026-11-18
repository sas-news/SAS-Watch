#!/usr/bin/env bash
# Muse Gadget SDK の s3-206 ファームウェアを実機 (Waveshare ESP32-S3-Touch-AMOLED-2.06)
# に書き込む。**SAS-Watch のファームは消える**ので、既定で先にフラッシュ全体
# (32MB) を out/backup/ に読み出してから書き込む。戻し方は README を参照。
#
# 先に tools/muse-gadget/build.sh でビルドしておくこと。
#
# 使い方:
#   tools/muse-gadget/flash.sh                 # ポート自動検出・バックアップ・書き込み
#   tools/muse-gadget/flash.sh /dev/ttyACM0    # ポート指定
#   tools/muse-gadget/flash.sh --backup-only   # バックアップだけ
#   tools/muse-gadget/flash.sh --no-backup     # バックアップを取らずに書き込む (非推奨)
#   tools/muse-gadget/flash.sh --help
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)
WORKSPACE=$(cd "$REPO_ROOT/.." && pwd)
OUT_DIR="$SCRIPT_DIR/out"
BACKUP_DIR="$OUT_DIR/backup"

SDK_DIR=${MUSE_GADGET_SDK_DIR:-"$WORKSPACE/.toolchain/muse-gadget-sdk"}
IDF_EXPORT_FILE=${MUSE_IDF_EXPORT:-"$WORKSPACE/.toolchain/esp-idf-v6.0.3/export.sh"}
IDF_TOOLS_DIR=${MUSE_IDF_TOOLS_PATH:-"$WORKSPACE/.toolchain/espressif-v6"}
COMPONENT_CACHE=${IDF_COMPONENT_CACHE_PATH:-"$WORKSPACE/.toolchain/comp-cache-v6"}

BUILD_NAME="build-muse-waveshare-s3-206"
FLASH_BAUD=${MUSE_FLASH_BAUD:-460800}
BACKUP_BAUD=${MUSE_BACKUP_BAUD:-921600}
# 基板は 32MB flash。全体を読む (ファクトリファームの復元用)。
FLASH_SIZE=0x2000000

port=""
backup_file=""
do_backup=1
backup_only=0
do_monitor=0

usage() {
    cat <<'EOS'
Muse Gadget SDK の s3-206 ファームウェアを実機に書き込む。

先に tools/muse-gadget/build.sh を実行しておくこと。書き込むと SAS-Watch の
ファームウェアは消える。既定では先に実機のフラッシュ全体 (32MB) を
tools/muse-gadget/out/backup/ に読み出してから書き込む。

使い方:
  tools/muse-gadget/flash.sh [オプション] [ポート]

オプション:
  -h, --help          このヘルプを表示する
  -p, --port PORT     シリアルポートを指定する (既定: 自動検出。例 /dev/ttyACM0)
  --backup FILE       バックアップの保存先を指定する
  --no-backup         事前バックアップを取らずに書き込む (非推奨)
  --backup-only       バックアップだけして終了する
  --monitor           書き込み後に idf.py monitor を開く (Ctrl-] で終了)

環境変数:
  MUSE_GADGET_SDK_DIR  SDK の場所 (既定: <workspace>/.toolchain/muse-gadget-sdk)
  MUSE_IDF_EXPORT      ESP-IDF v6.0.3 の export.sh
  MUSE_IDF_TOOLS_PATH  IDF_TOOLS_PATH (既定: <workspace>/.toolchain/espressif-v6)
  MUSE_FLASH_BAUD      書き込みのボーレート (既定: 460800)
  MUSE_BACKUP_BAUD     読み出しのボーレート (既定: 921600)

戻し方 (バックアップから):
  python -m esptool --chip esp32s3 -p PORT -b 921600 write-flash 0 <backup.bin>
EOS
}

die() {
    echo "エラー: $*" >&2
    exit 1
}

info() {
    echo "==> $*"
}

while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help)
            usage
            exit 0
            ;;
        -p|--port)
            [ $# -ge 2 ] || die "--port の後にポート名がありません"
            port=$2
            shift 2
            ;;
        --port=*)
            port=${1#--port=}
            shift
            ;;
        --backup)
            [ $# -ge 2 ] || die "--backup の後に保存先がありません"
            backup_file=$2
            shift 2
            ;;
        --backup=*)
            backup_file=${1#--backup=}
            shift
            ;;
        --no-backup)
            do_backup=0
            shift
            ;;
        --backup-only)
            backup_only=1
            shift
            ;;
        --monitor)
            do_monitor=1
            shift
            ;;
        -*)
            echo "不明なオプション: $1" >&2
            echo "  tools/muse-gadget/flash.sh --help で使い方を表示します。" >&2
            exit 2
            ;;
        *)
            [ -z "$port" ] || die "ポートは 1 つだけ指定してください"
            port=$1
            shift
            ;;
    esac
done

# --- ESP-IDF v6.0.3 (esptool) を有効にする --------------------------------
if [ -f "$IDF_EXPORT_FILE" ]; then
    if [ -z "${IDF_TOOLS_PATH:-}" ] && [ -d "$IDF_TOOLS_DIR" ]; then
        export IDF_TOOLS_PATH="$IDF_TOOLS_DIR"
    fi
    export IDF_COMPONENT_CACHE_PATH="$COMPONENT_CACHE"
    # shellcheck disable=SC1090
    . "$IDF_EXPORT_FILE" >/dev/null 2>&1 || die "ESP-IDF の export.sh に失敗しました: $IDF_EXPORT_FILE"
fi
command -v python >/dev/null 2>&1 || die "python が見つかりません (ESP-IDF の export.sh を確認してください)"
python -m esptool version >/dev/null 2>&1 || die "esptool が見つかりません (ESP-IDF v6.0.3 を有効にしてください)"

# --- 書き込む成果物を決める ------------------------------------------------
# build.sh が out/ にコピーしたものを使う。無ければ SDK のビルドディレクトリ。
if [ -f "$OUT_DIR/flash_args" ] && [ -f "$OUT_DIR/muse-gadget.bin" ]; then
    flash_dir="$OUT_DIR"
elif [ -f "$SDK_DIR/esp32/$BUILD_NAME/flash_args" ]; then
    flash_dir="$SDK_DIR/esp32/$BUILD_NAME"
else
    die "書き込むファームウェアがありません。先にビルドしてください:
  tools/muse-gadget/build.sh"
fi

# --- ポートの決定 ---------------------------------------------------------
if [ -z "$port" ]; then
    mapfile -t acm_ports < <(ls -1 /dev/ttyACM* 2>/dev/null | sort || true)
    mapfile -t usb_ports < <(ls -1 /dev/ttyUSB* 2>/dev/null | sort || true)
    candidates=("${acm_ports[@]}" "${usb_ports[@]}")
    # 空配列のときも 1 個と数えないようにする。
    found=()
    for p in "${candidates[@]:-}"; do
        [ -n "$p" ] && [ -e "$p" ] && found+=("$p")
    done
    if [ "${#found[@]}" -eq 1 ]; then
        port=${found[0]}
    elif [ "${#found[@]}" -eq 0 ]; then
        die "書き込み先のポートが見つかりません。
  USB ケーブル (データ通信できるもの) を接続し、ls /dev/ttyACM* で確認してください。
  ポートを明示する場合: tools/muse-gadget/flash.sh --port /dev/ttyACM0"
    else
        die "ポートが複数あります: ${found[*]}
  どれか 1 つを --port で指定してください。"
    fi
fi
[ -e "$port" ] || die "ポートが存在しません: $port"

info "ポート: $port"
info "書き込むファーム: $flash_dir"

# --- 実機のフラッシュをバックアップ ---------------------------------------
mkdir -p "$BACKUP_DIR"
backup_path=""
if [ "$do_backup" = "1" ]; then
    if [ -n "$backup_file" ]; then
        backup_path=$backup_file
        [ ! -e "$backup_path" ] || { [ "$backup_only" = "1" ] || die "バックアップ先が既にあります: $backup_path
  上書きするなら別の名前を指定するか、そのファイルを退避してください。"; }
    else
        # 既定の場所。既にあれば読み直さない (32MB の読み出しは数分かかる)。
        existing=$(ls -1 "$BACKUP_DIR"/waveshare-s3-2.06-factory.bin 2>/dev/null | head -1 || true)
        if [ -n "$existing" ]; then
            backup_path=$existing
            info "既存のバックアップを使います: $backup_path (読み直しません)"
        else
            backup_path="$BACKUP_DIR/waveshare-s3-2.06-factory.bin"
        fi
    fi

    if [ -s "$backup_path" ]; then
        info "バックアップ済み: $backup_path"
    else
        mkdir -p "$(dirname "$backup_path")"
        info "実機のフラッシュ全体 (32MB) を読み出します: $backup_path"
        info "  数分かかります。終わるまで USB を抜かないでください。"
        python -m esptool --chip esp32s3 -p "$port" -b "$BACKUP_BAUD" \
            --before default-reset --after hard-reset \
            read-flash 0 "$FLASH_SIZE" "$backup_path" \
            || die "バックアップの読み出しに失敗しました。書き込みは行いません。
  BOOT を押しながら RESET して再試行するか、ポートを確認してください。"
        actual=$(stat -c %s "$backup_path")
        if [ "$actual" -ne "$((FLASH_SIZE))" ]; then
            die "バックアップのサイズが想定と違います ($actual バイト)。書き込みは行いません:
  $backup_path"
        fi
        # 全体バックアップには Wi-Fi 認証情報などが含まれるので本人だけ読めるようにする。
        chmod 600 "$backup_path" 2>/dev/null || true
        info "バックアップ完了: $backup_path ($actual バイト, 0600)"
        info "  sha256: $(sha256sum "$backup_path" | cut -d' ' -f1)"
    fi
else
    echo "警告: --no-backup のためバックアップを取りません。" >&2
    echo "  書き込むと SAS-Watch のファームは戻せません (再ビルドして書き直すことは可能)。" >&2
fi

if [ "$backup_only" = "1" ]; then
    info "--backup-only: バックアップだけして終了します"
    echo "戻すとき: python -m esptool --chip esp32s3 -p $port -b $BACKUP_BAUD write-flash 0 $backup_path"
    exit 0
fi

# --- 書き込み -------------------------------------------------------------
info "Muse Gadget ファームウェアを書き込みます (SAS-Watch のファームは消えます)"
info "  cmd: python -m esptool --chip esp32s3 -p $port -b $FLASH_BAUD ... write-flash @flash_args"
( cd "$flash_dir" && \
    python -m esptool --chip esp32s3 -p "$port" -b "$FLASH_BAUD" \
        --before default-reset --after hard-reset \
        write-flash @flash_args ) \
    || die "書き込みに失敗しました。
  BOOT を押しながら RESET を押して離し、もう一度実行してください:
  tools/muse-gadget/flash.sh --no-backup --port $port"

echo
echo "============================== 書き込み完了 =============================="
echo "Muse アプリの設定 > デバイス > 開発者モード を ON にして、+ から追加する。"
echo "  MuseGadget-XXXXXX が見つかる。画面の指示か BOOT 押下でペアリングする。"
if [ -n "$backup_path" ]; then
    echo "元に戻す (バックアップ復元):"
    echo "  python -m esptool --chip esp32s3 -p $port -b $BACKUP_BAUD write-flash 0 $backup_path"
fi
echo "ログを見る:"
echo "  . $IDF_EXPORT_FILE && idf.py -p $port monitor   (Ctrl-] で終了)"
echo "========================================================================"

if [ "$do_monitor" = "1" ]; then
    # shellcheck disable=SC1090
    . "$IDF_EXPORT_FILE" >/dev/null 2>&1
    idf.py -p "$port" monitor
fi
