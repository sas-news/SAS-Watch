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

| method | params | result |
|---|---|---|
| `hello` | `{proto:1, app:"0.1.0", os:"android"}` | `{proto:1, fw:"0.1.0", caps:["timer","stopwatch","counter","memo","theme"]}` |
| `time.set` | `{epoch:<int s>, tz_offset_min:<int>}` | `{}` |
| `device.info` | `{}` | `{battery:<0-100>, charging:<bool>, fw:<str>, free_heap:<int>, free_psram:<int>}` |
| `settings.get` | `{keys:[...]}` (空なら全部) | `{<key>:<value>,...}` |
| `settings.set` | `{<key>:<value>,...}` | `{}` |
| `timer.start` | `{seconds:<int>}` | `{}` |
| `timer.stop` | `{}` | `{}` |
| `memo.create` | `{text:<str>}` | `{id:<int>}` |
| `notify.post` | `{app:<str>, title:<str>, body:<str>}` | `{}` |
| `media.state` | `{title:<str>, artist:<str>, playing:<bool>}` | `{}` |

error code: `bad_request`, `unknown_method`, `unsupported_proto`, `busy`, `internal`。

### settings keys
`brightness` (0-100), `dim_after_s`, `screen_off_after_s`, `button.boot.short`, `button.boot.long`, `button.boot.double`, `button.pwr.short`, `button.pwr.long` (値は Action 名文字列), `theme` (theme id 文字列)

## EVT (CBOR map)
`{ "e": <event string>, "d": <data map> }`

| event | data |
|---|---|
| `battery` | `{level, charging}` |
| `timer.finished` | `{}` |
| `memo.saved` | `{id}` |
| `media.cmd` | `{cmd:"play_pause"|"next"|"prev"|"vol_up"|"vol_down"}` (時計→スマホで音楽操作) |
| `agent.request` | `{id, text}` (将来) |

## BULK (Asset / OTA)
BULK_START payload (CBOR): `{id, kind:"theme"|"asset"|"ota", size, sha256:<bytes32>, chunk:<int>}`
BULK_CHUNK payload: `transfer_id:u16 | offset:u32 | bytes...`
BULK_ACK payload (CBOR): `{id, next:<offset>}`（8チャンクごと、または再開時）
BULK_END payload (CBOR): `{id}` → Watch は sha256 を検証し RES で返す。
切断後は Phone が `BULK_START` を同じ id で再送し、Watch は `BULK_ACK{next}` で再開位置を返す。
