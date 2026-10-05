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
| `hello` | `{proto:1, app:"0.1.0", os:"android"}` | `{proto:1, fw:"0.1.0", caps:["timer","stopwatch","counter","memo","theme","audio","agent"]}` |
| `time.set` | `{epoch:<int s>, tz_offset_min:<int>}` (`tz_offset_min` は省略可) | `{}` |
| `device.info` | `{}` | `{battery:<0-100 または不明時 -1>, charging:<bool>, fw:<str>, free_heap:<int>, free_psram:<int>}` |
| `settings.get` | `{keys:[...]}` (省略・空なら全部) | `{<key>:<value>,...}` |
| `settings.set` | `{<key>:<value>,...}` | `{}` |
| `timer.start` | `{seconds:<int>}` (1 以上) | `{}` |
| `timer.stop` | `{}` | `{}` |
| `memo.create` | `{text:<str>}` (空でない) | `{id:<int>}` |
| `memo.list` | `{i:<開始index>, n:<最大件数 (1-16)>}` | `{total:<int>, memos:[{id, kind:"text"\|"voice", sec:<voice秒>, size:<voice byte>}]}` 新しい順 |
| `memo.get` | `{id:<int>}` | `{id, kind, sec, size, text:<text メモの本文>}` |
| `memo.delete` | `{id:<int>}` | `{}` |
| `memo.audio.get` | `{id:<int>}` | `{id, size, sha256:<bytes32>}` この直後に時計から BULK (kind=`"memo"`, id=メモid & 0xFFFF) が送られる |
| `notify.post` | `{app:<str>, title:<str>, body:<str>}` | `{}` |
| `media.state` | `{title:<str>, artist:<str>, playing:<bool>}` (`playing` は省略可) | `{}` |
| `agent.reply` | `{id:<int>, text:<str>}` | `{}` | AI の返答。id は直前の `agent.request` / BULK kind=`"agent"` の id と同じ。text は最大960バイト (UTF-8)。適用できない id でも `ok` |

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
| `button.boot.double` | text | `memo.record` | BOOT 2回押しの Action 名 |
| `button.pwr.short` | text | `back` | PWR 短押しの Action 名 |
| `button.pwr.long` | text | `power_menu` | PWR 長押しの Action 名 |
| `button.pwr.double` | text | `none` | PWR 2回押しの Action 名 |
| `audio.volume` | u32 | 70 | クリック音・ビープ・メモ再生の音量 0-100 |
| `audio.click` | u32 | 1 | ボタンのクリック音 ON/OFF (0/1) |
| `agent.q1` | text | `今日の予定は？` | AI の定型質問ボタン 1 (空 = 非表示、63バイトまで) |
| `agent.q2` | text | `今の天気は？` | AI の定型質問ボタン 2 (同左) |
| `agent.q3` | text | (空) | AI の定型質問ボタン 3 (同左) |

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
| `media.cmd` | `{cmd:"play_pause"|"next"|"prev"|"vol_up"|"vol_down"}` (時計→スマホで音楽操作) |
| `agent.request` | `{id, text}` AI の定型質問。id は返答の `agent.reply` の id と同じ |

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
BULK_START payload (CBOR): `{id, kind:"theme"|"asset"|"ota"|"memo"|"agent", size, sha256:<bytes32>, chunk:<int>}` (kind は7文字まで)
BULK_CHUNK payload: `transfer_id:u16 | offset:u32 | bytes...`
BULK_ACK payload (CBOR): `{id, next:<offset>}`
BULK_END payload (CBOR): `{id}`

送信方向は2通り。どちらも「受信側が BULK_ACK を返す」のは同じ。

- Phone → Watch (kind `"theme"|"asset"|"ota"`): bulk char に write-without-response。
  Watch は8チャンクごとに `BULK_ACK{next}` を notify で返し、
  `BULK_END` 受信後は sha256 を検証して最終 `BULK_ACK{next=size}` を返す
  (RES-on-ctrl でも同じ結果を返してよい)。
- Watch → Phone (kind `"memo"`): `memo.audio.get` の直後に時計が bulk char の
  notify で BULK_START→CHUNK→END を送る。Phone は同じく8チャンクごとに
  `BULK_ACK{next}` を bulk char へ write-without-response で返し、
  `BULK_END` 受信・sha256 一致後に最終 `BULK_ACK{next=size}` を返す。
  (Phone は ctrl に notify を送れないので RES ではなくこの ACK が完了の合図)
- Watch → Phone (kind `"agent"`): 「話しかける」の録音 (ADP1, 最大30秒)。
  kind `"memo"` と同じ手順だが REQ に紐付かない push 型で、id は AI の要求 id
  (時計側の連番)。返答は `agent.reply` REQ で同じ id とともに返す。

## Agent (AI)
時計はオフライン。スマホが OpenAI 互換 API に中継する。

1. 「話しかける」: 録音 (ADP1, 最大30秒) → BULK kind=`"agent"` id=<要求id>
   を push。スマホは STT → LLM に投げ、`agent.reply{id, text}` を返す。
2. 定型質問ボタン (settings `agent.q1..3`、空欄は非表示): EVT
   `agent.request{id, text}` を送り、同じく `agent.reply` を待つ。

要求 id は時計側の連番 (BULK transfer id と同じ値)。返答は `agent.reply` REQ で、
時計は「録音中→送信中→考え中→返答」の状態を表示する。送信開始から60秒で
タイムアウト (エラー表示)。返答 text は最大960バイト。

切断後は送信側が `BULK_START` を同じ id で再送し、受信側は `BULK_ACK{next}`
で再開位置を返す。
`BULK_END` は送信側の「出し切った」の合図。受信側は sha256 を検証してから
最終 `BULK_ACK{next=size}` を返す。
