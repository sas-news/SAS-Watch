# Watch ⇄ Phone BLE Protocol v1

## GATT
Service UUID: `7a1e0001-5d2b-4b8e-9c3f-6f0a11c0a7e1`

| Char | UUID | Props | 用途 |
|---|---|---|---|
| ctrl  | `7a1e0002-5d2b-4b8e-9c3f-6f0a11c0a7e1` | write, notify | REQ (Phone→Watch) / RES (Watch→Phone) |
| event | `7a1e0003-5d2b-4b8e-9c3f-6f0a11c0a7e1` | notify | EVT (Watch→Phone) |
| bulk  | `7a1e0004-5d2b-4b8e-9c3f-6f0a11c0a7e1` | write-without-response, notify | BULK_* |

ペアリング：LE Secure Connections + Bonding + Numeric Comparison（時計に6桁表示）。ボンド済み端末以外の ctrl 書き込みは拒否。

## Frame (little endian)
```
offset size field
0      1    ver       = 1
1      1    type      0x01 REQ, 0x02 RES, 0x03 EVT, 0x10 BULK_START, 0x11 BULK_CHUNK, 0x12 BULK_ACK, 0x13 BULK_END
2      1    flags     bit0 = more fragments (1フレームが MTU を超える場合)
3      1    seq       フラグメント番号 (0..)
4      2    msg_id    REQ と RES で同じ値。EVT は送信側の連番
6      2    len       payload の長さ
8      len  payload   REQ/RES/EVT は CBOR map、BULK_CHUNK は生バイト
8+len  2    crc16     CRC-16/CCITT-FALSE (0x1021, init 0xFFFF) を offset 0..8+len-1 に対して
```
1フレームの最大長 = ATT MTU - 3。MTU は 247 を要求する。

## REQ / RES (CBOR map)
REQ: `{ "m": <method string>, "p": <params map> }`
RES: `{ "ok": true, "r": <result> }` または `{ "ok": false, "e": <error code string>, "msg": <string> }`

method の一覧はこの表が唯一の正。時計 (core/protocol/dispatch.cpp) と
スマホ (android/protocol) はこの表通りの名前を送受信する。

