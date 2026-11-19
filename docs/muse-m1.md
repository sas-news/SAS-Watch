# M1: Muse の資格情報を公式ファームから取り出して SAS-Watch へ移す手順

2026-11-18。T48 の **M1（資格情報スパイク）** の具体的な手順と、その作業を
半自動化するツール (T53) の説明。

目的は「Muse アプリとの BLE ペアリングでしか得られない device token を、
公式 Muse Gadget ファームから取り出し、SAS-Watch の B-min 実装に渡せるか」を
確かめること。**2026-11-18 に実機 (`/dev/ttyACM0`) で M0→M1 の全工程を実施し、GO 判定を得た (§7.1)**。
ツールは合成 NVS イメージ + 実機 NVS ダンプで検証済み (§6)。

前提:
- `docs/muse-gadget.md` の M0 が完了していること（公式ファーム書き込み +
  Muse アプリでペアリング + Wi-Fi 設定）。
- 公式ファーム (`s3-206`, ESP-IDF v6.0.3) の実機ペアリングでは、資格情報と Wi-Fi が
  NVS namespace **`homehub`** に保存される（`access_token`, `refresh_token`,
  `noise_host`, `vm_url` 任意, `ssid`, `password`）。旧/互換レイアウトでは namespace
  **`muse`** (`host` / `vm` / `token`, SDK `esp32/components/muse/muse_settings.c`) も使う。
  `export_credentials.py` は両スキーマ (`muse` / `homehub`) を自動判別し、`--include-wifi`
  時は Wi-Fi 投入行も含める。
- SAS-Watch 側の受け皿は T52 + M1 実機対応で実装済み（NVS namespace `muse` と `wifi.ssid`/`wifi.pass`、
  シリアル大文字 `M` の行モードで `muse set host|vm|token|ssid|pass`）。
  `CONFIG_WATCH_ENABLE_MUSE_LINK=y` でビルドしたファームでのみ有効。

## 0. まとめ（手順の流れ）

1. M0 で公式ファームをペアリング（この時点で実機の NVS に host/vm/token が入る）。
2. **ペアリング後に**実機から NVS を読む（`m0_m1.sh --dump-nvs`）。
3. 読んだ NVS から host/vm/token を取り出し、値を画面に出さず 0600 の
   JSON / コマンド列にする（`m0_m1.sh --export`）。
4. SAS-Watch の全量バックアップを書き戻す（`m0_m1.sh --restore-saswatch`）。
5. SAS-Watch ファーム（`CONFIG_WATCH_ENABLE_MUSE_LINK=y`）を書き込み、
   シリアル `M` 行モードへ投入する（`m0_m1.sh --feed-serial`）。
6. `muse status` で長さだけ確認 → 再起動後も確認。
7. **接続検証 (T55, §5.1)**: `muse start` → `muse status` が `ready` → `muse send` →
   `muse reply` で長さだけ確認 → `muse stop`。ここまで通れば M1 GO。

> **重要な注意: flash.sh の 32MB バックアップには Muse の資格情報は入っていない。**
> `tools/muse-gadget/flash.sh` のバックアップは **M0 を書く前の SAS-Watch の状態**を
> 保存したもの（Muse を書く前の安全網）。資格情報はペアリング後に実機の NVS
> (offset `0x11000` / size `0xC000`) へ書かれるので、**ペアリング後に
> `--dump-nvs` で読み直す**。バックアップから読むと SAS-Watch 側の無関係な
> NVS を読むことになる。

## 1. NVS の場所と読み出し方

公式ファームの SDK `esp32/partitions_muse.csv` より:

| パーティション | オフセット | サイズ |
|---|---|---|
| `nvs` | `0x11000` | `0xC000` (49,152B) |

読み出しは次のどちらでもよい（ツールは両対応）:

```sh
# 実機から NVS パーティションだけ読む (Muse ファーム起動中に。48KB と短い)
python -m esptool --chip esp32s3 -p /dev/ttyACM0 \
  read-flash 0x11000 0xC000 muse-nvs.bin

# 32MB のフルフラッシュを読んで、その一部として渡す
python -m esptool --chip esp32s3 -p /dev/ttyACM0 \
  read-flash 0 0x2000000 full-backup.bin
```

