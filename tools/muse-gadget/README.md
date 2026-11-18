# Muse Gadget SDK を SAS-Watch の基板で試す

SAS-Watch と同じ基板 **Waveshare ESP32-S3-Touch-AMOLED-2.06** 向けに、Meta 公式の
Muse Gadget SDK (`facebookincubator/muse-gadget-sdk`, Apache-2.0) が持つ
`s3-206` プロファイルのファームウェアをビルドし、実機に書き込むためのツール一式。

> **これは SAS-Watch のファームウェアではない。** 書き込むと SAS-Watch の
> ファームウェア (アプリ・パーティションテーブル・NVS) は消え、Muse の
> 公式ファームに入れ替わる。`flash.sh` は既定で実機のフラッシュ全体
> (32MB) をバックアップしてから書き込む。戻し方は「元に戻す」を参照。

背景・仕組み・調査結果は [`docs/muse-gadget.md`](../../docs/muse-gadget.md) にまとめた。

## できること (SDK の s3-206 プロファイル)

- Muse アプリと BLE でペアリングし、2.4GHz Wi-Fi につないで Muse の VM と
  暗号化セッション (Noise) を張る。
- BOOT ボタンの押下で音声ノートを送り、Muse の返答を画面に字幕表示する
  (音声の読み上げは別途 TTS 連携が要る)。
- アバターと設定画面 (タッチ操作)、画面の明るさ・スリープ、音量などの設定。
- Muse から画像表示などのコマンドを受けられる (GUI 付きボード向け)。

## 必要なもの

| 必要なもの | 内容 |
|---|---|
| 基板 | Waveshare ESP32-S3-Touch-AMOLED-2.06 (SAS-Watch と同じもの)。32MB flash / 8MB PSRAM |
| USB ケーブル | **データ通信できる** USB-C ケーブル。ポートは `/dev/ttyACM*` (チップ内蔵 USB Serial/JTAG) |
| SDK トークン | https://gadgets.muse.ai/settings/sdk-tokens で発行する `mgst_...`。Muse アカウントが必要 |
| Muse アプリ | スマホ側。設定 > デバイス > 開発者モード を ON にして使う |
| Wi-Fi | **2.4GHz** (5GHz 専用 SSID ではつながらない)。ペアリング時の設定で使う |
| ESP-IDF v6.0.3 | SDK の唯一の対応バージョン。このワークスペースでは `.toolchain/esp-idf-v6.0.3` に導入済み |
| SDK のミラー | `.toolchain/muse-gadget-sdk` (コミット `ec976f0537db594b0947a2d1a78926575f97a895` を固定)。**再 clone しない** |

SDK トークンは **表示・保存・コミットしない**。`build.sh` は環境変数
`MUSE_SDK_TOKEN` から受け取り、ビルドディレクトリの `sdkconfig` にだけ埋め込む
(その `sdkconfig` は SDK ミラー側の gitignore 対象)。未設定でもビルドでき、
その場合は後からシリアルで `>sdk_token=mgst_...` を送るか、再ビルドで入れる。

## ビルド

```sh
# トークン無しでビルド (ペアリングはできない。後で入れる手順は下)
tools/muse-gadget/build.sh

# トークンを埋め込んでビルド (推奨)
MUSE_SDK_TOKEN=mgst_... tools/muse-gadget/build.sh

# 初回や SDK を切り替えたとき: ビルドディレクトリを作り直す
tools/muse-gadget/build.sh --clean
```

- ビルド設定は SDK の `devices/sdkconfig.muse` + `devices/sdkconfig.muse-waveshare-s3-206`。
  このスクリプトが **日本語字幕 `CONFIG_MUSE_CJK_FONT=y`**、
  **`CONFIG_MUSE_CJK_FONT_NOTO` off**、**リアルタイム音声 `CONFIG_MUSE_REALTIME_VOICE` off**
  を sdkconfig に入れる。
