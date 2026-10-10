# 実機ブリングアップ手順

実機が届いたときの「書き込み → 起動確認 → 動作確認」の手順。
board.md の未確認リストと plan.md の【要実機】を全部ここで潰す。
各項目は **[ ] 番号** でコメントできるようにしてある。失敗した番号とシリアルログを貼ってくれれば原因を調べる。

## 0. PC 側の準備

```bash
pip install esptool pyserial
```

- ボードを USB-C で接続 → ポート名を確認
  - Linux: `/dev/ttyACM0` (`dmesg | tail` で確認)
  - macOS: `/dev/cu.usbmodem*`
  - Windows: `COMx`
- **ポートが出ない**: BOOT ボタンを押したまま USB を挿す（ダウンロードモード強制）

## 1. チップ確認（30秒）

```bash
esptool.py --chip esp32s3 -p /dev/ttyACM0 flash_id
```

期待値: `Manufacturer: c8 (GD)` / `Device: 4019`（GD25Q256 32MB）
→ board.md の Flash 行を更新する材料になる。

## 2. 書き込み

`build/sas-watch-bringup.bin`（bootloader+partition+OTA+app の合体イメージ）を配布する。

```bash
esptool.py --chip esp32s3 -p /dev/ttyACM0 -b 460800 \
  --before default_reset --after hard_reset \
  write_flash 0x0 sas-watch-bringup.bin
```

自動でリセットされず真っ暗のままなら PWR を一度押す。

**esptool の注意（実測）**: pacman 版 esptool 5.3.1 の stub flasher は `read_flash` が flash 0x23d000 付近で必ず転送停止する（開始位置・サイズ・ボーレート不問）。バックアップ等の読み出しは `--no-stub`（ROM ローダー経路）か ESP-IDF 同梱の esptool 4.12 を使う。

## 3. 起動ログ確認（最重要・まずここだけ見る）

別ターミナルで:

```bash
python3 -m serial.tools.miniterm /dev/ttyACM0 115200
```

開いたままボードをリセット（PWR or 抜き差し）。USB-Serial-JTAG はリセット時にポートが一度切れて再列挙されるので、出ないときは `miniterm` を開き直す。

**期待するログ（この順）:**

```
I (xxx) app: === SAS-Watch firmware boot ===
I (xxx) diag: chip=esp32s3 cores=2 rev=x.x
I (xxx) diag: flash=32MB (external)        ← 16MB設定でも実容量32が出るはず
I (xxx) diag: psram=7xxxKB free ...
I (xxx) diag: reset_reason=...
I (xxx) diag: i2c scan start (expect 0x18,0x34,0x38,0x40,0x51,0x6B)
I (xxx) diag:   found 0x18                 ← ES8311 コーデック
I (xxx) diag:   found 0x34                 ← AXP2101 PMIC
I (xxx) diag:   found 0x38                 ← FT3168 タッチ（※下の注意）
I (xxx) diag:   found 0x40                 ← ES7210 マイクADC
I (xxx) diag:   found 0x51                 ← PCF85063 RTC
I (xxx) diag:   found 0x6B                 ← QMI8658 IMU
I (xxx) pmic: AXP2101 ok: batt=xx% charging=0 vbus=0
```

**注意: 0x38 (FT3168) はこの時点で無応答でも正常。** スキャンは BSP のタッチ初期化（TP_RESET=GPIO9 駆動）より前に走るため、IC がまだリセット中。タッチの生死は §4 の実操作で判断する。

**赤信号（どれか出たらその番号の項目は飛ばして報告）:**
- `AXP2101 begin failed` → 電源系全部 NG
- `display init failed` → 画面系 NG
- `watch_app start failed` / `lvgl lock timeout` → 起動自体 NG
- `i2c scan` で found が 6 個未満 → 0x38 以外が欠けていたらそのアドレスが犯人

## 4. 画面・タッチ

- [ ] 4-1 時計 face が表示される（向き・隅の丸み・AMOLED の黒が真っ黒か）
- [ ] 4-2 スワイプで画面遷移（上下左右）
- [ ] 4-3 タップでアプリが開く / 戻れる
- [ ] 4-4 タッチ座標のズレ・左右反転・上下反転がない
- [ ] 4-5 描画の縞・ガタつき・奇数座標由来のずれがない（CO5300 は偶数座標必須）

## 5. ボタン

| 操作 | 期待 |
|---|---|
| BOOT 短押し | primary（face 上で何か起きる） |
| BOOT 長押し(0.8s) | Dev 画面 |
| BOOT 2回押し | ボイスメモ録音トグル |
| PWR 短押し | 戻る |
| PWR 長押し(0.8s) | 電源メニュー |
| PWR 超長押し(6s) | ハード電源 OFF（AXP2101 が強制断） |
| OFF → PWR | 再電源 ON |

- [ ] 5-1 BOOT 3操作すべて
- [ ] 5-2 PWR 3操作すべて
- [ ] 5-3 電源 OFF → ON

## 6. 電源遷移（設定で秒数を変えられる）

- [ ] 6-1 無操作で dim → screen off に進む
- [ ] 6-2 screen off からタッチで復帰
- [ ] 6-3 light sleep → タッチ(GPIO38) / BOOT(GPIO0) で復帰
- [ ] 6-4 deep sleep → BOOT(GPIO0,LOW) / PWR(GPIO10,HIGH) / 持ち上げ(GPIO21) で復帰
- [ ] 6-5 raise-to-wake：画面 OFF 中に持ち上げる → 点灯（= QMI8658 WoM + INT1 極性の確認になる）
- [ ] 6-6 復帰時間を体感でメモ（plan.md では数百 ms 想定）

## 7. IMU・歩数

- [ ] 7-1 振る / 歩く → Steps 画面の歩数が増える
  - HW ペドメーター（QMI8658A 仕様書準拠の推測実装）の実機検証になる。**最重要項目**
- [ ] 7-2 歩数が暴発しない（静止時に勝手に増えない）

## 8. 充電

- [ ] 8-1 USB-C 挿す → 30 秒以内に `charging=1` / `vbus=1`（ログ or 画面上の電池表示）
- [ ] 8-2 抜く → `charging=0`

## 9. BLE

- [ ] 9-1 スマホの nRF Connect（無料アプリ）でスキャン → `SAS-Watch-XXXX` が見える
- [ ] 9-2 接続できる
- [ ] 9-3 ペアリング要求で時計側にパスキー確認 UI が出る（NUMCMP）
- [ ] 9-4 companion app で接続・時刻同期

## 10. あれば確認

- [ ] 10-1 振動モーターの有無（開けて見る。配線 P1/P2 のみならスキップ）
- [ ] 10-2 USB 電流計があるなら Active/dim/screen off/deep sleep の mA をメモ
- [ ] 10-3 microSD を挿しての挙動

### スピーカー未搭載機体について
この機体はスピーカー/コーデック IC を物理撤去済み → `sdkconfig` の `CONFIG_WATCH_DISABLE_AUDIO=y` で音声初期化（PA/I2S/codec/audio タスク）を全部スキップしている。音声コード自体は残っており、搭載機体では `n` にすればそのまま使える。有効のまま動かすと `bsp_audio_init` 内の `i2s_alloc_dma_desc` が内蔵 RAM 不足で ESP_ERR_NO_MEM → abort する（codec IC が I2C で応答していても起きる。I2S の DMA バッファ確保失敗なので IC 検出では防げない）。

## 報告の送り方

1. ブートログ全文（コピペ）
2. 失敗した番号
3. 画面写真（ズレ・崩れがあれば）

この手順の結果で board.md の未確認リストと plan.md の【要実機】を更新する。