`export_credentials.py` は NVS パーティション単体 (`--nvs`) と、フルフラッシュの
切り出し (`--flash`、既定 offset `0x11000` / size `0xC000`) の両方を受け付ける。
**ESP-IDF の `nvs_tool.py` には依存しない**（標準ライブラリだけで NVS をパースする）。

## 2. ツール 1: `export_credentials.py`

```sh
# キーの有無と長さだけ確認する。値は決して表示しない。全部揃えば exit 0
python3 tools/muse-gadget/export_credentials.py --nvs muse-nvs.bin --check

# 出力例 (host/vm/token の「値」は出ない)
#   NVS イメージ: muse-nvs.bin (49152 バイト, 12 ページ)
#   namespace "muse": あり
#     host : あり (21 バイト)
#     vm   : あり (9 バイト)
#     token: あり (39 バイト)
#   結果: OK (3/3)

# フルフラッシュから NVS を切り出して確認 (offset/size は既定 0x11000/0xC000)
python3 tools/muse-gadget/export_credentials.py --flash full-backup.bin --check

# 0600 の JSON と、SAS-Watch のシリアル投入用コマンド列を書き出す
python3 tools/muse-gadget/export_credentials.py --nvs muse-nvs.bin \
  --out tools/muse-gadget/out/credentials/muse-credentials.json \
  --shell tools/muse-gadget/out/credentials/muse-set.txt
```

- 出力されるコマンド列 (`--shell`、`--include-wifi` 指定時) は次の形（値は実物、vm は空文字のこともある）:

  ```
  muse set ssid <ssid>
  muse set pass <password>
  muse set host <host>
  muse set vm <vm>
  muse set token <token>
  muse status
  ```
  (`--include-wifi` 無しの既定では `host` / `vm` / `token` / `muse status` の 4 行)

- `--out` / `--shell` は既存ファイルがあると失敗する。上書きは `--force`。
- 値の検証: 空・空白/制御文字/非 ASCII・長さ超過 (host/vm 63B、token 1023B) は
  エラー。`--check` は NG で exit 1。**token が無い/空なら M1 は NO-GO。**
- 旧 SAS-Watch Phase 1 の `vm_id` キーしか無い場合は読み取りフォールバックする
  （表示に「旧キー vm_id から」と出る）。
- NVS が壊れている場合 (ページ/エントリ/文字列データの CRC 不一致、ページ境界、
  切り詰め) は明示的にエラーになる。値は例外メッセージにも出さない。

## 3. ツール 2: `m0_m1.sh` (M0→M1 ランブック)

既定は **dry-run**。何も実行せず、状態とチェックリスト、実行予定コマンドを表示する。
実際の操作は `--execute` を付ける。Muse アプリでのペアリングだけはユーザー操作。

```sh
tools/muse-gadget/m0_m1.sh                 # 状態 + 全チェックリスト (dry-run)
tools/muse-gadget/m0_m1.sh --status        # 状態だけ

# M0
tools/muse-gadget/m0_m1.sh --backup --execute        # SAS-Watch の 32MB バックアップ
tools/muse-gadget/m0_m1.sh --flash-muse --execute    # 公式ファーム書き込み
# ここで Muse アプリのペアリング (ユーザー操作)

# M1
tools/muse-gadget/m0_m1.sh --dump-nvs --execute      # ペアリング後に NVS を実機から読む
tools/muse-gadget/m0_m1.sh --check                   # キーと長さだけ確認 (値は出ない。NG は exit 1)
tools/muse-gadget/m0_m1.sh --export --execute        # out/credentials/ へ (0600)
tools/muse-gadget/m0_m1.sh --restore-saswatch --execute  # SAS-Watch へ戻す
# ここで SAS-Watch を CONFIG_WATCH_ENABLE_MUSE_LINK=y でビルドして書き込み (ユーザー操作)
tools/muse-gadget/m0_m1.sh --feed-serial --execute   # シリアル M 行モードで投入
```

- `--execute` なしでは、`--check` 以外はコマンド表示だけ。`--check` は読み取り
  専用なので dry-run でも実行して結果を見せる（キーが揃わなければ exit 1）。