| method | params | result |
|---|---|---|
| `hello` | `{proto:1, app:"0.1.0", os:"android"}` | `{proto:1, fw:"0.1.0", caps:["timer","stopwatch","counter","memo","theme","audio","alarm","notify","media","wifi","ota","steps","agent","walk"]}` |
| `time.set` | `{epoch:<int s>, tz_offset_min:<int>}` (`tz_offset_min` は省略可) | `{}` |
| `device.info` | `{}` | `{battery:<0-100 または不明時 -1>, charging:<bool>, fw:<str>, free_heap:<int>, free_psram:<int>}` |
| `settings.get` | `{keys:[...]}` (省略・空なら全部) | `{<key>:<value>,...}` |
| `settings.set` | `{<key>:<value>,...}` | `{}` |
| `timer.start` | `{seconds:<int>}` (1 以上) | `{}` |
| `timer.stop` | `{}` | `{}` |
| `memo.create` | `{text:<str>}` (空でない) | `{id:<int>}` |
| `memo.list` | `{i:<開始index>, n:<最大件数 (1-16)>}` | `{total:<int>, memos:[{id, kind:"text"\|"voice", sec:<voice秒>, size:<voice byte>}]}` 新しい順 |
| `memo.get` | `{id:<int>}` | `{id, kind, sec, size, text:<本文 (音声メモは文字起こし結果、未処理なら空)>}` |
| `memo.delete` | `{id:<int>}` | `{}` |
| `memo.audio.get` | `{id:<int>}` | `{id, size, sha256:<bytes32>}` この直後に時計から BULK (kind=`"memo"`, id=メモid & 0xFFFF) が送られる |
| `memo.text.set` | `{id:<int>, text:<str>}` (音声メモの文字起こし結果、1-256B) | `{}` (`not_found` = 音声メモが無い / text が長すぎる) |
| `notify.post` | `{app:<str>, title:<str>, body:<str>}` | `{}` |
| `media.state` | `{title:<str>, artist:<str>, playing:<bool>}` (`playing` は省略可、任意 `art:<u32>` = アルバムアート識別子。0/省略なら画像なし) | `{}` |
| `alarm.list` | `{}` | `{alarms:[{id:<int>, hour:<0-23>, min:<0-59>, dow:<曜日bit bit0=日..bit6=土, 0=毎日>, on:<bool>}]}` (最大5件) |
| `alarm.set` | `{hour, min}` + 省略可 `{id:<int>, dow:<int>, on:<bool>}` | `{id:<int>}` | `id` 省略/0 で新規 (満杯なら `busy`)、既存 id で更新 (`not_found`)。`dow` 0-0x7F 省略時 0、`on` 省略時 true |
| `alarm.delete` | `{id:<int>}` | `{}` | 無い id は `not_found` |
| `agent.reply` | `{id:<int>, text:<str>}` | `{}` | AI の返答。id は直前の `agent.request` / BULK kind=`"agent_audio"` の id と同じ。text は最大960バイト (UTF-8)。適用できない id でも `ok` |
| `agent.partial` | `{id:<int>, text:<str>, final?:<bool>}` | `{}` | リアルタイム文字起こしの途中経過 (CUMULATIVE: 前回を置き換える)。`final:true` で発話確定。text は最大 768 バイト (UTF-8)、空文字でクリア。id は BULK kind=`"dictation"` の id と同じ。時計は現在の dictation と一致するときだけ表示する |
| `debug.info` | `{}` | `{heap_int, heap_int_min, psram, psram_min, uptime_s, reset_reason, fw, screen, battery, batt_mv?, vbus_mv?, ble_rssi?, ble_itvl_ms?}` | 実機診断 (T13)。`?` は接続中/PMIC 検出時のみ |
| `wifi.set` | `{ssid:<1-32文字>, pass:<0または8-63文字>}` | `{}` |
| `wifi.status` | `{}` | `{configured:<bool>, ssid:<str>}` |
| `ota.start` | `{url:<http(s)://〜 ≤255文字>, sha256:<bytes32>, version:<str>}` | `{}` |
| `ota.status` | `{}` | `{active:<bool>, stage:<str>, pct:<0-100>, msg:<str>, version:<str>}` |
| `steps.get` | `{}` | `{steps:<今日の歩数>, goal:<steps.goal の値>}` |
| `walk.state` | `{state:<str>, route?:<str>, total_m?:<u32>, done_m?:<u32>, remain_m?:<u32>, permille?:<0-1000>, elapsed_s?:<u32>, eta_epoch?:<int64>, pace_s_per_km?:<u32>, started_epoch?:<int64>, next_cp?:<str>, next_cp_m?:<u32>, off_route_m?:<u32>, accuracy_m?:<u32>, updated_epoch?:<int64>, msg?:<str>}` | `{}` | 歩く大会の表示状態。EVT `walk.query` への応答としてスマホ→時計へ送る (§歩く大会) |

`wifi.set` の資格情報は settings keys の表に**入れない**。ssid/pass は
NVS に直接保存され、読み出し経路は `wifi.status` の ssid のみ
(pass はいかなる method でも読み出せない)。

`ota.start` は Wi-Fi セッション方式 OTA: 時計が Wi-Fi を立ち上げて
url のイメージを HTTPS 取得→ sha256 照合→ 再起動。更新中は EVT
`ota.progress` が飛び、終了時に `ota.result` が飛ぶ。
`ota.status` の `stage` は `"idle"|"wifi"|"download"|"verify"|"done"|"reboot"|"fail"`。
失敗の詳細は `msg`、更新先の表示名は `version`。もう1つの更新経路は
BULK kind `"firmware"` (下記 BULK 節)。

error code:

| code | 意味 |
|---|---|
| `bad_request` | CBOR が壊れている / params の形や型が違う |
| `unknown_method` | 表に無い method |
| `unsupported_proto` | `hello` の proto が一致しない |
| `busy` | 時計が処理できない状態 |
| `internal` | 時計内部の失敗 |
| `not_found` | id で指定したものが無い |

