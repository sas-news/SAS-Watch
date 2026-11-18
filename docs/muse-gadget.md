# Muse Gadget SDK を SAS-Watch の基板で動かす (T46 調査・ビルド記録)

2026-11-18。**ビルド検証まで完了。実機への書き込み・ペアリングは未実施** (USB 未接続)。
SAS-Watch への変更はこの文書と `tools/muse-gadget/` の追加のみ (firmware/core/android は未変更)。

対象: [facebookincubator/muse-gadget-sdk](https://github.com/facebookincubator/muse-gadget-sdk)
(Apache-2.0) の pinned コミット `ec976f0537db594b0947a2d1a78926575f97a895` (2026-11-18 時点)。
ローカルミラー: `.toolchain/muse-gadget-sdk` (37MB、git 管理外、**再 clone しない**)。
ビルドツール: `tools/muse-gadget/` (使い方の要約は同 `README.md`)。

> 前提: SAS-Watch の実機は、SDK が **`s3-206` として公式サポートしているのと同じ基板**
> (`Waveshare ESP32-S3-Touch-AMOLED-2.06`)。SDK の `esp32/devices/waveshare-s3-206.md` は
> この基板で BLE ペアリング / Wi-Fi / BOOT 押下の音声ノート送信 / テキスト字幕 /
> タッチ・表示を実機で確認済みと記録している。したがって「基板が違うから動かない」
> という障壁は無い。問題は資格情報 (SDK token)・ファームの置き換え・運用。
>
> 統合の可否をさらに深く調べた調査が `docs/muse-port.md` (T47) にある。
> 本ドキュメントは「公式ファームを同じ基板向けにビルドして試せる状態にする」までを扱う。

---

## 0. 結論

| 項目 | 結果 |
|---|---|
| ビルド | **成功**。ESP-IDF **v6.0.3** で `s3-206` プロファイルをビルド |
| アプリ | `muse-gadget.bin` **3,411,968 バイト** (0x341000)。app スロット 4MB に **782,336 バイト (19%) の空き** |
| 構成 | 32MB flash 前提。`partitions_muse.csv` (テーブル @ `0x10000`、app @ `0x20000`) |
| 日本語字幕 | `CONFIG_MUSE_CJK_FONT=y` (Unifont 16px, flash +約1.2MB) でビルド。Noto 版は off |
| リアルタイム音声 | `CONFIG_MUSE_REALTIME_VOICE` off (既知の HTTP 404 のため)。音声ノート + テキスト字幕を主経路にする |
| SDK token | この検証ビルドは **トークン無し** (空)。ユーザーが発行したら `MUSE_SDK_TOKEN` で注入して再ビルドするか、シリアルで `>sdk_token=` を送る |
| 実機 | **未書き込み**。手順とバックアップは `tools/muse-gadget/flash.sh` に実装済み |
| SAS-Watch との関係 | **別ファーム**。書き込むと SAS-Watch は消える (戻しはフラッシュ全量バックアップから) |

成果物 (gitignore。Lead のコミット対象ではない): `tools/muse-gadget/out/`。
サイズ・sha256・設定の記録: 同 `out/build-info.txt` / `out/SHA256SUMS`。

---

## 1. なぜ同じ基板で動くのか (ハードの一致)

SAS-Watch の `docs/board.md` と SDK の `esp32/devices/waveshare-s3-206.md` を突き合わせた。

| 項目 | SAS-Watch (`docs/board.md`) | SDK `s3-206` |
|---|---|---|
| SoC | ESP32-S3R8 (PSRAM 8MB Octal) | ESP32-S3R8, 240MHz, native USB Serial/JTAG |
| Flash | GD25Q256 32MB (実測 32MB) | 32MB GD25Q256 (`esptool flash-id` で 32MB 実測) |
| 画面 | 410×502 AMOLED CO5300 (BSP は SH8601 駆動), QSPI | 同じ (CS12, PCLK11, D0–D3 4–7, RST8, 22列ギャップ) |
| タッチ | FT3168 (ft5x06), I2C 0x38 | 同じ (INT38, RST9) |
| I2C | SCL14 / SDA15 共有 | 同じ |
| オーディオ | ES8311 + ES7210 (mic×2), I2S MCLK16/BCLK41/LRCK45/DOUT40/DIN42, amp46 | 同じ |
| 電源 | AXP2101 (IRQ は ESP32 に未接続) | 同じ |
| ボタン | BOOT=GPIO0, PWR=GPIO10 (SYS_OUT) | BOOT=GPIO0、PWR は AXP2101 の PWRON を I2C で補助ボタン扱い |
| BSP | `waveshare/esp32_s3_touch_amoled_2_06` | 同じ (2.0.0) + `esp_lcd_sh8601` + `esp_lcd_touch_ft5x06` |

**相違点・要実機確認**: PWR ボタンの扱い。SAS-Watch は GPIO10 (SYS_OUT) を読む実装だが、
SDK の s3-206 は「PWR は AXP2101 の PWRON にのみ配線され、ESP32 には来ない。I2C 経由で
aux ボタンとして読む」としている。実機で PWR の挙動を確かめる
(`docs/muse-port.md` も同じ点を確認対象に挙げている)。

---

## 2. Muse Gadget SDK の仕組み

`esp32/` が ESP-IDF ファーム本体。`main/` がリンク/資格情報/コマンド処理、
`components/muse/` がアバター + 音声 + 設定 UI、`components/noise_core/` が Noise プロトコル。

### 2.1 ペアリングと接続

1. 起動すると BLE で `MuseGadget-XXXXXX` としてアドバタイズする。
2. スマホの **Muse アプリ** (設定 > デバイス > 開発者モード ON) から追加する。
3. デバイス側の **BOOT 押下**でペアリングを確認する。BLE の暗号化チャネルで
   Wi-Fi の SSID/パスワードと SDK トークンを受け渡す。
4. Wi-Fi (2.4GHz) に接続し、Muse の VM と **Noise セッション** (HTTP-over-Noise,
   `NOISE_PATH="/v1/noise"`, 443) を張る。以後の会話・コマンドはこのセッションで流れる。
5. 一部ボードでは home-network tunnel が有効 (LAN 内の機器を Muse から触れる)。
   s3-206 は SDK の表で「Enabled (unverified)」= 実機未確認。

資格情報: デバイストークンを Muse API で VM 用トークンに交換する。`mgst_` SDK トークンは
ペアリング時にデバイスが持つもので、**PC 側からこれ単体で VM を叩けるものではない**
(この点は `docs/muse-port.md` §0 の調査参照)。

### 2.2 音声ノートと字幕

- `components/muse/muse_chat_session.cpp` が会話の本体。既定 (`VOICE_NOTE 1`) は
  **押している間の 16kHz PCM16 mono 録音**を音声ノートとして `/chat/stream` に送り、
  **サーバー側で文字起こし**される (24kHz の `/api/voice/dictation` 経路は今は使わない。
  将来 ASR が付いたときのための分岐)。
- 返答は `/chat/subscribe` のイベントでテキストとして届き、**字幕として読む速さで表示**される
  (`CAPTION_TEXT`, `SPEECH_CHARS_PER_S` / `TEXT_CHARS_PER_S`)。
- **読み上げは既定で無い**。`start_tts()` は「自前の TTS API を差す場所」として
  コメントだけ置かれている (`muse_chat_session.cpp`)。SAS-Watch の個体はスピーカー無効なので、
  「返答 = 字幕」という既定動作がそのまま都合よい。
- ターン上限・無音終了・遅延返答の扱いなどは SDK 側に実装済み (20 秒で
  「Muse is working on it.」を出してターンを手放す、等)。

### 2.3 デバイスの自己記述とコマンド

- `link.register` で `device_family: "gadget"`, `model_id: muse-gadget-waveshare-s3-206`,
  `metadata.model_name: Waveshare ESP32-S3-Touch-AMOLED-2.06` を送る。
- `device.guide` は `devices/guides/muse.md` を返す (画面つきガジェットの説明)。
- コマンドは `display.*` (画像表示)、`device.health`、`device.discover` など。
  Muse はデバイスが登録したコマンドだけを呼べる。
- 自分でコマンドを足す方法は `esp32/AGENTS.md` の「Adding a command」に整理されている
  (`build_register_json()` と `on_ws_command()` の 2 か所)。

### 2.4 UI・設定・電源

- LVGL 9.5.0 のアバター + 設定画面 (Wi-Fi / BLE / サウンド / スリープ / バッテリー / 電源)。
  アバターの上下ドラッグで音量、横スワイプで設定。s3-206 は BOOT=PTT、PWR=aux。
- 画面オフ時は light sleep (s3-206 オーバーレイで `CONFIG_PM_ENABLE`,
  `CONFIG_FREERTOS_USE_TICKLESS_IDLE`, BLE modem sleep 等)。電池駆動の挙動は実機未確認。
- OTA は有効 (`CONFIG_HOMEHUB_OTA_ENABLED=y`)。署名は SDK 同梱の `dev_signing_key.pem`、
  Secure Boot は無効 (eFuse を焼かない)。

---

## 3. このワークスペースに作ったビルド環境

| もの | 場所 / 値 | サイズ |
|---|---|---|
| ESP-IDF v6.0.3 | `.toolchain/esp-idf-v6.0.3` (tag `v6.0.3`, commit `76f5dedd...`)、`install.sh esp32s3` 実行済み | 2.5GB |
| IDF ツール | `.toolchain/espressif-v6` (`IDF_TOOLS_PATH`) | 4.7GB |
| コンポーネントキャッシュ | `.toolchain/comp-cache-v6` (`IDF_COMPONENT_CACHE_PATH`) | 258MB |
| SDK ミラー | `.toolchain/muse-gadget-sdk` (pinned `ec976f0...`) | 37MB |
| ビルドディレクトリ | `.toolchain/muse-gadget-sdk/esp32/build-muse-waveshare-s3-206/` | — |

既存の ESP-IDF 5.5.1 (`.toolchain/esp-idf`, `.toolchain/espressif`) は**触っていない**。

### 3.1 セットアップ手順 (再現用)

```sh
cd /home/sasnews/deepseek-harness/default-workspace/.toolchain

# 1) ESP-IDF v6.0.3 (shallow + submodules)。既存の 5.5.1 とは別ディレクトリ。
GIT_CONFIG_GLOBAL=/dev/null git clone -b v6.0.3 --depth 1 --recursive \
  https://github.com/espressif/esp-idf.git esp-idf-v6.0.3

# 2) esp32s3 のツールを IDF_TOOLS_PATH を分けてインストール
cd esp-idf-v6.0.3
export IDF_TOOLS_PATH=/home/sasnews/deepseek-harness/default-workspace/.toolchain/espressif-v6
./install.sh esp32s3

# 3) ビルド (SDK トークンがあれば MUSE_SDK_TOKEN に入れる)
cd /home/sasnews/deepseek-harness/default-workspace/SAS-Watch
tools/muse-gadget/build.sh
```

注意: ESP-IDF のコンポーネントマネージャは既定で `~/.cache/Espressif` に書くが、
この環境のサンドボックスでは書けない。`build.sh` は
`IDF_COMPONENT_CACHE_PATH=<workspace>/.toolchain/comp-cache-v6` を設定する。
手動で `idf.py` を叩くときも同じ変数を設定すること。

### 3.2 所要時間 (この環境、8 コア)

| 工程 | 時間 |
|---|---|
| IDF v6.0.3 の clone (submodules 含む) | 約 4 分 |
| `install.sh esp32s3` (ツール取得) | 約 3 分 |
| 初回 configure (コンポーネント取得込み) | 約 30 秒 |
| クリーンビルド (`build.sh --clean`、キャッシュ温) | 約 2 分 17 秒 (全 2,250 ステップ) |
| 再ビルド (設定変更なし) | 数十秒〜 |

---

## 4. ビルド結果 (2026-11-18 11:23 JST)

`tools/muse-gadget/build.sh --clean` (トークン無し) の成果物。

| ファイル | 書き込み位置 | サイズ (bytes) | sha256 |
|---|---|---|---|
| `muse-gadget.bin` (アプリ) | `0x20000` | **3,411,968** | `2d10b59f5719f03a7f491d14977e53282bbdb4f2e5c1e2b9a5533e2fff37d05b` |
| `bootloader.bin` | `0x0` | 22,752 | `cb7852dc020bdbf3296f152a9a9d1dfdadf397c6e55c5d4501c9565cb259e7c2` |
| `partition-table.bin` | `0x10000` | 3,072 | `f1eed33f4d8c528ce6085b46abfeb3a176beefe71601330a81c1cc15fc58e08f` |
| `ota_data_initial.bin` | `0x1d000` | 8,192 | `7d2c7ac4888bfd75cd5f56e8d61f69595121183afc81556c876732fd3782c62f` |

- **sha256 はビルドごとに変わる** (アプリは署名とビルド時刻、ブートローダもビルド時刻を含む)。
  同じ設定でも再ビルドすると値が変わるので、同一性の確認はその時点の
  `out/SHA256SUMS` を使う。サイズは再現する。
- アプリは 4MB の app スロット (`ota_0`) に収まり、残り 782,336 バイト (19%)。
  日本語フォント (Unifont) が約 1.2MB を占める。
- 32MB flash のうち使用するのは `partitions_muse.csv` の末尾 `files` まで
  (`0xA30000 + 4MB` ≒ 14.7MB) で、32MB に十分収まる。

### 4.1 パーティション (`partitions_muse.csv`)

| 名前 | 種別 | オフセット | サイズ |
|---|---|---|---|
| bootloader | — | `0x0` | — |
| partition table | — | **`0x10000`** | — |
| `nvs` | data/nvs | `0x11000` | 0xC000 |
| `otadata` | data/ota | `0x1D000` | 0x2000 |
| `phy_init` | data/phy | `0x1F000` | 0x1000 |
| `ota_0` (app) | app/ota_0 | **`0x20000`** | 4MB |
| `ota_1` (app) | app/ota_1 | `0x420000` | 4MB |
| `prod_data` / `prod_bak` | data 0x40/0x41 | `0x820000` / `0x821000` | 0x1000 ずつ |
| `apps` | data/littlefs | `0x830000` | 2MB |
| `files` | data/littlefs | `0xA30000` | 4MB |

**SAS-Watch との非互換**: SAS-Watch のパーティションテーブルは `0x8000` 置きで
(`firmware/sdkconfig` の `CONFIG_PARTITION_TABLE_OFFSET=0x8000`)、Muse は `0x10000` 置き。
アプリのオフセットは偶然どちらも `0x20000` だが、**テーブルと NVS の位置が違う**ので、
SAS-Watch に戻すときはアプリだけ書き戻しても動かない。**フラッシュ全量のバックアップから
丸ごと戻す**こと (`flash.sh` の `read-flash 0 0x2000000`)。

### 4.2 sdkconfig の主要値

```
CONFIG_IDF_TARGET="esp32s3"
CONFIG_ESPTOOLPY_FLASHSIZE="32MB"
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions_muse.csv"
CONFIG_GADGET_SDK_TOKEN=""                 # この検証ビルドはトークン無し
CONFIG_MUSE_CJK_FONT=y                     # 日本語・中国語・韓国語の字幕 (Unifont)
# CONFIG_MUSE_CJK_FONT_NOTO is not set     # Noto Sans SC 版は使わない
# CONFIG_MUSE_REALTIME_VOICE is not set    # リアルタイム通話は既定 off
CONFIG_HOMEHUB_OTA_ENABLED=y
```

オーバーレイ: `sdkconfig.defaults` → `devices/sdkconfig.muse` →
`devices/sdkconfig.muse-waveshare-s3-206` (この順で読み込む。SDK の README 指定どおり)。
生成された `sdkconfig` はビルドディレクトリ側にあり、SDK ソースは変更していない。

---

## 5. 使い方 (要点)

詳細・コマンド例は `tools/muse-gadget/README.md`。

1. **SDK トークンを発行**する: https://gadgets.muse.ai/settings/sdk-tokens
   (Muse アカウントが必要。Gadget SDK Terms に同意)。**値は表示・保存・コミットしない**。
2. **ビルド**: `MUSE_SDK_TOKEN=mgst_... tools/muse-gadget/build.sh`
   (`build.sh` はトークンをビルドディレクトリの sdkconfig にだけ入れ、ログでは
   `mgst_[REDACTED]` に置き換える)。
3. **書き込み**: `tools/muse-gadget/flash.sh`
   - ポート自動検出 (`/dev/ttyACM*`)。必要なら `--port`。
   - **先にフラッシュ全体 32MB を `out/backup/` へ読み出す** (`read-flash 0 0x2000000`)。
   - `esptool write-flash "@flash_args"` で bootloader / table / otadata / app を書き込む。
4. **ペアリング**: Muse アプリ > 設定 > デバイス > 開発者モード ON →
   Add Device で `MuseGadget-XXXXXX` → 画面指示 / BOOT 押下 → Wi-Fi (2.4GHz) 設定。
5. **戻す**: `python -m esptool --chip esp32s3 -p PORT -b 921600 write-flash 0 <backup>.bin`。
6. **ログ**: `. <workspace>/.toolchain/esp-idf-v6.0.3/export.sh && idf.py -p PORT monitor`。
   健康な起動は `link.main: Muse Gadget starting`、ボード名は `muse: board: ...`。

トークンを後から入れる方法: 書き込み済みならシリアルコンソールで
`>sdk_token=mgst_...` を送る (NVS に保存され再起動。ビルド時の値より優先)。
再ビルドするなら `MUSE_SDK_TOKEN=... tools/muse-gadget/build.sh`。

---

## 6. 日本語字幕

- `CONFIG_MUSE_CJK_FONT=y` で GNU Unifont 16×16 のビットマップを字幕フォントに持つ
  (`components/muse/fonts/muse_font_cjk_16.c`、リポジトリ同梱。生成不要)。
  flash +約 1.2MB、RAM は増えない。
- `CONFIG_MUSE_CJK_FONT_NOTO` は **off**。Noto Sans SC 版は GB2312 の漢字 6763 字まで、
  ハングル非対応で、サイズも約 910KB。Unifont 版の方がカバレッジが広い。
- 字幕は 16px の 1 行ずつ表示になる (小さい画面向けの仕様)。
- **音声認識の日本語品質は実機未検証**。音声ノートはサーバー側で文字起こしされる
  (Muse のアカウント/VM 側の処理)。この SDK 側に言語設定は無い。

---

## 7. SAS-Watch との関係

SAS-Watch はすでに「Muse と話す」機能を**スマホ経由**で持っている
(`docs/muse-integration.md`, `docs/muse-setup.md`: 時計 → BLE → スマホ Vosk →
自前 muse2api → Muse。音声は保存しない 0 円運用)。Muse Gadget SDK はこれと**別の経路**。

| 観点 | SAS-Watch (現行) | Muse Gadget SDK (今回ビルド) |
|---|---|---|
| 経路 | 時計 → BLE → スマホ → muse2api → Muse | 時計 → Wi-Fi → Muse VM 直結 (Noise) |
| 必要なもの | スマホアプリ + muse2api サーバー | Muse アカウント + **SDK token** + Muse アプリ |
| 字幕 | 自前 UI に表示 | SDK のアバター UI + 字幕 |
| 音声 | 16kHz ADPCM を BLE 送信、Vosk で認識 | 16kHz PCM 音声ノート、サーバーで認識 |
| ファーム | SAS-Watch ファーム | **別ファーム** (書き込むと入れ替わる) |
| 戻し | アプリを `0x20000` に書き直し | フラッシュ全量バックアップから復元 |
| 0 円性 | 0 円 (自前サーバー) | Muse アカウント/提供条件に依存 (Gadget SDK Terms) |
| 日本語 UI | 自前 | SDK の英語 UI + 日本語字幕 |

**共存はできない** (1 個のファームしか書けない)。選択肢は:

- **A. 別ファームとして試す** (今回の成果物)。`tools/muse-gadget/` でビルド → 書く → 戻す。
  まず「Muse 直結」の体験とアカウント可用性を確かめるのが目的。実機未検証。
- **B. SAS-Watch へ「Muse と話す層」だけ移植**。BLE ペアリングで VM トークンを得る必要があり、
  TLS+Noise の RAM (PSRAM 1.2MB 超・内部 RAM 30〜50KB)、Wi-Fi 常時接続の電池、
  ESP-IDF 6 前提の差分など壁が多い。詳細と工数見積りは `docs/muse-port.md` (T47)。
  同調査の推奨は「現行の C 経路を維持し、B は資格情報スパイク (M1) が通ったときだけ最小構成」。
- **C. 現行のスマホ + muse2api 経路を維持** (SAS-Watch の本線)。

**このタスクの推奨**: A を実機で一度試し、字幕・音声・電池・戻しやすさを確認してから
B の GO/NO-GO を決める。B を進める場合も SAS-Watch のファームを壊さない範囲で
「M0 (試す) → M1 (資格情報) → B1/B2」の順にする (`docs/muse-port.md` §9)。

---

## 8. 実機で確認すること (未検証リスト)

- **書き込み**: `flash.sh` のバックアップ → 書き込み → 起動ログ
  (`link.main: Muse Gadget starting`、`muse: board: Waveshare ESP32-S3-Touch-AMOLED-2.06`)。
- **表示・タッチ**: 410×502 の向き/欠け、設定画面のタッチ。
  (SDK 側は実機確認済みと記録しているが、この個体・このビルドでは未確認)
- **BLE ペアリング**: Muse アプリの開発者モードから。BOOT での確認。Wi-Fi は 2.4GHz。
- **音声ノート**: BOOT 押下 → 録音 → 送信 → 返答字幕。日本語の字幕表示 (Unifont) と認識品質。
- **PWR**: SDK は AXP2101 経由の aux 扱い。SAS-Watch の GPIO10 実装との差を実機で確認。
- **リアルタイム通話**: `CONFIG_MUSE_REALTIME_VOICE` は off。有効化すると
  `POST /voice/realtime` が HTTP 404 になる報告あり (2026-11-09 時点)。
  アカウント側の提供条件の可能性があり、このビルドでは試していない。
- **home-network tunnel / images from Muse**: SDK の s3-206 ドキュメントでも未確認。
- **電池・スリープ**: 画面オフ後、BOOT で起床できるか。電池持ちは未計測。
- **戻し**: バックアップからの全量復元、SAS-Watch ファームの再書き込み。

---

## 9. 既知のリスク・制約

- **SAS-Watch が消える**: パーティションテーブルごと変わる。部分復元は不可、全量復元が必要。
  バックアップ (32MB) は `out/backup/` に置かれ、gitignore される。別の場所にも退避すること。
- **SDK token が必須**: 未設定だとペアリングできない。トークンは秘密情報で、
  build ディレクトリの sdkconfig にだけ入る (ログは `mgst_[REDACTED]` に置換)。
  リポジトリ・文書・会話に貼らない。
- **Muse アカウントの利用条件**: Gadget SDK Terms と、アカウントで gadget が使えるかに依存。
- **SDK とファームの更新**: 今回は pinned `ec976f0...`。SDK は活発に変わるので、
  更新するときは BUILD 手順と s3-206 オーバーレイの差分を確認する。
- **32MB flash 前提**: Waveshare のサンプルは 16MB 表記だが、回路図は GD25Q256 (32MB)、
  SDK も 32MB でビルドする。SAS-Watch の `docs/board.md` でも実測 32MB。
- **IDF v6.0.3 固定**: 他のバージョンは非対応 (SDK の README/AGENTS.md)。
  既存 5.5.1 環境とは別ディレクトリ・別 IDF_TOOLS_PATH で共存させている。
- **ディスク**: IDF 6 の clone 2.5GB + ツール 4.7GB + キャッシュ 258MB を使う。
- **サンドボックス**: `~/.cache` に書けないため、コンポーネントキャッシュを
  `.toolchain/comp-cache-v6` に逃がしている。手動で `idf.py` を叩くときも同様に設定する。

---

## 10. ライセンス・出典

- ファームウェア: [facebookincubator/muse-gadget-sdk](https://github.com/facebookincubator/muse-gadget-sdk)
  **Apache-2.0**。pinned コミット `ec976f0537db594b0947a2d1a78926575f97a895`。
- 同梱フォント (GNU Unifont など) は SIL OFL 1.1 等。第三者コードの一覧は SDK の
  `README.md` の License 表を参照。
- ESP-IDF v6.0.3: Apache-2.0 (Espressif)。
- この文書と `tools/muse-gadget/` は SAS-Watch リポジトリの一部。ビルド生成物
  (`tools/muse-gadget/out/`) はコミットしない。

参照:

- SDK: https://github.com/facebookincubator/muse-gadget-sdk
- SDK token: https://gadgets.muse.ai/settings/sdk-tokens / Gadget SDK Terms: https://gadgets.muse.ai/sdk-terms
- ESP-IDF v6.0.3 setup: https://docs.espressif.com/projects/esp-idf/en/v6.0.3/esp32s3/get-started/
- SAS-Watch の現行 Muse 連携: `docs/muse-integration.md`, `docs/muse-setup.md`, `docs/muse2api-server.md`
- SAS-Watch への移植調査: `docs/muse-port.md` (T47)
