#!/usr/bin/env bash
# M0 → M1 の実機作業を「迷わず」進めるランブック。
#
#   M0: 公式 Muse Gadget ファームを書き込み、Muse アプリでペアリングする
#   M1: 公式ファームの NVS から host/vm/token を取り出し、SAS-Watch へ投入する
#
# 既定は **dry-run** (何も実行せず、コマンドとチェックリストを表示するだけ)。
# 実際のバックアップ・書き込み・NVS 読み出し・シリアル投入は --execute を付ける。
# Muse アプリでのペアリングだけはユーザー操作なので、このスクリプトは待機と
# チェックリストを表示する。
#
# 使い方:
#   tools/muse-gadget/m0_m1.sh                       # 状態 + チェックリスト (dry-run)
#   tools/muse-gadget/m0_m1.sh --status              # 状態だけ
#   tools/muse-gadget/m0_m1.sh --backup --execute    # M0 前の 32MB バックアップ
#   tools/muse-gadget/m0_m1.sh --flash-muse --execute   # 公式ファーム書き込み
#   tools/muse-gadget/m0_m1.sh --dump-nvs --execute  # ペアリング後に NVS を読む
#   tools/muse-gadget/m0_m1.sh --export --execute    # 資格情報を 0600 ファイルへ
#   tools/muse-gadget/m0_m1.sh --restore-saswatch --execute  # SAS-Watch に戻す
#   tools/muse-gadget/m0_m1.sh --feed-serial --execute       # シリアル M 行モードで投入
#
# 秘密: token/host/vm の値は表示・ログ出力しない。`--export` が作るファイルは
# 0600。`--feed-serial` は値を画面に出さず、長さだけ確認する。
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)
WORKSPACE=$(cd "$REPO_ROOT/.." && pwd)
OUT_DIR="$SCRIPT_DIR/out"
BACKUP_FILE="$OUT_DIR/backup/waveshare-s3-2.06-factory.bin"
NVS_DIR="$OUT_DIR/nvs"
CRED_DIR="$OUT_DIR/credentials"
CRED_JSON="$CRED_DIR/muse-credentials.json"
CRED_SHELL="$CRED_DIR/muse-set.txt"
EXPORT_PY="$SCRIPT_DIR/export_credentials.py"
FLASH_SH="$SCRIPT_DIR/flash.sh"

# 公式ファーム (SDK partitions_muse.csv) の NVS。docs/muse-m1.md を参照。
MUSE_NVS_OFFSET=${MUSE_NVS_OFFSET:-0x11000}
MUSE_NVS_SIZE=${MUSE_NVS_SIZE:-0xC000}
NVS_FULL_SIZE=$((MUSE_NVS_SIZE))
FLASH_IMAGE_SIZE=$((0x2000000))  # 32MB

IDF_EXPORT_FILE=${MUSE_IDF_EXPORT:-"$WORKSPACE/.toolchain/esp-idf-v6.0.3/export.sh"}
IDF_TOOLS_DIR=${MUSE_IDF_TOOLS_PATH:-"$WORKSPACE/.toolchain/espressif-v6"}

dry_run=1
port=""
serial_baud=${MUSE_SERIAL_BAUD:-115200}
from_file=""
force=0
declare -a actions=()

