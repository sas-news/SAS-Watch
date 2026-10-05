# protocol-vectors

時計 (core, C++) とスマホ (android/protocol, Kotlin) が共通で使う
プロトコルのテストベクタ。`tools/gen_protocol_vectors.py` が生成する
独立実装の出力で、両側のテストがこの JSON を読んで
encode/decode の結果一致を検査する。

## ファイル

| file | kind | 内容 |
|---|---|---|
| `cbor.json` | `"cbor"` | CBOR 各型の encode/decode (正規形) |
| `frame.json` | `"frame"` | Frame の encode/decode、フラグメント分割、壊れたフレーム |
| `messages.json` | `"frame"` | REQ/RES/EVT/BULK の実メッセージをフレームまで |

## JSON の形

`{"version": 1, "kind": <kind>, "cases": [...]}`

### kind == "cbor"

```
{ "name": ..., "description": ..., "value": <JSON>, "hex": "<cbor hex>" }
```

- encode: `value` を CBOR 化すると `hex` になること
- decode: `hex` を CBOR 復号すると `value` と一致すること

### kind == "frame"

```
{ "name": ..., "description": ...,
  "type": "REQ"|"RES"|"EVT"|"BULK_START"|"BULK_CHUNK"|"BULK_ACK"|"BULK_END",
  "msg_id": <int>, "mtu": <int, 省略時 247>,
  "payload_hex": "<payload の hex>",
  "frames": ["<1フレーム目の hex>", ...] }
```

- encode: `payload_hex` を `mtu` でフラグメント化したフレーム列が
  `frames` と一致すること
- decode: `frames` の各 hex をデコード・再構成すると
  `payload_hex` に戻ること

エラーケース:

```
{ "name": ..., "description": ..., "expect_error": <kind>, "raw_hex": "..." }
```

`expect_error` は `too_short` / `bad_version` / `bad_length` / `bad_crc`。
`raw_hex` をデコードしようとして該当の失敗になること。

## JSON → CBOR 値の対応

- object → map。**object のキー順がそのまま map のキー順**
  (正規形は「定義順」なので、パーサはキー順を保持すること)。
  例外として1キーだけの `{"$bytes": "<hex>"}` は byte string。
- array → array、string → text、整数 → int、true/false → bool、null → null。
- 整数は JSON の範囲内 (約 ±9e15) に収める。