- ポートは `/dev/ttyACM*` / `/dev/ttyUSB*` から自動検出。複数あるときは
  `--port /dev/ttyACM0` で指定する。
- `--from FILE` で `--check` / `--export` の入力 (フルフラッシュ or NVS 単体) を
  明示できる。省略時は `out/nvs/` の最新ダンプ。
- 途中のファイル:
  - `out/backup/waveshare-s3-2.06-factory.bin` — M0 前の SAS-Watch 全量 (0600)
  - `out/nvs/muse-nvs-<日時>.bin` — ペアリング後の NVS (0600)
  - `out/credentials/muse-credentials.json` / `muse-set.txt` — 資格情報 (0600)
  - `out/` は gitignore。**リポジトリ・チャット・ログに値を貼らない。**

### 3.1 `--feed-serial` の安全設計

SAS-Watch のシリアル行モード (T52) は、**大文字 `M`** を送ってから改行までの
1 行を `muse ...` として解釈し、**改行で行モードを抜ける**（= 1 行ごとに `M` が
必要）。`M` を送っていない状態の文字は通常の 1 文字コマンドとして解釈され得る
（`s` は強制 deep sleep、`m` は More など）ため、ツールは次の順で動く:

1. 改行（行モード中なら閉じるだけ、通常状態では無視される）を送ってから `M` を
   送り、シリアルログに `muse line mode` が出るまで待つ。これを**行ごとに**行う。
   `M` が通らなければ **その行は一切送らずに中止**する。
2. 最初に副作用のない `0` の 1 行を送り、ファームが「行を受けた」ログ
   （内容は出ない、バイト数のみ）を返すことを確認する。ここで失敗したら
   資格情報は 1 バイトも送らない。
3. 確認できたら `muse set ...` の各行を、**M → 1 行 → 受理ログ待ち** の順で送る。
4. 最後の `muse status` の JSON から **`token_len` だけ**を取り出して照合する。
   host/vm の値は画面に出さない。

値は Python 側が 0600 ファイルから読み、シェルの引数・標準出力・ログには出さない。
行モードの制限: 1 行 1100B まで（token 1023B + コマンド語で足りる）、
30 秒入力が無いと行モードを抜ける、行の内容はファーム側でもログに出ない。

> 注意: `docs/muse-bmin.md` §10 の例は `M` の後に複数行を並べているが、
> 実装（`watch_app.cpp` の `muse_line_feed`）は改行で行モードを抜けるため、
> **1 行ごとに `M` を送る**必要がある。手でシリアルから打つときも同様。

### 3.2 戻す順序（重要）

Muse ファームは SAS-Watch と**パーティションテーブルが違う**
（Muse: table `0x10000` / SAS-Watch: `0x8000`）。アプリだけ書き戻しても動かない。

1. 資格情報の抽出・保存が済んだら、`--restore-saswatch` で **M0 前の 32MB
   バックアップを全量書き戻す**。
2. SAS-Watch ファームを `CONFIG_WATCH_ENABLE_MUSE_LINK=y` でビルドして書き込む
   （M0 前のバックアップは T52 実装前のビルドかもしれないため）。
3. シリアル `M` 行モードで資格情報を投入する。

## 4. セキュリティ

- `token` (および `host`/`vm`) は Muse アカウントの資格情報。
  **リポジトリ・ログ・チャット・issue に貼らない。** 会話に貼った時点で漏えいと
  みなし、Muse アプリで unpair → 再ペアリングして token を更新する。
- 出力ファイルはツールが 0600 + atomic write で作る。作業後は
  `shred -u tools/muse-gadget/out/credentials/*` などで消す。
- 32MB バックアップには Wi-Fi の SSID/パスワードも入る。`flash.sh` は
  バックアップを 0600 にする。共有しない。
- SAS-Watch 側の NVS は平文（`docs/muse-bmin.md` §9.6 S1）。物理アクセスで読める。
  Phase 2 以降で NVS 暗号化を検討する。
- 公式ファームの NVS も平文。NVS ダンプを残すなら 0600、用が済んだら削除する。
- ツールは値を標準出力・標準エラー・例外メッセージに出さない（テストで検証済み）。

