# firmware/

Waveshare **ESP32-S3-Touch-AMOLED-2.06** 向けファームウェア (ESP-IDF 5.5 + 公式BSP v2 + LVGL 9)。
`docs/plan.md` R 章 Phase 4〜6 の MVP: `core/` の Runtime を回して時計単体で使える。

## 構成

```
main/app_main.cpp        起動処理 (NVS→ボード層→LVGL→watch_app開始)
components/watch_app/    core::Runtime を回すアプリタスク + 入力/PowerApply/BLE接着
components/platform_esp/ platform 実装 (Clock=esp_timer, KV=NVS, Log=esp_log, キューロック)
components/watch_core/   ../core を ESP-IDF コンポーネントとして取り込む
components/ui/           LVGL UI (Theme→Component→Screen の3段。日本語フォント同梱)
components/ble_link/     NimBLE GATT サーバ (CONFIG_SAS_BLE_LINK)
components/theme_store/  /assets littlefs マウント + BULK kind="theme" 受信・展開
components/board/        ボード層 (BSP依存部品: buttons/pmic/rtc/imu/haptics/sleep)
components/diag/         起動時診断 (システム情報, I2Cスキャン)
components/XPowersLib/   AXP2101ドライバ (公式サンプル 01_AXP2101 から vendor, MIT)
components/audio/        マイク (ES7210) 録音・ADP1 保存 (スピーカーは無効)
components/ota/ wifi/    HTTPS OTA と Wi-Fi
vendor/                  パッチを当てた外部コンポーネント (BSP / esp_lvgl_port / sh8601)。
                         main/idf_component.yml の override_path で差し替え。vendor/README.md
partitions.csv           docs/plan.md I章の案 (OTA x2 + littlefs x2 + coredump)
sdkconfig.defaults       esp32s3 / Flash 32MB / PSRAM Octal / NimBLE / PM / LVGL
```

## 操作方法

ボタン割り当てはスマホアプリの設定で変えられる (settings の `button.*`)。既定値:

| 入力 | 動作 |
|---|---|
| BOOT 短押し | 画面の主アクション (タイマー開始/停止 など) |
| BOOT 長押し | 開発者画面 |
| BOOT 2連打 | なし (既定では未使用。設定で割り当て可) |
| PWR 短押し | 画面 ON/OFF の切り替え (どの画面でも) |
| PWR 長押し | 電源メニュー (6 秒以上で AXP2101 がハード電源断) |
| 右スワイプ / 戻るピル | 戻る |
| 下スワイプ (Home) | クイック設定 |
| 上スワイプ (Home) | アプリ一覧 |
| 画面長押し | 現在のアプリへ (既定は何もしない) |

画面が消えている間の入力は「画面を起こす」だけで、アクションとしては処理しない。
無操作 8 秒で薄暗く、12 秒で画面 OFF。画面 OFF が `deep_sleep_after_s`
(既定 1800 秒) 続くと deep sleep に入る (PWR・腕上げ・タイマー期限で復帰)。
USB 接続中 (`usb_keep_awake=1`) は deep sleep しない。

依存は `main/idf_component.yml` 参照 (BSP `waveshare/esp32_s3_touch_amoled_2_06` ^2,
`lvgl/lvgl` 9.5, `espressif/button`, `waveshare/qmi8658`, `waveshare/pcf85063a`)。

## ビルド

ESP-IDF v5.5.x をインストール済みのこと ([docs](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/get-started/))。

> **注意**: Component Registry の `lvgl 9.6.0~1` の manifest が `$CONFIG{LV_USE_*}` を使っていて、
> ESP-IDF 5.5 同梱の `idf-component-manager 2.2.x` では依存解決が `MissingKconfigError: LV_USE_FREETYPE` で落ちる。
> `idf-component-manager` を **2.5 系以上**に更新してからビルドすること (IDF の制約 `~=2.2` = <3.0 の範囲内)。
> CI (`.github/workflows/firmware.yml`) も同じ手順を踏んでいる。

```sh
. ~/esp/esp-idf/export.sh        # IDF 環境を有効化
pip install "idf-component-manager~=2.5"   # 上記バグ回避のため (初回のみ)
cd firmware
idf.py set-target esp32s3        # 初回のみ (sdkconfig.defaults が効く)
idf.py build
```

Docker でも可:

```sh
cd firmware
docker run --rm -v "$PWD":/project -w /project -e IDF_TARGET=esp32s3 espressif/idf:v5.5 \
  sh -c 'pip install -q "idf-component-manager~=2.5" && idf.py build'
```

## 書き込み・ログ

USB-C で接続 (ネイティブUSB, GPIO19/20)。ポートはクラッシュや再列挙で
`/dev/ttyACM0` ↔ `/dev/ttyACM1` に変わるので、毎回 `ls /dev/ttyACM*` で確かめる。

アプリだけ書き込むなら esptool を直接使うのが速くて安全 (**オフセットは必ず 0x20000** =
ota_0。ほかの値だと partition を壊してブートループする):

```sh
python -m esptool -p /dev/ttyACM0 -b 460800 --before default_reset --after hard_reset \
  --chip esp32s3 write_flash 0x20000 build/sas_watch.bin
python -m esp_idf_monitor --port /dev/ttyACM0 build/sas_watch.elf   # 終了: Ctrl+]
```

