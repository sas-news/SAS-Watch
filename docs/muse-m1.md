# M1: Muse の資格情報を公式ファームから取り出して SAS-Watch へ移す手順

2026-11-18。T48 の **M1（資格情報スパイク）** の具体的な手順。
目的は「Muse アプリとの BLE ペアリングでしか得られない device token を、
公式 Muse Gadget ファームから取り出し、SAS-Watch の B-min 実装に渡せるか」を
確かめること。**まだ実機では実施していない**（M0 の書き込み・ペアリング待ち）。

前提:
- `docs/muse-gadget.md` の M0 が完了していること（公式ファーム書き込み +
  Muse アプリでペアリング + Wi-Fi 設定）。
- 公式ファームの SDK は NVS namespace **`muse`** に `host` / `vm` / `token` を
  保存する（SDK `esp32/components/muse/muse_settings.c`: NS="muse",
  keys host/vm/token, token 最大 1023B）。
- SAS-Watch 側の受け皿は T52 で実装する Phase 2 ①（NVS namespace `muse`、
  シリアル `muse set host/vm/token`）。T52 の完了後にコマンド名を確定する。

## 0. まとめ（手順の流れ）

1. M0 で公式ファームをペアリング（この時点で NVS に host/vm/token が入る）。
2. フラッシュ全体のバックアップから NVS パーティションを取り出す
   （`tools/muse-gadget/flash.sh` のバックアップを利用）。
3. IDF の `nvs_tool.py` で namespace `muse` の `host`/`vm`/`token` を読む。
4. SAS-Watch ファーム（`CONFIG_WATCH_ENABLE_MUSE_LINK=y`）へシリアルで投入。
5. `muse status` で長さだけ確認 → B-min の接続検証（M1 の GO/NO-GO）。

## 1. NVS の場所（公式ファーム）

SDK の `esp32/partitions_muse.csv` より:

| パーティション | オフセット | サイズ |
|---|---|---|
| `nvs` | `0x11000` | `0xC000` (49,152B) |

`tools/muse-gadget/flash.sh` は書き込み前に `read-flash 0 0x2000000` で
32MB 全体を `tools/muse-gadget/out/backup/` に保存している。ここから
NVS 部分だけを切り出す（値は秘密なので、作業ファイルは 0600・作業後に削除）。

```sh
cd tools/muse-gadget/out/backup
# 最新のバックアップファイル名は ls で確認する
dd if=<backup>.bin of=/tmp/muse-nvs.bin bs=1 skip=$((0x11000)) count=$((0xC000))
chmod 600 /tmp/muse-nvs.bin
```

実機から直接読む場合（バックアップが無い場合）:

```sh
python -m esptool --chip esp32s3 -p /dev/ttyACM0 read-flash 0x11000 0xC000 /tmp/muse-nvs.bin
chmod 600 /tmp/muse-nvs.bin
```

## 2. NVS の読み出し

ESP-IDF 同梱の `nvs_tool.py` を使う（IDF 5.5.1 で動作確認済み）。

```sh
IDF=/home/sasnews/deepseek-harness/default-workspace/.toolchain/esp-idf
python3 "$IDF/components/nvs_flash/nvs_partition_tool/nvs_tool.py" \
    -d minimal /tmp/muse-nvs.bin
```

出力例（**実際の値は秘密**。以下は合成イメージでの例）:

```
 muse:host = hatch.metaaivm.com
 muse:vm = <VM の ID>
 muse:token = <device token (長い)>
```

注意:
- この出力を**会話・チャット・issue・コミットに貼らない**。
- シェル履歴に残したくない場合は、出力をファイルにリダイレクトして
  `chmod 600` し、必要なら `shred -u` で消す。
- `muse:token` が空 / 無い場合は **M1 は NO-GO**（device token が
  NVS に保存されていない＝別の資格情報経路が必要）。`host`/`vm` が無い場合も
  SDK の設定（`muse_settings_set_hatch_*`）がどこに保存されたかを再確認する。

### 合成イメージでの手順検証（実機なしで確認済み）

IDF の `nvs_partition_gen.py` で namespace `muse` の `host/vm/token` を持つ
NVS イメージを作り、`nvs_tool.py -d minimal` で 3 キーが読めることを確認済み
（2026-11-18、作業用の一時ディレクトリで実施。生成物・値はリポジトリに残さない）。

```sh
# 参照: $IDF/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py
#       generate <csv> <out.bin> 0xC000
```

## 3. SAS-Watch への投入（T52 の実装後）

`CONFIG_WATCH_ENABLE_MUSE_LINK=y` でビルドしたファームを書き込み、
USB-Serial/JTAG のモニタから行ベースのコマンドを送る（T52 で実装。
実装後に確定したコマンド名を使う。以下は設計時の想定）。

```
muse set host <host>
muse set vm <vm>
muse set token <token>      # 値はエコーされないこと
muse status                 # host/vm/token_len/ready のみ表示
```

再起動後にもう一度 `muse status` を実行し、`token_len` が期待どおりで
`ready=true` になることを確認する（NVS 永続化の確認）。

## 4. M1 の GO/NO-GO 判定

| 条件 | 判定 |
|---|---|
| host/vm/token が NVS から読めた | M1 の素材は取れた（次は接続検証） |
| token が空 / キーが無い | NO-GO。B-min は B-full（ペアリング移植）なしでは不可 |
| SAS-Watch 側で NVS 保存・再起動後も読める | 投入経路 OK |
| B-min のセッションで VM に接続できる | M1 GO（Phase 2 続行） |
| 接続時に TLS 内部 RAM 不足 / PSRAM 不足 | RAM 対策（mbedTLS PSRAM 化・バッファ縮小）を先に実施 |

## 5. セキュリティ

- `token` は Muse アカウントのデバイス資格情報。**リポジトリ・ログ・チャットに
  出さない**。作業ファイルは 0600 で作り、用が済んだら削除する。
- SAS-Watch 側の NVS は平文（Phase 2 で NVS 暗号化/ラップを検討）。
- 漏れた可能性がある場合は Muse アプリでアンペア（unpair）し、再ペアリングで
  新しい token にする。
- 公式ファームの NVS には Wi-Fi の SSID/パスワードも入る。バックアップ
  ファイル全体を共有しない。

## 6. 未検証・TODO

- 実機での M0 ペアリング → NVS ダンプ → 3 キー抽出（未実施。USB 未接続）。
- T52 の `muse` シリアルコマンド名・出力形式の確定。
- token のフォーマット・長さの実測（合成値では 1023B 上限のみ確認）。
- B-min セッション接続時の TLS 内部 RAM / PSRAM 実測（Phase 2 ③）。
