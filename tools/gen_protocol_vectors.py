#!/usr/bin/env python3
"""docs/protocol-vectors/*.json を生成する参照実装。

protocol-v1.md の Frame / CRC-16/CCITT-FALSE / CBOR / フラグメントを
独立に実装し、core (C++) と android (Kotlin) の両方が読む
共通テストベクタを出力する。再生成:

    python3 tools/gen_protocol_vectors.py

JSON の object は「キー順がそのまま CBOR map のキー順」。
byte string は {"$bytes": "<hex>"} の1キー object で表す。
"""

import json
import os

OUT_DIR = os.path.join(os.path.dirname(__file__), "..", "docs", "protocol-vectors")

# ---------------------------------------------------------------- CBOR (canonical)


def cbor_head(major: int, arg: int) -> bytes:
    mt = major << 5
    if arg < 24:
        return bytes([mt | arg])
    if arg <= 0xFF:
        return bytes([mt | 24, arg])
    if arg <= 0xFFFF:
        return bytes([mt | 25, arg >> 8, arg & 0xFF])
    if arg <= 0xFFFFFFFF:
        return bytes([mt | 26]) + arg.to_bytes(4, "big")
    return bytes([mt | 27]) + arg.to_bytes(8, "big")


def cbor_encode(v) -> bytes:
    """JSON モデル → canonical CBOR。dict は挿入順で map 化。"""
    if v is None:
        return bytes([0xF6])
    if v is True:
        return bytes([0xF5])
    if v is False:
        return bytes([0xF4])
    if isinstance(v, int):
        if v >= 0:
            return cbor_head(0, v)
        return cbor_head(1, -1 - v)
    if isinstance(v, str):
        b = v.encode("utf-8")
        return cbor_head(3, len(b)) + b
    if isinstance(v, list):
        return cbor_head(4, len(v)) + b"".join(cbor_encode(x) for x in v)
    if isinstance(v, dict):
        if set(v.keys()) == {"$bytes"}:
            b = bytes.fromhex(v["$bytes"])
            return cbor_head(2, len(b)) + b
        return cbor_head(5, len(v)) + b"".join(
            cbor_encode(k) + cbor_encode(x) for k, x in v.items()
        )
    raise TypeError(f"unsupported value: {v!r}")


# ---------------------------------------------------------------- CRC16


