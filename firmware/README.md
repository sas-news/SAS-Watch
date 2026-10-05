# firmware/

Waveshare **ESP32-S3-Touch-AMOLED-2.06** 向けファームウェア (ESP-IDF 5.5 + 公式BSP v2 + LVGL 9)。
`docs/plan.md` R 章 Phase 2 の最小形: 時刻・日付・電池%を出して、ボタンとタッチで復帰する。

## 構成

```
main/app_main.cpp        起動処理・簡易電源管理 (無操作10秒で画面オフ)
components/board/        ボード層 (BSP依存部品: buttons/pmic/rtc/imu/haptics/sleep)
components/ui_min/       最小LVGL画面 (黒背景: 時刻HH:MM, 日付, 電池%, ボタン表示)
components/diag/         起動時診断 (システム情報, I2Cスキャン)
components/XPowersLib/   AXP2101ドライバ (公式サンプル 01_AXP2101 から vendor, MIT)
partitions.csv           docs/plan.md I章の案 (OTA x2 + littlefs x2 + coredump)
sdkconfig.defaults       esp32s3 / Flash 16MB / PSRAM Octal / NimBLE / PM / LVGL
```

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

USB-C で接続 (ネイティブUSB, GPIO19/20)。ポート名は環境で読み替える。

```sh
idf.py -p /dev/ttyACM0 flash monitor
# 終了: Ctrl+]
```

初回書き込み前に Factory ファームのバックアップを推奨 (plan.md Phase 0):

```sh
esptool.py -p /dev/ttyACM0 read_flash 0 0x1000000 factory_backup.bin
esptool.py -p /dev/ttyACM0 flash_id
```

## Phase 0 チェック手順 (実機が届いたら)

起動ログを見て `docs/board.md` の未確認リストを埋める:

| 見る場所 | 確認すること |
|---|---|
| `diag: flash=..MB` | 32MB (GD25Q256) が見えるか → `sdkconfig.defaults` の FLASHSIZE |
| `diag: psram=..` | PSRAM 8MB が確保できているか |
| `diag: i2c scan` | `0x18,0x34,0x38,0x40,0x51,0x6B` が全部応答するか |
| `board.pmic: AXP2101 ok` | 電池%/充電状態が正しいか |
| `board.rtc: RTC ..` | RTC 時刻が正しいか。電源OFF後も時刻を保つか |
| `board.imu: QMI8658 ok` | who_am_i が読めるか |
| `app: button BOOT/PWR` | BOOT 短押し/長押し/2回押し、PWR 短押し/長押しが出るか |
| 画面 | 時刻・日付・電池%が出るか、画面向き、タッチで復帰するか |
| 無操作10秒後 | 画面が消えて、タッチ/ボタンで復帰するか |

PWR 長押し 6 秒 → AXP2101 がハード電源 OFF するはず (要実機確認)。
モーター (GPIO18 パルス) は `board::haptics::pulse()` を呼ぶ経路がまだ無いので、実機で有無を確認してから試す。

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

## まだ入っていないもの / 推測値

`// TODO(hw)` を grep すれば一覧になる。主なもの:

- Flash 32MB 設定 (現在は 16MB として使う)
- QMI8658 INT1 の極性・wake-on-motion の閾値 (`0x40` は例値)
- 画面の向き・タッチ座標の一致
- `panel_power` (ALDO2 カット) 後のディスプレイ再初期化時間
- 本格的な PowerManager / light sleep / deep sleep (core/ と統合して作る)
- 日本語フォント