- 生成物は `tools/muse-gadget/out/` (gitignore) にコピーされる:
  `muse-gadget.bin` (アプリ)、`bootloader.bin`、`partition-table.bin`、
  `ota_data_initial.bin`、`flash_args`、`SHA256SUMS`、`build-info.txt`、`build.log`。
- 現在の検証済みビルド: アプリ **3,411,968 バイト** (4MB の app スロットに
  782,336 バイト = 19% の空き)。サイズ・sha256・設定は `out/build-info.txt` と
  `out/SHA256SUMS` に記録される。**再ビルドすると署名とビルド時刻が変わるため
  sha256 も変わる** (サイズは同じ)。
- 予備のビルド環境変数: `MUSE_GADGET_SDK_DIR`、`MUSE_IDF_EXPORT`、
  `MUSE_IDF_TOOLS_PATH`、`IDF_COMPONENT_CACHE_PATH`。詳しくは `build.sh --help`。

## 書き込み

**先にビルドする。** 書き込みは事前バックアップ込みで数分かかる。

```sh
# ポート自動検出 + バックアップ + 書き込み
tools/muse-gadget/flash.sh

# ポートを指定する場合
tools/muse-gadget/flash.sh --port /dev/ttyACM0

# バックアップだけ取る (初回に一度やっておくと安心)
tools/muse-gadget/flash.sh --backup-only
```

`flash.sh` の動き:

1. `/dev/ttyACM*` などからポートを自動検出 (複数あれば `--port` で指定)。
2. **実機のフラッシュ全体 (32MB, `read-flash 0 0x2000000`) を
   `out/backup/waveshare-s3-2.06-factory.bin` に保存**。既にあれば読み直さない。
3. `esptool write-flash "@flash_args"` で bootloader / パーティションテーブル /
   otadata / アプリを書き込む (NVS は消さないが、パーティションテーブルが
   SAS-Watch と違うので実質まっさらになる)。
4. 戻し方とログの見方を表示する。

`--no-backup` は非推奨。どうしても書き込みに失敗するときは、USB を挿し直し、
**BOOT を押しながら RESET を押して離す**とダウンロードモードになって再試行できる。

### 元に戻す (SAS-Watch に戻す)

```sh
# バックアップから丸ごと戻す (ファクトリファームもこの方法)
python -m esptool --chip esp32s3 -p /dev/ttyACM0 -b 921600 write-flash 0 \
  tools/muse-gadget/out/backup/waveshare-s3-2.06-factory.bin

# あるいは SAS-Watch をビルドして書き直す (firmware/README.md の手順)
```

## ペアリング

1. 書き込み後、ボードが起動して `MuseGadget-XXXXXX` として BLE アドバタイズする
   (画面にアバターが出る)。
2. スマホの Muse アプリで **設定 > デバイス > 開発者モード** を ON。
3. **設定 > デバイス > Add Device (+)** で `MuseGadget-XXXXXX` を選ぶ。
4. 画面の指示に従う。確認を求められたら基板の **BOOT ボタンを短く押す**。
5. Wi-Fi (2.4GHz) の SSID / パスワードをアプリから渡す。接続するとアバターが
   つながった状態になる。
6. 音声ノート: **BOOT を押している間**録音し、離すと送信。返答は字幕で出る。
   ペアリングし直すときは BOOT を 5 秒長押ししてリセットしてから追加し直す。

PWR ボタンは SDK では AXP2101 経由の aux ボタン扱い (詳細は docs を参照)。

## M1: 資格情報の取り出しと投入 (公式ファーム → SAS-Watch)

M0 (公式ファーム + ペアリング) の後、公式ファームが NVS (namespace `muse`,
offset `0x11000` / size `0xC000`) に保存した `host` / `vm` / `token` を
**値を画面に出さずに**取り出し、SAS-Watch のシリアル `M` 行モードへ投入する
ツール。詳細は [`docs/muse-m1.md`](../../docs/muse-m1.md)。