### settings keys
`settings.get` / `settings.set` で使うキーの一覧はこの表が唯一の正。
`settings.get` の結果はこの表の順で返す（`keys` 指定時は要求した順）。

| key | type | 既定 | 説明 |
|---|---|---|---|
| `brightness` | u32 | 50 | 画面の明るさ 0-100 |
| `dim_after_s` | u32 | 8 | Active→Dim までの秒数 |
| `screen_off_after_s` | u32 | 12 | Active→ScreenOff までの秒数 |
| `deep_sleep_after_s` | u32 | 1800 | ScreenOff→DeepSleep までの秒数 |
| `tz_offset_min` | i32 | 0 | UTC からのオフセット (分)。`time.set` でも記憶される |
| `theme` | text | `standard` | theme id 文字列 |
| `button.boot.short` | text | `primary` | BOOT 短押しの Action 名 |
| `button.boot.long` | text | `nav.dev` | BOOT 長押しの Action 名 |
| `button.boot.double` | text | `none` | BOOT 2回押しの Action 名 |
| `button.pwr.short` | text | `screen_off` | PWR 短押しの Action 名 (T17: 既定は画面ON/OFF。戻るはピル/右スワイプ) |
| `button.pwr.long` | text | `power_menu` | PWR 長押しの Action 名 |
| `button.pwr.double` | text | `none` | PWR 2回押しの Action 名 |
| `audio.volume` | u32 | 70 | クリック音・ビープ・メモ再生の音量 0-100 |
| `audio.click` | u32 | 1 | ボタンのクリック音 ON/OFF (0/1) |
| `notify.vibrate` | u32 | 1 | 通知受信時の振動 ON/OFF (0/1) |
| `raise_to_wake` | u32 | 1 | 腕を上げて画面オン (0/1) |
| `steps.goal` | u32 | 8000 | 歩数目標 (歩数画面の達成率・steps.get の goal) |
| `face` | text | `bold` | 文字盤 id (`bold`/`analog`/`hud`/`minimal`/`chara_side`/`chara_bubble`) |
| `clock_font` | text | `auto` | 時計数字フォント id (`auto`/`oswald`/`bebas`/`orbitron`/`outfit`/`chakra`) |
| `agent.q1` | text | `今日の予定は？` | AI の定型質問ボタン 1 (空 = 非表示、63バイトまで) |
| `agent.q2` | text | `今の天気は？` | AI の定型質問ボタン 2 (同左) |
| `agent.q3` | text | (空) | AI の定型質問ボタン 3 (同左) |
| `usb_keep_awake` | u32 | 1 | USB (VBUS) 接続中のスリープ抑制: 0=なし, 1=deep sleep 抑制, 2=deep sleep+画面OFF 抑制 (Dim までは許可。手動消灯は抑制しない) |

### Action 名 (button.* の値)
`button.*` キーに設定できる Action 名はこの表が唯一の正
(core `input_mapper.cpp` の `named_actions()` と一致)。
表に無い名前を `settings.set` しても値自体は保存されるが、
ボタン割り当てには反映されない（現在値のまま）。

| Action 名 | 日本語ラベル | 動作 |
|---|---|---|
| `none` | なし | 何もしない |
| `back` | 戻る | 1つ前の画面に戻る |
| `home` | ホーム | ホーム画面へ |
| `primary` | 主ボタン | 画面ごとの主アクション (開始/停止など) |
| `screen_off` | 画面OFF | 画面を消す |
| `wake` | 復帰 | スリープからの復帰要求 |
| `power_menu` | 電源メニュー | 電源メニューを開く |
| `nav.quick` | クイック設定 | クイック設定画面を開く |
| `nav.notifications` | 通知 | 通知画面を開く |
| `nav.more` | アプリ一覧 | アプリ一覧画面を開く |
| `nav.dev` | 開発者 | 開発者画面を開く |
| `nav.agent` | エージェント | AI (エージェント) 画面を開く |
| `nav.settings` | 設定 | 設定画面を開く |
| `nav.media` | メディア | メディア画面を開く |
| `nav.alarm` | アラーム | アラーム画面を開く |
| `nav.steps` | 歩数 | 歩数画面を開く |
| `nav.ota` | ファーム更新 | ファーム更新画面を開く |
| `memo.record` | メモ録音 | 音声メモの録音を開始 |
| `timer.start` | タイマー開始 | タイマーを開始 |
| `timer.stop` | タイマー停止 | タイマーを停止 |
| `stopwatch.toggle` | ストップウォッチ | ストップウォッチの開始/停止 |
| `counter.add` | カウンタ +1 | カウンタを +1 |
| `counter.sub` | カウンタ -1 | カウンタを -1 |

