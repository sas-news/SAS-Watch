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
| `hello` | `{proto:1, app:"0.1.0", os:"android"}` | `{proto:1, fw:"0.1.0", caps:["timer","stopwatch","counter","memo","theme","audio","alarm","notify","media","wifi","ota","steps"]}` |
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
| `alarm.list` | `{}` | `{alarms:[{id:<int>, hour:<0-23>, min:<0-59>, dow:<曜日bit bit0=日..bit6=土, 0=毎日>, on:<bool>}]}` (最大5件) |
| `alarm.set` | `{hour, min}` + 省略可 `{id:<int>, dow:<int>, on:<bool>}` | `{id:<int>}` | `id` 省略/0 で新規 (満杯なら `busy`)、既存 id で更新 (`not_found`)。`dow` 0-0x7F 省略時 0、`on` 省略時 true |
| `alarm.delete` | `{id:<int>}` | `{}` | 無い id は `not_found` |
| `wifi.set` | `{ssid:<1-32文字>, pass:<0または8-63文字>}` | `{}` |
| `wifi.status` | `{}` | `{configured:<bool>, ssid:<str>}` |
| `ota.start` | `{url:<http(s)://〜 ≤255文字>, sha256:<bytes32>, version:<str>}` | `{}` |
| `ota.status` | `{}` | `{active:<bool>, stage:<str>, pct:<0-100>, msg:<str>, version:<str>}` |
| `steps.get` | `{}` | `{steps:<今日の歩数>, goal:<steps.goal の値>}` |

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
| `button.boot.double` | text | `memo.record` | BOOT 2回押しの Action 名 |
| `button.pwr.short` | text | `back` | PWR 短押しの Action 名 |
| `button.pwr.long` | text | `power_menu` | PWR 長押しの Action 名 |
| `button.pwr.double` | text | `none` | PWR 2回押しの Action 名 |
| `audio.volume` | u32 | 70 | クリック音・ビープ・メモ再生の音量 0-100 |
| `audio.click` | u32 | 1 | ボタンのクリック音 ON/OFF (0/1) |
| `notify.vibrate` | u32 | 1 | 通知受信時の振動 ON/OFF (0/1) |
| `raise_to_wake` | u32 | 1 | 腕を上げて画面オン (0/1) |
| `steps.goal` | u32 | 8000 | 歩数目標 (歩数画面の達成率・steps.get の goal) |

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
| `nav.agent` | エージェント | エージェント画面を開く (将来) |
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
| `media.cmd` | `{cmd:"play_pause"|"next"|"prev"|"vol_up"|"vol_down"}` (時計→スマホで音楽操作) |
| `alarm.ringing` | `{id}` (鳴動中のアラーム id) |
| `agent.request` | `{id, text}` (将来) |
| `ota.progress` | `{pct:<0-100>, stage:<str>}` OTA 進捗 (wifi/download/verify) |
| `ota.result` | `{ok:<bool>, msg:<str>}` OTA 終了 (ok=true なら直後に再起動) |

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

## BULK (双方向: Asset / OTA / 音声メモ)
BULK_START payload (CBOR): `{id, kind:<str ≤15文字>, size, sha256:<bytes32>, chunk:<int>}`
kind は `"theme"|"asset"|"ota"|"memo"|"firmware"`。`"firmware"` はファーム更新
(Wi-Fi が使えないときの代替経路): 時計が /assets に一旦受け取り、
sha256 検証後に OTA パーティションへ書き込んで再起動する。
BULK_CHUNK payload: `transfer_id:u16 | offset:u32 | bytes...`
BULK_ACK payload (CBOR): `{id, next:<offset>}`
BULK_END payload (CBOR): `{id}`

送信方向は2通り。どちらも「受信側が BULK_ACK を返す」のは同じ。

- Phone → Watch (kind `"theme"|"asset"|"ota"|"firmware"`): bulk char に write-without-response。
  Watch は8チャンクごとに `BULK_ACK{next}` を notify で返し、
  `BULK_END` 受信後は sha256 を検証して最終 `BULK_ACK{next=size}` を返す
  (RES-on-ctrl でも同じ結果を返してよい)。
- Watch → Phone (kind `"memo"`): `memo.audio.get` の直後に時計が bulk char の
  notify で BULK_START→CHUNK→END を送る。Phone は同じく8チャンクごとに
  `BULK_ACK{next}` を bulk char へ write-without-response で返し、
  `BULK_END` 受信・sha256 一致後に最終 `BULK_ACK{next=size}` を返す。
  (Phone は ctrl に notify を送れないので RES ではなくこの ACK が完了の合図)

切断後は送信側が `BULK_START` を同じ id で再送し、受信側は `BULK_ACK{next}`
で再開位置を返す。
`BULK_END` は送信側の「出し切った」の合図。受信側は sha256 を検証してから
最終 `BULK_ACK{next=size}` を返す。