usage() {
    cat <<'EOS'
M0 (公式 Muse ファーム + ペアリング) → M1 (資格情報を SAS-Watch へ) のランブック。

既定は dry-run です。実行するには --execute を付けます。

使い方:
  tools/muse-gadget/m0_m1.sh [オプション] [アクション...]

アクション (複数指定可):
  --status             今の状態 (バックアップ・NVS ダンプ・資格情報ファイル・ポート) を表示
  --backup             M0 の前に SAS-Watch の 32MB 全量バックアップを取る (flash.sh --backup-only)
  --flash-muse         公式 Muse ファームを書き込む (flash.sh。バックアップ込み)
  --dump-nvs           実機 (Muse ファーム) から NVS 0x11000 / 0xC000 を読む
  --check              最新の NVS ダンプを検査する (キーの有無と長さだけ。値は出さない)
  --export             NVS ダンプから資格情報を JSON + コマンド列に書き出す (0600)
  --restore-saswatch   M0 前のバックアップを書き戻して SAS-Watch に戻す
  --feed-serial        SAS-Watch のシリアル 'M' 行モードに 'muse set ...' を流し込む
  --all                backup → flash-muse まで (M0 の自動部分)

オプション:
  -h, --help           このヘルプ
  --dry-run            何も実行しない (既定)。コマンドとチェックリストを表示
  --execute            実際に実行する (--yes も同じ)
  -p, --port PORT      シリアルポート (既定: /dev/ttyACM* などから自動検出)
  --from FILE          --check/--export の入力 (フルフラッシュ or NVS 単体。既定: 最新の NVS ダンプ)
  --force              --export の出力を上書きする
  --baud BAUD          --feed-serial のボーレート (既定: 115200)
  --nvs-offset HEX     NVS オフセット (既定: 0x11000。フルフラッシュ入力時)
  --nvs-size HEX       NVS サイズ (既定: 0xC000)

環境変数:
  MUSE_IDF_EXPORT      ESP-IDF の export.sh (esptool 用。既定: <workspace>/.toolchain/esp-idf-v6.0.3)
  MUSE_IDF_TOOLS_PATH  IDF_TOOLS_PATH
  MUSE_SERIAL_BAUD     シリアル投入のボーレート

流れ (M0 → M1):
  1. --backup        SAS-Watch の状態を 32MB バックアップ (Muse を書く前に必ず)
  2. --flash-muse    公式ファーム書き込み
  3. (ユーザー)      Muse アプリ: 開発者モード ON → Add Device → BOOT で確認 → Wi-Fi
  4. --dump-nvs      ペアリング後の NVS を実機から読む (Muse ファーム起動中に)
  5. --export        host/vm/token を 0600 ファイルへ (画面に値は出ない)
  6. --restore-saswatch  SAS-Watch のバックアップを書き戻す
  7. (ユーザー)      SAS-Watch を CONFIG_WATCH_ENABLE_MUSE_LINK=y でビルドして書き込み
  8. --feed-serial   'M' 行モードで muse set host/vm/token を投入し、token_len を確認
  9. (ユーザー)      シリアルで muse status → M1 の GO/NO-GO (docs/muse-m1.md)
EOS
}

die() {
    echo "エラー: $*" >&2
    exit 1
}

info() {
    echo "==> $*"
}

warn() {
    echo "警告: $*" >&2
}

# dry-run なら表示だけ、--execute なら実行する。
run_cmd() {
    if [ "$dry_run" = "1" ]; then
        printf '[dry-run]'
        printf ' %q' "$@"
        printf '\n'
        return 0
    fi
    printf '+'
    printf ' %q' "$@"
    printf '\n'
    "$@"
}

have_cmd() {
    command -v "$1" >/dev/null 2>&1
}

# ---- 共通ユーティリティ ---------------------------------------------------

ensure_esptool() {
    if [ -f "$IDF_EXPORT_FILE" ]; then
        if [ -z "${IDF_TOOLS_PATH:-}" ] && [ -d "$IDF_TOOLS_DIR" ]; then
            export IDF_TOOLS_PATH="$IDF_TOOLS_DIR"
        fi
        # shellcheck disable=SC1090
        . "$IDF_EXPORT_FILE" >/dev/null 2>&1 || die "ESP-IDF の export.sh に失敗しました: $IDF_EXPORT_FILE"
    fi
    have_cmd python || die "python が見つかりません (ESP-IDF の export.sh を確認してください)"
    python -m esptool version >/dev/null 2>&1 \
        || die "esptool が見つかりません。ESP-IDF v6.0.3 を有効にしてください (docs/muse-gadget.md)"
}