## EVT (CBOR map)
`{ "e": <event string>, "d": <data map> }`

event の一覧はこの表が唯一の正。

| event | data |
|---|---|
| `battery` | `{level, charging}` |
| `timer.finished` | `{}` |
| `memo.saved` | `{id, kind:"text"|"voice", sec:<voice秒>}` |
| `memo.deleted` | `{id}` |
| `memo.transcribe.request` | `{id}` 時計の「文字起こし」。電話は音声を取得して STT し、`memo.text.set` で返す (T31) |
| `media.cmd` | `{cmd:"play_pause"|"next"|"prev"|"vol_up"|"vol_down"}` (時計→スマホで音楽操作) |
| `alarm.ringing` | `{id}` (鳴動中のアラーム id) |
| `agent.request` | `{id, text}` AI の定型質問。id は返答の `agent.reply` の id と同じ |
| `ota.progress` | `{pct:<0-100>, stage:<str>}` OTA 進捗 (wifi/download/verify) |
| `ota.result` | `{ok:<bool>, msg:<str>}` OTA 終了 (ok=true なら直後に再起動) |
| `walk.query` | `{}` 歩く大会: 時計が今の進捗を要求する。スマホは REQ `walk.state` で返す (§歩く大会) |

## CBOR 正規形
両側の実装でバイト列を一致させるため、encode は次の正規形に従う。

- 整数・長さは最短形式で書く (definite-length のみ。indefinite-length、
  浮動小数点、タグは使わない)。
- map のキーは本書の各表に書いた順（定義順）で送る。REQ は `m`,`p`、
  RES は `ok`,`r` / `ok`,`e`,`msg`、EVT は `e`,`d` の順。
- `settings.set` の params や `settings.get` の結果のように可変の map は、
  settings keys 表の順が望ましい (要求キー指定時は要求した順)。
  表に無いキーを含む場合の順序は任意。
- 受信側は map のキー順に依存してはならない。

## テストベクタ
`docs/protocol-vectors/*.json` が encode/decode の共通期待値。
core (GoogleTest) と android (JUnit) の両方がこのファイルを読んで
結果一致を検査する。形式は `docs/protocol-vectors/README.md`、
再生成は `tools/gen_protocol_vectors.py`。

## BULK (双方向: Asset / OTA / 音声メモ / AI音声)
BULK_START payload (CBOR): `{id, kind:<str ≤15文字>, size, sha256:<bytes32>, chunk:<int>}`
kind は `"theme"|"asset"|"ota"|"memo"|"firmware"|"dictation"` (`"agent_audio"` は廃止。スマホ側は受信互換のみ残してよい)。
`"firmware"` はファーム更新
(Wi-Fi が使えないときの代替経路): 時計が /assets に一旦受け取り、
sha256 検証後に OTA パーティションへ書き込んで再起動する。
`"agent_audio"` は AI への質問の録音 (ADP1)。
BULK_CHUNK payload: `transfer_id:u16 | offset:u32 | bytes...`
BULK_ACK payload (CBOR): `{id, next:<offset>}`
BULK_END payload (CBOR): `{id}`

送信方向は2通り。どちらも「受信側が BULK_ACK を返す」のは同じ。