`idf.py flash` は cmake を再実行する。`IDF_CCACHE_ENABLE=1` を付けずに呼ぶと
全リビルドになるので注意。同梱テーマの書き込みはリポジトリ直下の AGENTS.md を参照。

### シリアルのデバッグコマンド

モニタ (USB-Serial/JTAG) に 1 文字送ると実行する (`watch_app.cpp` の serial_dbg_task)。

| 文字 | 動作 |
|---|---|
| `b` | 描画ベンチ。主要 6 画面で全面再描画とスクロールの ms を `bench:` 行に出す (docs/perf.md) |
| `t` | 同梱テーマを順に切り替える (standard→mame→cosmos→cute→cyber。NVS に保存される) |
| `h` | app タスクの心拍・内蔵ヒープ |
| `s` | 強制 deep sleep |
| `g` | 直前の起床ゲート・入眠の診断を再表示 |
| `a` / `c` / `w` | IMU の生値 / WoM ラッチのクリア / WoM の再 arm |
| `r` | raise_to_wake を ON |
| `d` | 電源系の設定を既定値に戻す |

初めて実機を立ち上げるときの手順は docs/bringup.md。

## 音声メモと音

> 現状: この個体はスピーカー/コーデックを撤去済みで再生は無効。録音も、開始時に
> I2S DMA 用の内蔵 RAM が足りず失敗している (docs/roadmap.md T18、保留中)。
> 以下は仕様として残す。

マイク (ES7210) で録った音声メモと、スピーカー (ES8311) で鳴らす音の仕様。

### 録音フォーマット

- **IMA-ADPCM** (4bit) を独自ヘッダ `ADP1` (16バイト) 付きで保存
  （`core/src/adpcm.cpp` と同じ形式。リトルエンディアン、ニブルは下位→上位の順）。
  WAV の素の PCM16 だと16kHz mono で約 1.9MB/分になり storage パーティション
  (約1.8MB) に入らないので、4bit に潰した ADP1 を採用した。
- サンプルレート 16kHz / mono / 実質 **約8KB/秒**（60秒 ≒ 470KB）。

### 容量ルール

| 項目 | 値 |
| --- | --- |
| 1件の最大録音時間 | **60秒**（到達したら自動停止して保存） |
| 音声メモの件数 | 最大 **8件** |
| 音声データの合計 | **1MB** まで（storage の約半分） |
| テキストメモ | 従来通り最大64件 × 256B（NVS、音声とは別管理） |

- 件数か合計容量が溢れたら **一番古い音声メモから順に削除**して新しい録音を置く。
  テキストメモは音声の容量に数えない。
- 音声実体は littlefs (`/storage/memo/`) に `memo_xxxx.adp` として置く。

### 動作ルール

- 録音中・再生中・ビープ中は `Res::Audio` の PowerManager Lease を取るので
  Deep Sleep / 画面消灯に落ちない。
- PA の電源 (GPIO46 `PA_CTRL`) は**再生中だけ ON**。録音時・アイドル時は OFF
  （ノイズ・消費電力対策）。
- タイマー終了音・ボタンのクリック音は設定 `audio.click` (0/1) で切れる。
  音量は `audio.volume` (0-100)。設定画面から変えられる。

### BLE での音声メモ転送

- `memo.audio.get{id}` の RES 直後に、時計→スマホ方向の **BULK 転送**
  （kind=`"memo"`）で ADP1 の実体を送る。スマホ側は SHA-256 を照合してから
  ADP1 → WAV (16kHz PCM16) に変換して再生・共有する。詳細は
  `docs/protocol-v1.md` の BULK 節。

## Android とのペアリング手順

時計とスマホを Bluetooth でつなぐ方法です。難しい操作はありません。

1. **時計の電源を入れる。** 勝手にペアリング待ちの状態になります
   （画面の電波マークみたいなところに、未接続の表示が出ます）。
2. **スマホのアプリを開く。** SAS-Watch のアプリを起動して
   「時計を探す」ボタンを押します。
3. **時計を選ぶ。** `SAS-Watch-XXXX`（XXXXは時計ごとの4桁）という名前が
   出るので、それをタップします。
4. **数字が合っているか見る。** スマホと時計の両方に同じ6桁の数字が出ます。
   同じだったら、時計側で「はい」を、スマホ側でも「ペアする」を押します。
5. **これで完了。** 一度ペアリングすれば、次からはアプリを開くだけで
   自動でつながります。

### うまくいかないとき

- **時計が見つからない** → 時計の画面がついているか確認。それでもダメなら
  時計を再起動（PWRボタン長押しで電源OFF→ON）して、もう一度1から。
- **数字が違う・エラーになる** → スマホのBluetooth設定で
  `SAS-Watch-XXXX` の登録を「解除/削除」して、1からやり直してください。
- **接続が切れる** → 少し離れると切れます。近づけば自動でつなぎ直します。

> 技術メモ: ペアリング方式は LE Secure Connections の Numeric Comparison。
> ボンド情報は時計の NVS に保存されるので、ペアリングは初回だけで済みます。

## 推測値

`// TODO(hw)` を grep すると、実機で確かめていない値の一覧になる。
実機で分かったことは docs/handoff.md (現状) と docs/board.md (ハード) に書く。