```sh
# M0 (公式ファーム)
tools/muse-gadget/m0_m1.sh --backup --execute      # SAS-Watch の 32MB バックアップ
tools/muse-gadget/m0_m1.sh --flash-muse --execute  # 公式ファーム書き込み
# → Muse アプリでペアリング (ユーザー操作。開発者モード ON → Add Device → BOOT)

# M1 (資格情報)
tools/muse-gadget/m0_m1.sh --dump-nvs --execute     # ペアリング後に NVS を実機から読む
tools/muse-gadget/m0_m1.sh --check                  # キーと長さだけ確認 (値は出ない)
tools/muse-gadget/m0_m1.sh --export --execute       # out/credentials/ へ (0600)
tools/muse-gadget/m0_m1.sh --restore-saswatch --execute
# → SAS-Watch を CONFIG_WATCH_ENABLE_MUSE_LINK=y でビルドして書き込み (ユーザー操作)
tools/muse-gadget/m0_m1.sh --feed-serial --execute  # M 行モードで投入し token_len を確認
```

- `m0_m1.sh` は既定が **dry-run**。実行には `--execute`。Muse アプリの
  ペアリングだけはユーザー操作なので、チェックリストを表示する。
- SAS-Watch の `M` 行モードは **1 行ごとに `M` が必要**（改行で抜ける）。
  `--feed-serial` は行ごとに `M` を送り、受理ログを確認してから次の行へ進む。
- **`flash.sh` の 32MB バックアップは M0 前の SAS-Watch の状態**で、Muse の
  資格情報は入っていない。資格情報はペアリング後に `--dump-nvs` で読む。
- 個別に使うなら `tools/muse-gadget/export_credentials.py`:
  `--nvs FILE` / `--flash FILE`、`--check` (キーの有無と長さのみ)、
  `--out JSON` / `--shell FILE` (0600)、`--force` (上書き)。
- 出力は 0600・atomic。値は標準出力/エラー/ログに出さない。使い終わったら削除する。
- テスト: `python3 -m unittest discover -s tools/muse-gadget/test -v`
  (ESP-IDF の `nvs_partition_gen.py` を使うテストは IDF python env が無い環境では
  skip と明示される)。

## 日本語字幕

`build.sh` が既定で `CONFIG_MUSE_CJK_FONT=y` (GNU Unifont 16px, flash +約1.2MB)
にする。`CONFIG_MUSE_CJK_FONT_NOTO` は off (Noto 版は漢字 6763 字までで
ハングル非対応のため)。字幕は 16px の 1 行ずつ表示になる。

## 既知の制約

- **SAS-Watch とは別ファーム**。同時には入らない。切り替えは書き込み直し。
- **リアルタイム音声通話 (`CONFIG_MUSE_REALTIME_VOICE`) は既定 off**。
  2026-11-09 時点で `POST /voice/realtime` が HTTP 404 になる報告があり、
  アカウント側の提供条件の可能性がある。音声ノート + テキスト字幕を使う。
- Muse の返答は**字幕 (テキスト)**。読み上げは SDK の `start_tts` フックに
  自前の TTS を差す必要がある。
- 32MB flash 前提。Waveshare のサンプルは 16MB 表記だが、回路図の GD25Q256 は
  32MB で、SDK の s3-206 プロファイルも 32MB でビルドする。
- 実機書き込み・ペアリング・音声ノートは**この環境では未検証** (USB 未接続)。
  検証済みなのはビルドまで。
- SDK トークンを変えた / 失効させた場合は、ペアリングし直す必要がある。

## ライセンス

- ファームウェア: [facebookincubator/muse-gadget-sdk](https://github.com/facebookincubator/muse-gadget-sdk)
  Apache-2.0 (固定コミット `ec976f0537db594b0947a2d1a78926575f97a895`)。
- 同梱フォント (Unifont など) は SIL OFL 1.1 など。詳細は SDK の README を参照。
- このディレクトリのスクリプトは SAS-Watch リポジトリの一部 (ライセンスはリポジトリに従う)。
  ビルド生成物 (`out/`) はコミットしない。