- Phone → Watch (kind `"theme"|"asset"|"ota"|"firmware"`): bulk char に write-without-response。
  Watch は8チャンクごとに `BULK_ACK{next}` を notify で返し、
  `BULK_END` 受信後は sha256 を検証して最終 `BULK_ACK{next=size}` を返す

### BULK kind="asset" (アルバムアート, T11)
`media.state.art` (u32、sha256 先頭4B LE) と対応する画像。
中身は LVGL bin: 12B ヘッダ `lv_image_header_t` LE
(`magic=0x19, cf=0x12(RGB565), flags=0, w=120, h=120, stride=240, reserved=0`)
+ RGB565 ピクセル 28,800B = 計 28,812B。
時計は PSRAM バッファ 1 枚に受け (flash には書かない)、完了で
MediaArtReceived を出して media 画面に出す。切断で破棄する。
同じ art id が時計に既にあれば送り直さない (電話側が送信済み id を記録)。
  (RES-on-ctrl でも同じ結果を返してよい)。
- Watch → Phone (kind `"memo"`): `memo.audio.get` の直後に時計が bulk char の
  notify で BULK_START→CHUNK→END を送る。Phone は同じく8チャンクごとに
  `BULK_ACK{next}` を bulk char へ write-without-response で返し、
  `BULK_END` 受信・sha256 一致後に最終 `BULK_ACK{next=size}` を返す。
  (Phone は ctrl に notify を送れないので RES ではなくこの ACK が完了の合図)
- Watch → Phone (kind `"agent_audio"`): **廃止** (旧 AI 音声経路)。現在は
  `kind="dictation"` を使う。スマホ側は旧アプリとの互換のため受信だけ残してよい。

### BULK kind="dictation" (リアルタイム文字起こし音声)
- Watch → Phone の push 型。`agent_audio` の置き換え。
- BULK_START: `{id:<talk id>, kind:"dictation", size:0, sha256:<32B ゼロ>, chunk:<232>}`。
  `size` は録音中なので不明 (0)。sha256 は使わない (ゼロ埋め)。
- BULK_CHUNK payload: `transfer_id:u16 | offset:u32 | bytes...`。
  bytes は **IMA ADPCM 4bit / 16kHz / モノラル** の連続ストリーム。
  ヘッダは無い。デコーダ状態はチャンクをまたいで連続し、`offset` は
  音声バイト列の先頭からの累積位置 (受信側はオフセット順に連結するだけ)。
- Phone は 8 チャンクごとに `BULK_ACK{next:<期待する次の offset>}` を返す。
  Watch は ACK を待たずに送ってよいが、未 ACK が 8KB を超えたら
  古い音声を捨てる (リアルタイム優先。取りこぼしは ASR が補間する)。
  捨てたとき Watch は ADPCM エンコーダを `reset()` してから続きを積む。
  Phone は **chunk の offset が期待値より飛んでいたら ADPCM デコーダを
  `reset()` してからデコードする** (数サンプルのクリックだけ許容)。
- **音声は保存しない**。時計は dictation 中にファイルを書かず、PSRAM の
  リングバッファ (16KB) を流すだけ。スマホも文字起こし後に PCM を破棄する。
  発話の自動終了は安全弁の 180 秒だけ (3 秒/30 秒では切らない)。
- 停止操作で `BULK_END{id}`。Phone は ASR に `endStream` を送り、
  確定テキストを `agent.partial{final:true}` で、応答を `agent.reply` で返す。
- 切断時は Watch が BULK_END を送れないことがある。Phone は
  5 秒 audio が来なくなったら ASR を自動確定し、`agent.reply` は
  接続が戻ったときだけ返す。

## 音声メモの文字起こし (T31)
時計単体では音声認識しない (ESP32-S3 のメモリでは現実的でない)。電話側の STT を使う。

1. 時計のメモ詳細で「文字起こし」→ EVT `memo.transcribe.request{id}`。
2. 電話は `memo.audio.get{id}` → BULK kind=`"memo"` で ADP1 を取得し、STT する。
   アプリのメモ画面の「文字起こし」ボタンからも同じ処理を呼べる。