# シリアル (pyserial) が使える python を探す。IDF python env を優先する。
resolve_idf_python() {
    local candidates=()
    [ -n "${IDF_PYTHON:-}" ] && candidates+=("$IDF_PYTHON")
    shopt -s nullglob
    local candidate
    for candidate in "$WORKSPACE"/.toolchain/espressif*/python_env/*/bin/python; do
        [ -x "$candidate" ] && candidates+=("$candidate")
    done
    shopt -u nullglob
    candidates+=("python3")
    for candidate in "${candidates[@]}"; do
        if "$candidate" -c 'import serial' >/dev/null 2>&1; then
            echo "$candidate"
            return 0
        fi
    done
    return 1
}

detect_port() {
    if [ -n "$port" ]; then
        [ -e "$port" ] || die "ポートが存在しません: $port"
        return 0
    fi
    local found=()
    local candidate
    shopt -s nullglob
    for candidate in /dev/ttyACM* /dev/ttyUSB*; do
        [ -e "$candidate" ] && found+=("$candidate")
    done
    shopt -u nullglob
    if [ "${#found[@]}" -eq 1 ]; then
        port=${found[0]}
    elif [ "${#found[@]}" -eq 0 ]; then
        die "書き込み先のポートが見つかりません。USB ケーブルを接続し、
  ls /dev/ttyACM* で確認してください。ポートを明示するなら --port /dev/ttyACM0"
    else
        die "ポートが複数あります: ${found[*]}
  どれか 1 つを --port で指定してください。"
    fi
}

# 最新の NVS ダンプ (無ければ空)
latest_nvs_dump() {
    local files=()
    local candidate
    shopt -s nullglob
    for candidate in "$NVS_DIR"/*.bin; do
        files+=("$candidate")
    done
    shopt -u nullglob
    if [ "${#files[@]}" -eq 0 ]; then
        echo ""
        return 0
    fi
    # mtime の新しい順
    ls -1t "${files[@]}" | head -1
}

nvs_input() {
    if [ -n "$from_file" ]; then
        [ -f "$from_file" ] || die "入力ファイルがありません: $from_file"
        echo "$from_file"
        return 0
    fi
    local latest
    latest=$(latest_nvs_dump)
    [ -n "$latest" ] || return 1
    echo "$latest"
}

# 入力の種類 (フルフラッシュか NVS 単体か) をサイズで判定する。
# - NVS サイズ (既定 48KB) と同じ → NVS 単体
# - NVS の offset+size より大きい → フルフラッシュ
# - それ以外の小さいページ整合ファイル → NVS 単体とみなす
export_flags_for() {
    local input=$1
    local size
    size=$(stat -c %s "$input")
    if [ "$size" -eq "$NVS_FULL_SIZE" ]; then
        echo "--nvs"
    elif [ "$size" -ge "$((MUSE_NVS_OFFSET + NVS_FULL_SIZE))" ]; then
        echo "--flash"
    else
        echo "--nvs"
    fi
}

# ---- アクション -----------------------------------------------------------

print_checklist() {
    cat <<'EOS'

M0 → M1 チェックリスト (実機作業)
  M0: 公式ファームを書き込んでペアリング
    [ ] tools/muse-gadget/build.sh                  # 公式ファームをビルド (未ビルドなら)
    [ ] m0_m1.sh --backup --execute                 # SAS-Watch の 32MB バックアップ
    [ ] m0_m1.sh --flash-muse --execute             # 公式ファームを書き込み
    [ ] Muse アプリ > 設定 > デバイス > 開発者モード ON
    [ ] + から MuseGadget-XXXXXX を追加し、BOOT 押下で確認
    [ ] Wi-Fi (2.4GHz) を設定し、音声ノート + 日本語字幕を確認
    [ ] ペアリング後、5 秒ほど待つ
  M1: 資格情報を取り出して SAS-Watch へ
    [ ] m0_m1.sh --dump-nvs --execute               # Muse ファーム起動中に実機から NVS を読む
    [ ] m0_m1.sh --check                            # キーと長さだけ確認 (値は出ない)
    [ ] m0_m1.sh --export --execute                 # out/credentials/ に 0600 で保存
    [ ] m0_m1.sh --restore-saswatch --execute       # SAS-Watch に戻す
    [ ] SAS-Watch を CONFIG_WATCH_ENABLE_MUSE_LINK=y でビルドして書き込み
    [ ] m0_m1.sh --feed-serial --execute            # シリアル 'M' 行モードで投入
    [ ] シリアルで muse status → token_len / ready を確認 (値は出ない)
    [ ] docs/muse-m1.md の GO/NO-GO 判定

注意:
  - flash.sh が取る 32MB バックアップは「M0 を書く前の SAS-Watch の状態」です。
    **Muse の資格情報は入っていません。** 資格情報はペアリング後に --dump-nvs で
    実機から読みます。
  - token は秘密。画面・ログ・チャット・コミットに出さない。
EOS
}

action_status() {
    info "状態"
    if [ -f "$OUT_DIR/muse-gadget.bin" ]; then
        echo "  公式ファーム: ビルド済み ($OUT_DIR/muse-gadget.bin, $(stat -c %s "$OUT_DIR/muse-gadget.bin") バイト)"
    else
        echo "  公式ファーム: 未ビルド (tools/muse-gadget/build.sh)"
    fi
    if [ -f "$BACKUP_FILE" ]; then
        echo "  M0 前バックアップ: あり ($BACKUP_FILE, $(stat -c %s "$BACKUP_FILE") バイト)"
    else
        echo "  M0 前バックアップ: なし (m0_m1.sh --backup --execute)"
    fi
    local latest
    latest=$(latest_nvs_dump)
    if [ -n "$latest" ]; then
        echo "  NVS ダンプ: あり ($latest, $(stat -c %s "$latest") バイト)"
    else
        echo "  NVS ダンプ: なし (ペアリング後に --dump-nvs --execute)"
    fi
    if [ -f "$CRED_JSON" ] && [ -f "$CRED_SHELL" ]; then
        echo "  資格情報ファイル: あり ($CRED_DIR/、0600)"
    else
        echo "  資格情報ファイル: なし (--export --execute)"
    fi
    local muselink="不明"
    if [ -f "$REPO_ROOT/firmware/sdkconfig" ]; then
        if grep -q '^CONFIG_WATCH_ENABLE_MUSE_LINK=y' "$REPO_ROOT/firmware/sdkconfig"; then
            muselink="有効 (CONFIG_WATCH_ENABLE_MUSE_LINK=y)"
        else
            muselink="無効 (firmware/sdkconfig で未設定)"
        fi
    elif [ -d "$REPO_ROOT/firmware" ]; then
        muselink="未ビルド (firmware/ に sdkconfig なし)"
    fi
    echo "  SAS-Watch muse_link: $muselink"
    local ports=()
    shopt -s nullglob
    for candidate in /dev/ttyACM* /dev/ttyUSB*; do
        [ -e "$candidate" ] && ports+=("$candidate")
    done
    shopt -u nullglob
    if [ "${#ports[@]}" -gt 0 ]; then
        echo "  USB シリアル: ${ports[*]}"
    else
        echo "  USB シリアル: なし"
    fi
    echo "  実行モード: $([ "$dry_run" = "1" ] && echo 'dry-run (--execute で実行)' || echo 'execute')"
}

action_backup() {
    info "M0 前の SAS-Watch 全量バックアップ"
    local args=("$FLASH_SH" --backup-only)
    [ -n "$port" ] && args+=(--port "$port")
    if [ "$dry_run" = "1" ]; then
        printf '[dry-run]'
        printf ' %q' "${args[@]}"
        printf '\n'
        echo "  バックアップ先 (予定): $BACKUP_FILE"
        return 0
    fi
    "${args[@]}"
    [ -f "$BACKUP_FILE" ] || die "バックアップが作られませんでした: $BACKUP_FILE"
    chmod 600 "$BACKUP_FILE" 2>/dev/null || true
    info "バックアップ: $BACKUP_FILE ($(stat -c %s "$BACKUP_FILE") バイト)"
    echo "  次の手順: m0_m1.sh --flash-muse --execute"
}

action_flash_muse() {
    info "公式 Muse ファームを書き込む (SAS-Watch のファームは消えます)"
    if [ ! -f "$OUT_DIR/muse-gadget.bin" ]; then
        die "公式ファームがありません。先に tools/muse-gadget/build.sh を実行してください"
    fi
    if [ ! -f "$BACKUP_FILE" ]; then
        warn "M0 前のバックアップがありません。--backup --execute を先に実行してください"
        [ "$dry_run" = "1" ] || die "安全のため中止します (バックアップなしでは書き込みません)"
    fi
    local args=("$FLASH_SH")
    [ -n "$port" ] && args+=(--port "$port")
    if [ "$dry_run" = "1" ]; then
        printf '[dry-run]'
        printf ' %q' "${args[@]}"
        printf '\n'
        echo "  (flash.sh が事前バックアップを確認/取得してから書き込みます)"
        return 0
    fi
    "${args[@]}"
    echo
    echo "次の手順 (ユーザー操作):"
    echo "  1. Muse アプリ > 設定 > デバイス > 開発者モード ON"
    echo "  2. + から MuseGadget-XXXXXX を追加し、BOOT 押下で確認"
    echo "  3. Wi-Fi (2.4GHz) を設定し、音声ノート + 日本語字幕を確認"
    echo "  4. 5 秒待ってから: m0_m1.sh --dump-nvs --execute"
}

action_dump_nvs() {
    info "実機 (Muse ファーム) から NVS を読み出す"
    local timestamp
    timestamp=$(date +%Y%m%d-%H%M%S)
    local out="$NVS_DIR/muse-nvs-$timestamp.bin"
    if [ "$dry_run" = "1" ]; then
        local shown_port=${port:-/dev/ttyACM0}
        echo "[dry-run] python -m esptool --chip esp32s3 -p $shown_port -b 460800 \\"
        echo "    --before default-reset --after hard-reset read-flash $MUSE_NVS_OFFSET $MUSE_NVS_SIZE $out"
        echo "  出力先 (予定): $out (0600)"
        echo "  注意: Muse ファームが起動している状態で実行してください (公式ファームの NVS を読みます)"
        return 0
    fi
    detect_port
    ensure_esptool
    local args=(python -m esptool --chip esp32s3 -p "$port" -b 460800 \
        --before default-reset --after hard-reset \
        read-flash "$MUSE_NVS_OFFSET" "$MUSE_NVS_SIZE" "$out")
    mkdir -p "$NVS_DIR"
    chmod 700 "$NVS_DIR" 2>/dev/null || true
    "${args[@]}"
    chmod 600 "$out" 2>/dev/null || true
    local size
    size=$(stat -c %s "$out")
    if [ "$size" -ne "$NVS_FULL_SIZE" ]; then
        die "NVS ダンプのサイズが想定と違います ($size バイト)。ポートとファームを確認してください"
    fi
    info "NVS ダンプ: $out ($size バイト, 0600)"
    echo "  次の手順: m0_m1.sh --check (キー確認) → m0_m1.sh --export --execute"
}

# --check / --export の入力 (CRED_INPUT) と種類 (CRED_FLAG) を決める。
CRED_INPUT=""
CRED_FLAG=""
resolve_cred_input() {
    local input
    input=$(nvs_input) || die "NVS ダンプがありません。先に --dump-nvs --execute を実行してください
  (フルフラッシュから読む場合は --from FILE で指定できます)"
    [ -f "$input" ] || die "入力ファイルがありません: $input"
    CRED_INPUT=$input
    CRED_FLAG=$(export_flags_for "$input")
}

# --check は読み取りだけで安全なので、dry-run でも実行して結果を見せる。
action_check() {
    resolve_cred_input
    info "NVS を検査する (キーの有無と長さだけ。値は表示しません)"
    local args=(python3 "$EXPORT_PY" "$CRED_FLAG" "$CRED_INPUT" --check)
    if [ "$CRED_FLAG" = "--flash" ]; then
        args+=(--nvs-offset "$MUSE_NVS_OFFSET" --nvs-size "$MUSE_NVS_SIZE")
    fi
    if "${args[@]}"; then
        echo "  (キーが揃っています。次は --export --execute)"
    else
        echo "  NG: この NVS では M1 は NO-GO です。docs/muse-m1.md を参照" >&2
        return 1
    fi
}

action_export() {
    resolve_cred_input
    info "資格情報を書き出す (値は画面に出しません)"
    local args=(python3 "$EXPORT_PY" "$CRED_FLAG" "$CRED_INPUT" \
        --out "$CRED_JSON" --shell "$CRED_SHELL")
    if [ "$CRED_FLAG" = "--flash" ]; then
        args+=(--nvs-offset "$MUSE_NVS_OFFSET" --nvs-size "$MUSE_NVS_SIZE")
    fi
    [ "$force" = "1" ] && args+=(--force)

    if [ "$dry_run" = "1" ]; then
        # dry-run でも検査だけは実行して、キーが揃っているかを表示する (値は出ない)。
        local check_args=(python3 "$EXPORT_PY" "$CRED_FLAG" "$CRED_INPUT" --check)
        if [ "$CRED_FLAG" = "--flash" ]; then
            check_args+=(--nvs-offset "$MUSE_NVS_OFFSET" --nvs-size "$MUSE_NVS_SIZE")
        fi
        "${check_args[@]}" || true
        printf '[dry-run]'
        printf ' %q' "${args[@]}"
        printf '\n'
        echo "  出力先 (予定): $CRED_JSON / $CRED_SHELL (0600)"
        return 0
    fi

    mkdir -p "$CRED_DIR"
    chmod 700 "$CRED_DIR" 2>/dev/null || true
    "${args[@]}"
    info "保存: $CRED_JSON / $CRED_SHELL (0600)"
    echo "  次の手順: m0_m1.sh --restore-saswatch --execute (SAS-Watch に戻す)"
    echo "  注意: この 2 ファイルは秘密。共有・コミットしない。使い終わったら削除する。"
}

action_restore() {
    info "M0 前のバックアップを書き戻して SAS-Watch に戻す"
    [ -f "$BACKUP_FILE" ] || die "バックアップがありません: $BACKUP_FILE"
    local size
    size=$(stat -c %s "$BACKUP_FILE")
    if [ "$size" -ne "$FLASH_IMAGE_SIZE" ]; then
        die "バックアップのサイズが想定と違います ($size バイト, 期待 $FLASH_IMAGE_SIZE)"
    fi
    if [ "$dry_run" = "1" ]; then
        local shown_port=${port:-/dev/ttyACM0}
        echo "[dry-run] python -m esptool --chip esp32s3 -p $shown_port -b 460800 \\"
        echo "    --before default-reset --after hard-reset write-flash 0 $BACKUP_FILE"
        echo "  SAS-Watch の状態に戻します (Muse のペアリング情報は公式ファーム側に残ります)"
        return 0
    fi
    detect_port
    ensure_esptool
    local args=(python -m esptool --chip esp32s3 -p "$port" -b 460800 \
        --before default-reset --after hard-reset write-flash 0 "$BACKUP_FILE")
    warn "32MB を書き戻します。数分かかります。USB を抜かないでください"
    "${args[@]}"
    info "書き戻し完了"
    echo "  次の手順: SAS-Watch を CONFIG_WATCH_ENABLE_MUSE_LINK=y でビルドして書き込み"
    echo "  例: cd firmware && . ~/esp/esp-idf/export.sh && idf.py build && idf.py -p <PORT> flash"
    echo "  その後: m0_m1.sh --feed-serial --execute"
}

action_feed_serial() {
    info "SAS-Watch のシリアル 'M' 行モードで資格情報を投入する"
    [ -f "$CRED_SHELL" ] || die "資格情報ファイルがありません: $CRED_SHELL
  先に --export --execute を実行してください"
    [ -f "$CRED_JSON" ] || die "資格情報 JSON がありません: $CRED_JSON"
    if [ "$dry_run" = "1" ]; then
        echo "[dry-run] <pyserial 入り python> <シリアル投入スクリプト> --port ${port:-/dev/ttyACM0} --baud $serial_baud"
        echo "  送信内容: 'M' で行モードに入り、$CRED_SHELL の $(grep -c '^muse set ' "$CRED_SHELL" || true) 行を送る"
        echo "  値は表示しません。token_len だけ確認します。"
        echo "  注意: これは SAS-Watch ファーム (CONFIG_WATCH_ENABLE_MUSE_LINK=y) が起動しているときだけ有効"
        return 0
    fi
    detect_port
    local py
    py=$(resolve_idf_python) || die "pyserial が使える python が見つかりません。
  ESP-IDF の python env (.toolchain/espressif*/python_env/*/bin/python) を確認してください"
    # 秘密の値は Python 側でファイルから読み、シェルの引数・ログには出さない。
    "$py" - "$port" "$serial_baud" "$CRED_SHELL" "$CRED_JSON" <<'PY'
import json
import re
import sys
import time

import serial

port = sys.argv[1]
baud = int(sys.argv[2])
shell_path = sys.argv[3]
json_path = sys.argv[4]

with open(shell_path, "r", encoding="utf-8") as handle:
    lines = [line.rstrip("\n") for line in handle if line.strip()]
with open(json_path, "r", encoding="utf-8") as handle:
    creds = json.load(handle)
expected_token_len = len(creds.get("token", ""))
expected_set_lines = [line for line in lines if line.startswith("muse set ")]

buffer = b""
seen = 0


def wait_after(needle, timeout):
    """バッファの seen 以降に needle が現れるまで待つ (古いログを数えない)。"""
    global buffer, seen
    needle_bytes = needle.encode()
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        index = buffer.find(needle_bytes, seen)
        if index >= 0:
            seen = index + len(needle_bytes)
            return True
        chunk = ser.read(4096)
        if chunk:
            buffer += chunk
        else:
            time.sleep(0.05)
    return False


def drain(seconds):
    """入力バッファを読み捨て、seen を現在位置に進める。"""
    global seen
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        chunk = ser.read(4096)
        if not chunk:
            time.sleep(0.05)
    seen = len(buffer)


def enter_line_mode():
    """'M' で行モードに入る。行モードは 1 行で終わるので、行ごとに呼ぶ。

    SAFETY: 行モードでない状態で改行は無視されるだけ。行モード中なら閉じる。
    こうしてから 'M' を送るので、前の試行の 'M' が行バッファに残らない。
    """
    for _attempt in range(4):
        ser.write(b"\n")
        ser.flush()
        time.sleep(0.2)
        drain(0.2)  # 古いログ (前の M の "line mode" など) を数えない
        ser.write(b"M")
        ser.flush()
        if wait_after("muse line mode", 3.0):
            return True
    return False


def send_line(line):
    """M → 1 行 → ファームの受理ログ、を 1 コマンドずつ繰り返す。"""
    if not enter_line_mode():
        return False
    ser.write(line.encode("utf-8") + b"\n")
    ser.flush()
    # ファームは「行を受けた」(内容は出さない、バイト数だけ) をログに出す。
    return wait_after("serial: muse line (", 4.0)


try:
    ser = serial.Serial(port=port, baudrate=baud, timeout=0.2, write_timeout=2,
                        dsrdtr=False, rtscts=False)
except serial.SerialException as exc:
    print("エラー: シリアルポートを開けません: %s" % exc, file=sys.stderr)
    sys.exit(2)

try:
    try:
        ser.dtr = False
        ser.rts = False
    except (AttributeError, OSError):
        pass
    ser.reset_input_buffer()
    drain(0.2)

    # 安全確認: 副作用のない 1 行 ("0") が行モードで受理されることを先に確かめる。
    # 受理されなければ資格情報を一切送らずに中止する。
    if not send_line("0"):
        print(
            "エラー: 'M' 行モードに入れない/行を受理できませんでした。\n"
            "  SAS-Watch ファームが CONFIG_WATCH_ENABLE_MUSE_LINK=y でビルドされているか、\n"
            "  ポートとボーレート (--baud) を確認してください。資格情報は送信していません。",
            file=sys.stderr,
        )
        sys.exit(3)

    for line in lines:
        if not send_line(line):
            print(
                "エラー: 行モードの応答がありません。途中で中止しました。\n"
                "  シリアルで muse status を確認し、必要なら再実行してください。",
                file=sys.stderr,
            )
            sys.exit(4)

    # muse status の JSON から token_len だけを取り出す (host/vm の値は扱わない)。
    status = None
    deadline = time.monotonic() + 8.0
    while time.monotonic() < deadline:
        match = re.search(rb'"token_len":\s*(\d+)', buffer[seen:])
        if match:
            status = int(match.group(1))
            break
        chunk = ser.read(4096)
        if chunk:
            buffer += chunk
        else:
            time.sleep(0.05)

    if status is None:
        print(
            "警告: muse status の応答を確認できませんでした。\n"
            "  シリアルで muse status を実行し、token_len と ready を確認してください。",
            file=sys.stderr,
        )
        sys.exit(5)

    print("muse status: token_len=%d (期待 %d)" % (status, expected_token_len))
    if status != expected_token_len:
        print(
            "エラー: token_len が一致しません。トークンが途中で切れた可能性があります。\n"
            "  シリアルで muse status を確認し、必要なら再実行してください。",
            file=sys.stderr,
        )
        sys.exit(6)
    print("投入に成功しました (%d 行, 値は表示していません)" % len(expected_set_lines))
    print("次の手順: シリアルで muse status を実行し、ready を確認 (再起動後にも確認)")
finally:
    ser.close()
PY
    echo
    echo "  次の手順: 再起動してもう一度 muse status → ready と token_len を確認"
    echo "  M1 の GO/NO-GO: docs/muse-m1.md"
}

# ---- 引数解析 -------------------------------------------------------------

# サブコマンド形式 (m0_m1.sh backup など) も受け付ける。
if [ $# -gt 0 ]; then
    case "$1" in
        status) set -- --status "${@:2}" ;;
        backup) set -- --backup "${@:2}" ;;
        flash|flash-muse) set -- --flash-muse "${@:2}" ;;
        dump-nvs|dump) set -- --dump-nvs "${@:2}" ;;
        export) set -- --export "${@:2}" ;;
        check) set -- --check "${@:2}" ;;
        restore|restore-saswatch) set -- --restore-saswatch "${@:2}" ;;
        feed|feed-serial) set -- --feed-serial "${@:2}" ;;
        all) set -- --backup --flash-muse "${@:2}" ;;
    esac
fi

while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help)
            usage
            exit 0
            ;;
        --dry-run)
            dry_run=1
            shift
            ;;
        --execute|--yes)
            dry_run=0
            shift
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
        --from)
            [ $# -ge 2 ] || die "--from の後にファイルがありません"
            from_file=$2
            shift 2
            ;;
        --from=*)
            from_file=${1#--from=}
            shift
            ;;
        --force)
            force=1
            shift
            ;;
        --baud)
            [ $# -ge 2 ] || die "--baud の後に値がありません"
            serial_baud=$2
            shift 2
            ;;
        --nvs-offset)
            [ $# -ge 2 ] || die "--nvs-offset の後に値がありません"
            MUSE_NVS_OFFSET=$2
            shift 2
            ;;
        --nvs-size)
            [ $# -ge 2 ] || die "--nvs-size の後に値がありません"
            MUSE_NVS_SIZE=$2
            NVS_FULL_SIZE=$((MUSE_NVS_SIZE))
            shift 2
            ;;
        --status) actions+=("status"); shift ;;
        --backup) actions+=("backup"); shift ;;
        --flash-muse|--flash) actions+=("flash"); shift ;;
        --dump-nvs) actions+=("dump"); shift ;;
        --check) actions+=("check"); shift ;;
        --export) actions+=("export"); shift ;;
        --restore-saswatch|--restore) actions+=("restore"); shift ;;
        --feed-serial) actions+=("feed"); shift ;;
        --all) actions+=("backup" "flash"); shift ;;
        *)
            echo "不明なオプション: $1" >&2
            echo "  tools/muse-gadget/m0_m1.sh --help で使い方を表示します。" >&2
            exit 2
            ;;
    esac
done

[ -f "$EXPORT_PY" ] || die "export_credentials.py がありません: $EXPORT_PY"
[ -f "$FLASH_SH" ] || die "flash.sh がありません: $FLASH_SH"

if [ "${#actions[@]}" -eq 0 ]; then
    info "M0/M1 ランブック (既定は dry-run。実行は --execute)"
    action_status
    print_checklist
    exit 0
fi

for action in "${actions[@]}"; do
    case "$action" in
        status) action_status ;;
        backup) action_backup ;;
        flash) action_flash_muse ;;
        dump) action_dump_nvs ;;
        check) action_check ;;
        export) action_export ;;
        restore) action_restore ;;
        feed) action_feed_serial ;;
        *) die "内部エラー: 不明なアクション $action" ;;
    esac
    echo
done

if [ "$dry_run" = "1" ]; then
    echo "※ dry-run でした。実行するには --execute を付けてください。"
fi