def crc16_ccitt_false(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


# ---------------------------------------------------------------- Frame

TYPES = {
    "REQ": 0x01,
    "RES": 0x02,
    "EVT": 0x03,
    "BULK_START": 0x10,
    "BULK_CHUNK": 0x11,
    "BULK_ACK": 0x12,
    "BULK_END": 0x13,
}

HEADER = 8
CRC = 2


def frame_encode(ftype: int, more: bool, seq: int, msg_id: int, payload: bytes) -> bytes:
    out = bytearray()
    out.append(1)  # ver
    out.append(ftype)
    out.append(1 if more else 0)
    out.append(seq & 0xFF)
    out += msg_id.to_bytes(2, "little")
    out += len(payload).to_bytes(2, "little")
    out += payload
    out += crc16_ccitt_false(bytes(out)).to_bytes(2, "little")
    return bytes(out)


def fragment(ftype: int, msg_id: int, payload: bytes, mtu: int) -> list:
    """payload を MTU に収まるフレーム列にする (1フレーム最大長 = MTU - 3)。"""
    max_frame = mtu - 3
    chunk = max_frame - HEADER - CRC
    frames = []
    off = 0
    seq = 0
    while True:
        take = min(chunk, len(payload) - off)
        more = off + take < len(payload)
        frames.append(frame_encode(ftype, more, seq, msg_id, payload[off : off + take]))
        off += take
        seq += 1
        if off >= len(payload):
            return frames


def hx(b: bytes) -> str:
    return b.hex()


# ---------------------------------------------------------------- cases


def frame_case(name, desc, ftype, msg_id, payload, mtu=247):
    """payload: bytes か JSON モデル。frames に全フラグメントの hex を入れる。"""
    if not isinstance(payload, (bytes, bytearray)):
        payload = cbor_encode(payload)
    payload = bytes(payload)
    frames = fragment(TYPES[ftype], msg_id, payload, mtu)
    case = {
        "name": name,
        "description": desc,
        "type": ftype,
        "msg_id": msg_id,
        "payload_hex": hx(payload),
        "frames": [hx(f) for f in frames],
    }
    if mtu != 247:
        case["mtu"] = mtu
    return case


def error_case(name, desc, raw, expect_error):
    return {
        "name": name,
        "description": desc,
        "expect_error": expect_error,
        "raw_hex": hx(raw),
    }


def main():
    os.makedirs(OUT_DIR, exist_ok=True)

    # ---------------------------------------------------------- cbor.json
    cbor_cases = [
        {"name": "uint_small", "description": "0..23 の正の整数は1バイト", "value": v, "hex": hx(cbor_encode(v))}
        for v in (0, 1, 23)
    ]
    cbor_cases += [
        {
            "name": f"uint_{v}",
            "description": f"正の整数 {v} の最短形式",
            "value": v,
            "hex": hx(cbor_encode(v)),
        }
        for v in (24, 255, 256, 65535, 65536, 4_294_967_295, 4_294_967_296)
    ]
    cbor_cases += [
        {
            "name": f"nint_{abs(v)}",
            "description": f"負の整数 {v} の最短形式",
            "value": v,
            "hex": hx(cbor_encode(v)),
        }
        for v in (-1, -24, -25, -255, -256, -1000, -65537)
    ]
    cbor_cases += [
        {"name": "bool_true", "description": "true は 0xF5", "value": True, "hex": hx(cbor_encode(True))},
        {"name": "bool_false", "description": "false は 0xF4", "value": False, "hex": hx(cbor_encode(False))},
        {"name": "null", "description": "null は 0xF6", "value": None, "hex": hx(cbor_encode(None))},
        {
            "name": "text_ascii",
            "description": "ASCII テキスト",
            "value": "hello",
            "hex": hx(cbor_encode("hello")),
        },
        {
            "name": "text_ja",
            "description": "日本語 UTF-8 テキスト",
            "value": "通知テスト",
            "hex": hx(cbor_encode("通知テスト")),
        },
        {
            "name": "text_empty",
            "description": "空文字列",
            "value": "",
            "hex": hx(cbor_encode("")),
        },
        {
            "name": "bytes",
            "description": "byte string (sha256 等)",
            "value": {"$bytes": "000102030405060708090a0b0c0d0e0f"},
            "hex": hx(cbor_encode({"$bytes": "000102030405060708090a0b0c0d0e0f"})),
        },
        {
            "name": "array",
            "description": "配列 (混在型)",
            "value": [1, "two", True, None],
            "hex": hx(cbor_encode([1, "two", True, None])),
        },
        {
            "name": "map_simple",
            "description": "フラットな map (キーは定義順)",
            "value": {"m": "hello", "p": {"proto": 1}},
            "hex": hx(cbor_encode({"m": "hello", "p": {"proto": 1}})),
        },
        {
            "name": "map_nested",
            "description": "ネストした map/array/bytes",
            "value": {
                "id": 7,
                "kind": "theme",
                "sha256": {"$bytes": "aa" * 32},
                "meta": {"tags": ["a", "b"], "enabled": False},
            },
            "hex": hx(
                cbor_encode(
                    {
                        "id": 7,
                        "kind": "theme",
                        "sha256": {"$bytes": "aa" * 32},
                        "meta": {"tags": ["a", "b"], "enabled": False},
                    }
                )
            ),
        },
        {
            "name": "map_empty",
            "description": "空 map",
            "value": {},
            "hex": hx(cbor_encode({})),
        },
    ]
    write("cbor.json", {"version": 1, "kind": "cbor", "cases": cbor_cases})

    # ---------------------------------------------------------- frame.json
    frame_cases = [
        frame_case(
            "req_small",
            "小さい REQ は1フレーム",
            "REQ",
            1,
            {"m": "timer.stop", "p": {}},
        ),
        frame_case(
            "res_small",
            "RES も同じ形式 (type=0x02)",
            "RES",
            1,
            {"ok": True, "r": {}},
        ),
        frame_case(
            "evt_battery",
            "EVT は type=0x03、msg_id は送信側の連番",
            "EVT",
            41,
            {"e": "battery", "d": {"level": 87, "charging": False}},
        ),
        frame_case(
            "fragmented_res",
            "MTU 100 に収まらない payload はフラグメント分割 (flags bit0=more, seq 連番)",
            "RES",
            0x1234,
            bytes(range(200)),
            mtu=100,
        ),
        frame_case(
            "bulk_chunk_raw",
            "BULK_CHUNK の payload は生バイト (id:u16 + offset:u32 + data)",
            "BULK_CHUNK",
            5,
            (7).to_bytes(2, "little") + (0).to_bytes(4, "little") + bytes(range(16)),
        ),
        # デコード専用のエラーケース
        error_case(
            "err_too_short",
            "ヘッダ+CRC に満たないフレームは拒否",
            bytes([1, 2, 3]),
            "too_short",
        ),
    ]
    bad_ver = bytearray(frame_encode(0x01, False, 0, 1, b""))
    bad_ver[0] = 2
    # CRC も再計算して「バージョンだけ違う」フレームにする
    body = bytes(bad_ver[:-2])
    bad_ver[-2:] = crc16_ccitt_false(body).to_bytes(2, "little")
    frame_cases.append(
        error_case("err_bad_version", "ver != 1 は拒否 (CRC が合っていても)", bytes(bad_ver), "bad_version")
    )
    bad_len = bytearray(frame_encode(0x01, False, 0, 1, b"\xa0"))
    bad_len[7] = bad_len[7] + 1  # len を 2 に水増し
    bad_len[-2:] = crc16_ccitt_false(bytes(bad_len[:-2])).to_bytes(2, "little")
    frame_cases.append(
        error_case("err_bad_length", "len と実サイズが合わないフレームは拒否", bytes(bad_len), "bad_length")
    )
    bad_crc = bytearray(frame_encode(0x02, False, 0, 7, b"\x01\x02\x03"))
    bad_crc[-1] ^= 0xFF
    frame_cases.append(
        error_case("err_bad_crc", "CRC-16 が一致しないフレームは拒否", bytes(bad_crc), "bad_crc")
    )
    write("frame.json", {"version": 1, "kind": "frame", "cases": frame_cases})

    # ---------------------------------------------------------- messages.json
    messages = [
        frame_case(
            "req_hello",
            "hello REQ: {m, p:{proto, app, os}}",
            "REQ",
            1,
            {"m": "hello", "p": {"proto": 1, "app": "0.1.0", "os": "android"}},
        ),
        frame_case(
            "req_time_set",
            "time.set REQ (JST = +540min)",
            "REQ",
            2,
            {"m": "time.set", "p": {"epoch": 1_751_180_000, "tz_offset_min": 540}},
        ),
        frame_case(
            "req_settings_set",
            "settings.set REQ (明るさとボタン割り当て)",
            "REQ",
            3,
            {
                "m": "settings.set",
                "p": {"brightness": 80, "button.pwr.long": "power_menu"},
            },
        ),
        frame_case(
            "req_notify_post_ja",
            "notify.post REQ (日本語 UTF-8 を含む)",
            "REQ",
            4,
            {
                "m": "notify.post",
                "p": {
                    "app": "jp.co.sony.himedia",
                    "title": "新着メッセージ",
                    "body": "こんにちは！返事ください。",
                },
            },
        ),
        frame_case(
            "req_media_state",
            "media.state REQ (日本語タイトル)",
            "REQ",
            5,
            {
                "m": "media.state",
                "p": {"title": "時をかける少女", "artist": "椎名林檎", "playing": True},
            },
        ),
        frame_case(
            "res_ok",
            "RES 成功形 {ok:true, r:...}",
            "RES",
            1,
            {"ok": True, "r": {"proto": 1, "fw": "0.1.0", "caps": ["timer", "stopwatch", "counter", "memo", "theme", "audio", "agent"]}},
        ),
        frame_case(
            "res_error",
            "RES 失敗形 {ok:false, e:code, msg:str}",
            "RES",
            9,
            {"ok": False, "e": "unknown_method", "msg": "unknown method"},
        ),
        frame_case(
            "evt_media_cmd",
            "media.cmd EVT (時計→スマホの再生/停止)",
            "EVT",
            12,
            {"e": "media.cmd", "d": {"cmd": "play_pause"}},
        ),
        frame_case(
            "evt_memo_saved",
            "memo.saved EVT (音声メモ)",
            "EVT",
            13,
            {"e": "memo.saved", "d": {"id": 1, "kind": "voice", "sec": 8}},
        ),
        frame_case(
            "evt_memo_deleted",
            "memo.deleted EVT",
            "EVT",
            14,
            {"e": "memo.deleted", "d": {"id": 1}},
        ),
        frame_case(
            "req_memo_list",
            "memo.list REQ",
            "REQ",
            15,
            {"m": "memo.list", "p": {"i": 0, "n": 16}},
        ),
        frame_case(
            "req_memo_audio_get",
            "memo.audio.get REQ (音声メモ取得。直後に時計から BULK kind=\"memo\")",
            "REQ",
            16,
            {"m": "memo.audio.get", "p": {"id": 2}},
        ),
        frame_case(
            "res_memo_list",
            "memo.list RES (新しい順、text/voice 混在)",
            "RES",
            15,
            {
                "ok": True,
                "r": {
                    "total": 2,
                    "memos": [
                        {"id": 2, "kind": "voice", "sec": 8, "size": 64016},
                        {"id": 1, "kind": "text", "sec": 0, "size": 0},
                    ],
                },
            },
        ),
        frame_case(
            "req_notify_post_fragmented",
            "長い本文の notify.post は MTU 100 でフラグメント化される",
            "REQ",
            6,
            {
                "m": "notify.post",
                "p": {
                    "app": "com.example.mail",
                    "title": "長文テスト",
                    "body": "あいうえおかきくけこさしすせそたちつてと" * 5,
                },
            },
            mtu=100,
        ),
        frame_case(
            "bulk_start",
            "BULK_START payload {id, kind, size, sha256(bytes), chunk}",
            "BULK_START",
            20,
            {
                "id": 1,
                "kind": "theme",
                "size": 4096,
                "sha256": {"$bytes": "00" * 16 + "11" * 16},
                "chunk": 1024,
            },
        ),
        frame_case(
            "bulk_ack",
            "BULK_ACK payload {id, next} (再開位置通知)",
            "BULK_ACK",
            1,
            {"id": 1, "next": 1024},
        ),
        frame_case(
            "bulk_end",
            "BULK_END payload {id}",
            "BULK_END",
            1,
            {"id": 1},
        ),
        frame_case(
            "req_agent_reply",
            "agent.reply REQ (AI の返答。id は agent.request/BULK agent と同じ)",
            "REQ",
            30,
            {"m": "agent.reply", "p": {"id": 7, "text": "今日は晴れです。"}},
        ),
        frame_case(
            "evt_agent_request",
            "agent.request EVT (定型質問。時計→スマホ)",
            "EVT",
            31,
            {"e": "agent.request", "d": {"id": 7, "text": "今日の予定は？"}},
        ),
        frame_case(
            "res_settings_get",
            "settings.get RES (全キー、settings keys 表の順)",
            "RES",
            21,
            {
                "ok": True,
                "r": {
                    "brightness": 50,
                    "dim_after_s": 8,
                    "screen_off_after_s": 12,
                    "deep_sleep_after_s": 1800,
                    "tz_offset_min": 0,
                    "theme": "standard",
                    "button.boot.short": "primary",
                    "button.boot.long": "nav.dev",
                    "button.boot.double": "memo.record",
                    "button.pwr.short": "back",
                    "button.pwr.long": "power_menu",
                    "button.pwr.double": "none",
                    "audio.volume": 70,
                    "audio.click": 1,
                    "agent.q1": "今日の予定は？",
                    "agent.q2": "今の天気は？",
                    "agent.q3": "",
                },
            },
        ),
    ]
    write("messages.json", {"version": 1, "kind": "frame", "cases": messages})


def write(name, obj):
    path = os.path.join(OUT_DIR, name)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(obj, f, ensure_ascii=False, indent=2)
        f.write("\n")
    print(f"wrote {path} ({len(obj['cases'])} cases)")


if __name__ == "__main__":
    main()