3. 電話は `memo.text.set{id, text}` を送る。時計は音声メモの text に保存し、
   `memo.saved{id, kind:"voice", sec}` を返す。一覧・詳細に本文が出る。

text は UTF-8 で最大 256 バイト (時計の MemoEntry.text)。電話側で切り詰める。
本文は時計の NVS (`feat.memo` の音声メモスロット) に保存され、
電話側は音声を `cacheDir/memo_audio/memo_<id>.adp` にキャッシュするだけで、
本文の正は時計にある。

## 歩く大会 (walk)

決まったルートを歩く大会用。**測位・ルート・計算はスマホが正**で、時計は表示だけを
持つ (時計に GNSS は無い)。時計は画面を開いている間だけ問い合わせるので、
測位を時計側で動かし続けることはない。

1. 時計のウォーク画面を開く / 画面が Active のまま 10 秒経過 / 「いま更新」→
   EVT `walk.query {}`。
2. スマホは最新状態を組み立てて REQ `walk.state` を送る。params のキーは
   上表の順で送る。文字列は route ≤32B、next_cp ≤24B、msg ≤48B (UTF-8) に
   切り詰める。キーは省略可 (省略時 0 / 空文字、時計はキー順に依存しない)。
3. 時計は状態を保存して画面を更新し、RES `{ok:true, r:{}}` を返す。
4. `state` は `"idle"|"walking"|"finished"|"no_route"|"no_fix"`。未知の値は
   `"idle"` として扱う。`permille` は 0..1000、`eta_epoch`/`started_epoch`/
   `updated_epoch` は epoch 秒 (0 = 不明/未開始/省略)。`off_route_m` は生の逸脱距離で、
時計は 60m 以上のときだけ「ルートから外れています」を表示する。

スマホは `walk.query` を受けたら必ず 1 回 `walk.state` を返し、加えて
歩行の開始・終了・ルート外れなど表示が大きく変わったときに自発的に送ってよい。
時計は自発送信も同じに扱う。時計は Dim / ScreenOff / 別画面では `walk.query` を
送らない (電池優先)。設計の詳細と画面仕様は `docs/walk-event.md`。

## Agent (AI)
時計はオフライン。スマホが **Meta Muse Voice Transcribe (リアルタイム STT)** と
**Meta Muse (personal AI agent, useapi.net 経由)** へ中継する。
設計・セットアップは `docs/muse-integration.md`。

1. 「話しかける」を押す: 録音を開始し、**同時に** BULK kind=`"dictation"` で
   16kHz IMA ADPCM をリアルタイム送出する (下記 BULK 節)。要求 id は
   録音開始時に時計が採番する。
2. スマホは ADPCM を PCM 16kHz にデコードして Meta realtime ASR
   (`wss://api.meta.ai/v1/asr/realtime`) へ流し、部分結果を
   REQ `agent.partial{id, text}` で時計へ返す。時計は録音中にこれを表示する。
3. 録音停止 → 時計は BULK_END を送る。スマホは ASR を `endStream` して確定し、
   `agent.partial{id, text, final:true}` を送ってから Muse のチャットへ
   メッセージを POST する。
4. スマホは Muse の SSE 応答を読み、`agent.reply{id, text}` で時計に返す
   (v1 は最終応答のみ。途中経過のストリーミング表示は将来)。
5. 定型質問 (`agent.request`) もスマホ経由で Muse に送り、`agent.reply` で返す。
6. フォールバック: Meta API キー未設定ならスマホは OpenAI 互換
   `/audio/transcriptions` で発話単位の STT に切り替える (部分表示なし)。
   Muse トークン未設定なら電話は OpenAI 互換 `chat/completions` に切り替える
   (従来の AI 設定)。
7. 旧経路 BULK kind=`"agent_audio"` (録音完了後にまとめて送る) は廃止。
   時計とスマホを同時に更新する。スマホが dictation を ACK しない
   (旧アプリ) 場合、時計は録音を継続しつつ「スマホを更新してください」を
   表示して録音終了時にエラーにする。