## 5. M1 の GO/NO-GO 判定

| 条件 | 判定 |
|---|---|
| host/vm/token が NVS から読めた | M1 の素材は取れた（次は接続検証） |
| token が空 / キーが無い | NO-GO。B-min は B-full（ペアリング移植）なしでは不可 |
| SAS-Watch 側で NVS 保存・再起動後も読める | 投入経路 OK |
| B-min のセッションで VM に接続できる | M1 GO（Phase 2 続行） |
| 接続時に TLS 内部 RAM 不足 / PSRAM 不足 | RAM 対策（mbedTLS PSRAM 化・バッファ縮小）を先に実施 |

### 5.1 SAS-Watch 側の接続検証 (Phase 2 ③, T55)

資格情報の投入後、SAS-Watch のセッション実装で VM に接続する。
**本文・token はログに残さない** (長さだけ)。

```
M
muse status          # session: off / configured: true / token_len: N
muse start           # 即 return。ログに int_largest / psram_largest が出る
muse status          # session: starting → ready（最大 ~20 秒。error なら last_error）
muse send こんにちは  # 送信（本文は出さない。長さのみ）
muse reply           # 返答の長さだけ（本文は UI/Phase ⑤）
muse stop            # Wi-Fi release / session off
```

判定:

| 確認 | GO の条件 |
|---|---|
| `muse start` のログ | `int_largest` が 12KB 以上、`psram_largest` が 1400KB 以上 (Kconfig 閾値) |
| `muse status` | `session":"ready"` になる (error でない) |
| TLS 内部 RAM | `ready` 直後の `h` の `int_free` が起動前から大きく減っていない (目安: 残り 8KB 以上) |
| `muse send` → `muse reply` | 返答の長さ > 0 |
| `muse stop` | `wifi released` / `session stopped` が出て `h` が回復 |

- `muse start` が「内蔵メモリ不足」「PSRAM 不足」「資格情報が未設定」で止まる場合は、
  それぞれ §5 の表の RAM 対策 / 資格情報の再投入へ。
- TLS で落ちる (リセット/ハンドシェイク失敗) 場合は mbedTLS の PSRAM 化が先决
  (`docs/muse-bmin.md` §12.7)。
- 実測手順の詳細は `docs/muse-bmin.md` §12.6。

## 6. 検証（実機なしで完了していること）

合成 NVS イメージによる unittest を用意した。生成物・値はリポジトリに残さない。

```sh
cd /home/sasnews/deepseek-harness/default-workspace/SAS-Watch
python3 -m unittest discover -s tools/muse-gadget/test -v
```

- **自前パーサのテスト** (ESP-IDF 不要): 文字列/namespace の読み出し、
  旧キー `vm_id`、欠落・空値・空白入り・長すぎる token、複数ページの
  「新しいページが勝つ」、エントリ/ヘッダ/文字列データの CRC 破損、
  ページ整合しないイメージ、フルフラッシュからの切り出し。
- **CLI テスト**: `--check` の exit 0/1、`--out`/`--shell` の内容と 0600、
  上書き拒否と `--force`、壊れたイメージ、32MB フルフラッシュの既定 offset、
  値が stdout/stderr に出ないこと。
- **ランブックのテスト**: `--help`、既定 dry-run、`--check --from`、
  `--dump-nvs` の dry-run、`--export` の dry-run がファイルを作らないこと、
  未知のオプション。
- **ESP-IDF の `nvs_partition_gen.py` で生成した NVS** を使うテスト:
  IDF の python env (`.toolchain/espressif*/python_env/*/bin/python` など) が
  見つかるときだけ実行し、無い環境では「見つからないため skip」と明示する
  (`unittest` の skip メッセージ)。
- **シリアル投入スクリプトの擬似ファームウェアテスト**: `m0_m1.sh` の埋め込み
  Python を取り出し、`serial` を差し替えて「1 行ごとに M を送る」「行モードが
  応答しないときは資格情報を 1 バイトも送らずに中止する」「token_len 不一致を
  検出する」を実機なしで検証する。

現在: **54 テスト / failure 0**（`homehub` スキーマ + `--include-wifi` 4 件追加。2026-11-18）。

## 7. M1 実機検証結果 (GO, 2026-11-18)

`/dev/ttyACM0` の実機で公式 `s3-206` ファームをペアリング後に NVS (`0x11000`, `49,152B`) を抽出し、
SAS-Watch を 32MB バックアップ復元 → `CONFIG_WATCH_ENABLE_MUSE_LINK=y` (`0x20000`) 書込み →
シリアル `M` 行モードで資格情報・Wi-Fi 投入 → 再起動永続化 → 実接続往復・マイク録音まで通して **M1 GO** を確認した（一時平文クレデンシャルファイルは `shred -u` で削除済み）。

| 確認項目 | 実測結果 |
|---|---|
| 公式 NVS (`homehub` スキーマ) | `access_token` 597B / `refresh_token` 147B / `noise_host` `hatch.metaaivm.com` (18B) / `ssid` 17B / `password` 17B / `vm_url` 無し (既定 VM) |
| シリアル投入 + 再起動永続化 | `16B / 0.18s` ペーシングで `token_len=597` を NVS 書込み。`esptool` `hard_reset` 後・`0x20000` 再書込み後も `configured=true`, `token_len=597` を維持 |
| `muse start` 前ヒープ (`'h'` / 開始直前) | `heap=90,747B` (起動後)、`start: int_largest=31,744B`, `psram_free=1,545,180B`, `psram_largest=1,212,416B` (ガード下限 `int=12,288 / psram=1,177,600`) |
| Wi-Fi 安定化〜VM セッション `ready` | 初回 AP 切断 (`0x2a0 reason=2`) を 4 秒 settle 内で捕捉し、RF 完全停止バックオフ (`deinit`/`init` + `disable_pmf=1`) の retry 4 で接続・IP 取得 → TLS 証明書検証 (`api.muse.ai` → VM) → `ready` (起動要求から約 `89.6s` / Wi-Fi 確立後 `3.34s` で VM 接続) |
| `ready` 時ヒープ / RAM | `ready heap: int_free=32,531B int_largest=13,824B psram_free=368,528B psram_largest=360,448B` (ready 中 `'h'` `heap=24,799B`) |
| `muse send` (43B) → `muse reply` | `turn done` `9.5s`、`muse_link: reply done (36 bytes, kept 36)`、`console: last reply 36 bytes (kept 36)` (本文非表示) |
| `muse stop` 回復 | `wifi: session end (wifi off)` / `wifi quiet off` (BLE 広告復帰) / `session=off`、`wifi=false`、`'h'` `heap=62,495B` |
| マイク録音経路 (`'v'` テスト) | `slot_mask=0x2` (`MIC2`) 16kHz mono、+30dB: `samples=88064`, `size=44048`, `peak=3780` (`stream=0`) |

### 7.1 今後の実機残確認 (UI・音声送信・電池)
- **Muse アプリ画面経由 (`boot` / ボタン押下 → Wi-Fi → UI 字幕ページ送り → 15 秒音声ノート送信)** の一連の体感確認（シリアル経由のテキスト往復と `'v'` `MIC2` 録音は確認済み、`voice` 実送信は未実施）。
- **連続稼働・電池消費**（Wi-Fi on-demand + アイドル 120 秒開放 / light sleep 無効区間を含む）。
- **既知課題の追跡**: `muse stop` 後も SDK 側 PSRAM (`muse_chat` タスク等) が残るため、`muse_chat` 停止 API の追加検討 (`docs/muse-bmin.md`)。

## 8. 関連ドキュメント

- `tools/muse-gadget/README.md` — M0/M1 ツールの使い方（`build.sh` / `flash.sh` も）
- `tools/muse-gadget/export_credentials.py --help` / `tools/muse-gadget/m0_m1.sh --help`
- `docs/muse-gadget.md` — 公式ファームのビルド・書き込み・ペアリング（M0）
- `docs/muse-bmin.md` — SAS-Watch 側の `muse set` / シリアル `M` 行モード（T52）と
  セッション `muse start|stop|send|reply` / RAM ガード（T55 §12）
- `docs/muse-port.md` — SAS-Watch への移植調査（T47、M1 の位置づけ）
