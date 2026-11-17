#!/usr/bin/env python3
"""Meta Model API (Muse Voice Transcribe) のスモークテスト (SAS-Watch / T40)。

※ 2026-11-17 方針変更: これは有料構成 (Meta Model API / useapi.net) 用のツール。
   現在の無料構成 (スマホ内 Vosk + self-hosted muse2api) では使いません。
   無料構成の疎通は docs/muse-setup.md §5 の curl を参照。

環境変数:
  META_API_KEY   Meta Model API キー (LLM_...)。未設定ならドライラン。
                 任意で MODEL_API_KEY も受け付ける (Meta のサンプルと同じ名前)。

使い方:
  # 鍵なし: WAV の形式検証とリクエスト形状の表示だけ (実通信なし)
  python3 tools/muse_smoke/asr_smoke.py recording.wav

  # ファイル一括文字起こし (POST /v1/asr/transcribe)
  META_API_KEY='LLM_...' python3 tools/muse_smoke/asr_smoke.py recording.wav

  # リアルタイム (wss /asr/realtime)。websockets パッケージが必要
  META_API_KEY='LLM_...' python3 tools/muse_smoke/asr_smoke.py recording.wav --realtime

  # 通信なしの自己テスト
  python3 tools/muse_smoke/asr_smoke.py --self-test

音声は mono / 16-bit PCM / 16 kHz または 24 kHz の WAV のみ。
別形式は ffmpeg で変換する:
  ffmpeg -i input.m4a -ac 1 -ar 24000 -c:a pcm_s16le -map_metadata -1 recording.wav

鍵は環境変数だけで渡す (ファイル・シェル履歴・リポジトリに書かない)。
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import time
import urllib.error
import urllib.request
import uuid

API_BASE = "https://api.meta.ai"
TRANSCRIBE_URL = f"{API_BASE}/v1/asr/transcribe"
REALTIME_URL = "wss://api.meta.ai/v1/asr/realtime"
MODEL = "muse-voice-transcribe-1.0"
USER_AGENT = "sas-watch-asr-smoke/1.0"
MAX_AUDIO_SECONDS = 600  # 10 分 (サーバ側の上限)
MAX_BODY_BYTES = 32 * 1024 * 1024  # 32 MB (サーバ側の上限)
FRAME_MS = 80  # 実時間ペースで送る単位

HTTP_HINTS = {
    400: "WAV 形式 (mono/16-bit PCM/16k or 24k) か、10 分超でないかを確認する。",
    401: "API キーが不正。LLM_... 全体を META_API_KEY に入れ直す。",
    406: "Accept ヘッダが未対応。このスクリプトは application/json を送っている。",
    413: "リクエストが 32MB 超。音声を分割する。",
    429: "レート/同時実行の上限。指数バックオフで再試行する。",
    500: "サーバ側の処理失敗。一度だけ再試行し、直らなければ sessionId を添えて support へ。",
}

WS_CLOSE_HINTS = {
    1000: "正常終了。final の文字起こしを確認する。",
    1008: "不正リクエスト、またはバックログ/実時間より遅い送信。"
    "5 秒以上先読みしない・実時間ペースで送る・handshake を 10 秒以内に送る、を確認する。",
    1011: "サーバ内部エラー。バックオフして再接続する。"
    "'Max session duration reached' なら 60 分上限なので新しいセッションにする。",
    1013: "レート制限 (同時ストリーム/開始回数)。待ってから再接続する。",
}


def redact(text: object) -> str:
    s = str(text)
    s = re.sub(r"LLM_[0-9]+_[A-Za-z0-9._~+/=-]+", "LLM_****", s)
    s = re.sub(r"Bearer\s+[A-Za-z0-9._~+/=:-]{8,}", "Bearer ****", s)
    return s


def mask(value: str | None) -> str:
    """メールなど、秘密でない識別子用。先頭と末尾だけ見せる。"""
    if not value:
        return "(未設定)"
    return value[:5] + "…" + value[-4:] if len(value) > 8 else "****"


def mask_secret(value: str | None) -> str:
    """API キー用。種類と長さだけ見せ、値は出さない。"""
    if not value:
        return "(未設定)"
    prefix = "LLM_" if value.startswith("LLM_") else ""
    return f"{prefix}**** ({len(value)} 文字)"


def eprint(*args: object) -> None:
    print(*args, file=sys.stderr)


# ---------------------------------------------------------------------------
# WAV
# ---------------------------------------------------------------------------


class WavError(ValueError):
    pass


def parse_wav_bytes(raw: bytes, name: str = "audio.wav") -> dict:
    """RIFF/WAVE を読み、Muse Voice Transcribe が受け付ける形式か検証する。"""
    if len(raw) < 44 or raw[:4] != b"RIFF" or raw[8:12] != b"WAVE":
        raise WavError(f"{name}: RIFF/WAVE ではありません。WAV ファイルを指定してください。")
    pos = 12
    fmt: dict | None = None
    data_offset = data_len = None
    while pos + 8 <= len(raw):
        chunk_id = raw[pos : pos + 4]
        size = int.from_bytes(raw[pos + 4 : pos + 8], "little")
        body_start = pos + 8
        body = raw[body_start : body_start + size]
        if chunk_id == b"fmt ":
            if len(body) < 16:
                raise WavError(f"{name}: fmt チャンクが短すぎます。")
            fmt = {
                "audio_format": int.from_bytes(body[0:2], "little"),
                "channels": int.from_bytes(body[2:4], "little"),
                "sample_rate": int.from_bytes(body[4:8], "little"),
                "byte_rate": int.from_bytes(body[8:12], "little"),
                "block_align": int.from_bytes(body[12:14], "little"),
                "bits_per_sample": int.from_bytes(body[14:16], "little"),
            }
        elif chunk_id == b"data":
            data_offset = body_start
            data_len = min(size, max(0, len(raw) - body_start))
            break
        pos = body_start + size + (size & 1)

    if fmt is None:
        raise WavError(f"{name}: fmt チャンクがありません。")
    if data_offset is None or data_len is None:
        raise WavError(f"{name}: data チャンクがありません。")
    if fmt["audio_format"] != 1:
        raise WavError(
            f"{name}: PCM ではありません (audio_format={fmt['audio_format']})。"
            "`-c:a pcm_s16le` で変換してください。"
        )
    if fmt["channels"] != 1:
        raise WavError(f"{name}: モノラルではありません (channels={fmt['channels']})。`-ac 1`。")
    if fmt["bits_per_sample"] != 16:
        raise WavError(
            f"{name}: 16-bit PCM ではありません (bits={fmt['bits_per_sample']})。`-c:a pcm_s16le`。"
        )
    if fmt["sample_rate"] not in (16000, 24000):
        raise WavError(
            f"{name}: サンプルレート {fmt['sample_rate']} Hz は非対応 (16k か 24k)。`-ar 24000`。"
        )
    seconds = data_len / (fmt["sample_rate"] * fmt["channels"] * 2)
    fmt.update(
        {
            "data_offset": data_offset,
            "data_len": data_len,
            "seconds": seconds,
            "encoding": "PCM_16KHZ" if fmt["sample_rate"] == 16000 else "PCM_24KHZ",
        }
    )
    return fmt


def load_wav(path: str) -> tuple[bytes, dict]:
    try:
        with open(path, "rb") as fh:
            raw = fh.read()
    except OSError as exc:
        raise SystemExit(f"WAV を読めません: {exc}") from exc
    info = parse_wav_bytes(raw, os.path.basename(path))
    return raw, info


def load_wav_checked(path: str) -> tuple[bytes, dict]:
    try:
        return load_wav(path)
    except WavError as exc:
        raise SystemExit(f"WAV を検証できません: {exc}") from exc


def make_test_wav(seconds: float = 0.2, rate: int = 16000) -> bytes:
    """自己テスト用の無音 WAV をその場で作る (ファイルには残さない)。"""
    frames = int(rate * seconds)
    data = b"\x00\x00" * frames
    header = (
        b"RIFF"
        + (36 + len(data)).to_bytes(4, "little")
        + b"WAVE"
        + b"fmt "
        + (16).to_bytes(4, "little")
        + (1).to_bytes(2, "little")
        + (1).to_bytes(2, "little")
        + rate.to_bytes(4, "little")
        + (rate * 2).to_bytes(4, "little")
        + (2).to_bytes(2, "little")
        + (16).to_bytes(2, "little")
        + b"data"
        + len(data).to_bytes(4, "little")
        + data
    )
    return header


# ---------------------------------------------------------------------------
# リクエスト
# ---------------------------------------------------------------------------


def transcribe_request(language_bias: list[str], keywords: list[str], mode: str) -> dict:
    body: dict[str, object] = {
        "mode": mode,
        "model": MODEL,
        "audioEncoding": "WAV",
    }
    if language_bias:
        body["languageBias"] = language_bias
    if keywords:
        body["keywords"] = keywords
    return body


def multipart_body(request_json: dict, wav: bytes, boundary: str, filename: str) -> bytes:
    crlf = b"\r\n"
    parts = [
        b"--" + boundary.encode(),
        b'Content-Disposition: form-data; name="request"',
        b"Content-Type: application/json",
        b"",
        json.dumps(request_json, ensure_ascii=False).encode("utf-8"),
        b"--" + boundary.encode(),
        b'Content-Disposition: form-data; name="audio"; filename="' + filename.encode() + b'"',
        b"Content-Type: audio/wav",
        b"",
        wav,
        b"--" + boundary.encode() + b"--",
        b"",
    ]
    return crlf.join(parts)


def post_transcribe(key: str, wav: bytes, info: dict, req: dict, timeout: float) -> int:
    boundary = "----SasWatchSmoke" + uuid.uuid4().hex
    body = multipart_body(req, wav, boundary, "smoke.wav")
    session_id = f"sas-watch-smoke-{uuid.uuid4()}"
    url = f"{TRANSCRIBE_URL}?sessionId={session_id}"
    print(f"[transcribe] POST {TRANSCRIBE_URL}  (sessionId={session_id})")
    print(
        f"[transcribe] audio: {info['seconds']:.2f} 秒 / {info['sample_rate']} Hz / "
        f"mono / 16-bit / {len(wav) / 1024:.0f} KiB"
    )
    req_http = urllib.request.Request(
        url,
        data=body,
        method="POST",
        headers={
            "Authorization": f"Bearer {key}",
            "Content-Type": f"multipart/form-data; boundary={boundary}",
            "Accept": "application/json",
            "User-Agent": USER_AGENT,
        },
    )
    started = time.monotonic()
    try:
        with urllib.request.urlopen(req_http, timeout=timeout) as res:
            raw = res.read().decode("utf-8", "replace")
            status = res.status
    except urllib.error.HTTPError as exc:
        raw = exc.read().decode("utf-8", "replace")
        print_http_error(exc.code, raw)
        return 1
    except urllib.error.URLError as exc:
        raise SystemExit(f"接続に失敗しました: {exc.reason}") from exc

    elapsed = time.monotonic() - started
    print(f"[transcribe] HTTP {status} / {elapsed:.1f} 秒")
    try:
        payload = json.loads(raw)
    except json.JSONDecodeError:
        print(f"応答が JSON ではありません: {redact(raw[:500])}")
        return 1
    print(f"[transcribe] sessionId = {payload.get('sessionId')}")
    print(f"[transcribe] audioDurationMs = {payload.get('audioDurationMs')}")
    print(f"[transcribe] transcript = {redact(payload.get('transcript', ''))}")
    for turn in payload.get("turns") or []:
        print(f"  turn {turn.get('turnId')}: {redact(turn.get('transcript', ''))}")
    return 0 if payload.get("transcript") is not None else 1


def print_http_error(code: int, raw: str) -> None:
    hint = HTTP_HINTS.get(code, "未知のエラー。レスポンス本文を確認する。")
    print(f"HTTP {code}: {hint}")
    try:
        payload = json.loads(raw)
        if payload.get("error"):
            print(f"  server: {redact(payload['error'])}")
    except json.JSONDecodeError:
        if raw.strip():
            print(f"  server: {redact(raw[:400])}")


# ---------------------------------------------------------------------------
# リアルタイム
# ---------------------------------------------------------------------------


async def run_realtime(key: str, wav: bytes, info: dict, req: dict, timeout: float) -> int:
    try:
        import websockets  # type: ignore
    except ImportError:
        eprint(
            "websockets パッケージがありません。\n"
            "  python3 -m pip install websockets\n"
            "または --realtime を付けずにファイル一括モードを使ってください。"
        )
        return 1

    encoding = info["encoding"]
    sample_rate = info["sample_rate"]
    frame_bytes = sample_rate * 2 * FRAME_MS // 1000
    session_id = f"sas-watch-smoke-{uuid.uuid4()}"
    uri = f"{REALTIME_URL}?sessionId={session_id}"
    handshake = {
        "authorization": {"accessToken": f"Bearer {key}"},
        "audioEncoding": encoding,
        "model": MODEL,
        "mode": req["mode"],
        "partialMode": "CUMULATIVE",
        "emitAudioProgress": False,
    }
    if req.get("languageBias"):
        handshake["languageBias"] = req["languageBias"]
    if req.get("keywords"):
        handshake["keywords"] = req["keywords"]

    print(f"[realtime] 接続 {REALTIME_URL} (sessionId={session_id})")
    print(f"[realtime] {encoding} / {frame_bytes} バイトを {FRAME_MS} ms 間隔で送信 (実時間ペース)")

    audio = wav[info["data_offset"] : info["data_offset"] + info["data_len"]]
    final_text = ""
    started = time.monotonic()
    try:
        async with websockets.connect(uri, open_timeout=timeout, max_size=2**23) as ws:
            await ws.send(json.dumps(handshake, ensure_ascii=False))
            ack = json.loads(await ws.recv())
            if "sessionId" not in ack:
                print(f"[realtime] handshake 失敗: {redact(ack)}")
                return 1
            print(f"[realtime] handshake OK (session {ack['sessionId']})")

            import asyncio

            for offset in range(0, len(audio), frame_bytes):
                await ws.send(audio[offset : offset + frame_bytes])
                await asyncio.sleep(FRAME_MS / 1000.0)
            await ws.send(json.dumps({"type": "endStream"}))
            print("[realtime] endStream 送信。残りのイベントを読みます…")

            async for message in ws:
                if isinstance(message, bytes):
                    continue
                try:
                    event = json.loads(message)
                except json.JSONDecodeError:
                    continue
                kind = event.get("type")
                if kind == "transcript":
                    text = event.get("transcript", "")
                    if event.get("final"):
                        final_text = text
                        print(f"\n[realtime] final: {redact(text)}")
                    else:
                        sys.stdout.write("\r\x1b[K[realtime] partial: " + redact(text)[-110:])
                        sys.stdout.flush()
                elif kind == "speechStart":
                    print("[realtime] speechStart")
                elif kind == "speechEnd":
                    print("[realtime] speechEnd")
                elif kind == "speechComplete":
                    final_text = event.get("transcript", final_text)
                    print(f"[realtime] speechComplete: {redact(final_text)}")
                elif kind == "error":
                    print(f"[realtime] error: {redact(event.get('message', ''))}")
    except Exception as exc:  # websockets の例外型はバージョンで変わるため広く拾う
        name = type(exc).__name__
        if "ConnectionClosed" in name:
            code = getattr(exc, "code", None) or getattr(getattr(exc, "rcvd", None), "code", None)
            hint = WS_CLOSE_HINTS.get(code, "close code を確認する。")
            print(f"\n[realtime] 接続が閉じました: code={code} {hint}")
            return 0 if code in (1000, 1011) else 1
        raise SystemExit(f"リアルタイム ASR でエラー: {name}: {exc}") from exc

    print(f"\n[realtime] 所要 {time.monotonic() - started:.1f} 秒 / final={bool(final_text)}")
    return 0 if final_text else 2


# ---------------------------------------------------------------------------
# ドライラン / 自己テスト
# ---------------------------------------------------------------------------


def dry_run(wav_path: str | None, req: dict, realtime: bool) -> int:
    print("== Meta Muse Voice Transcribe スモークテスト (ドライラン) ==")
    print(f"理由: META_API_KEY {'未設定' if not os.environ.get('META_API_KEY') else '未使用 (--dry-run)'}")
    print(f"META_API_KEY: {mask_secret(os.environ.get('META_API_KEY'))}")

    info = None
    if wav_path:
        raw, info = load_wav_checked(wav_path)
        print(
            f"\nWAV 検証 OK: {os.path.basename(wav_path)} / {info['seconds']:.2f} 秒 / "
            f"{info['sample_rate']} Hz / mono / 16-bit"
        )
        if info["seconds"] > MAX_AUDIO_SECONDS:
            print(f"警告: 10 分を超えています ({info['seconds'] / 60:.1f} 分)。分割してください。")
        if len(raw) > MAX_BODY_BYTES:
            print("警告: 32MB を超えています。分割してください。")
    else:
        print("\nWAV 未指定のため形式検証はスキップ (引数に 16k/24k mono 16-bit WAV を渡してください)。")

    boundary = "----SasWatchSmokeDRYRUN"
    print("\n送信するリクエスト (キーは伏字):")
    if realtime:
        handshake = {
            "authorization": {"accessToken": "Bearer ****"},
            "audioEncoding": info["encoding"] if info else "PCM_16KHZ",
            "model": MODEL,
            "mode": req["mode"],
            "partialMode": "CUMULATIVE",
            "emitAudioProgress": False,
        }
        if req.get("languageBias"):
            handshake["languageBias"] = req["languageBias"]
        if req.get("keywords"):
            handshake["keywords"] = req["keywords"]
        print(f"  1. WS  {REALTIME_URL}?sessionId=<uuid>")
        print(f"     handshake: {redact(json.dumps(handshake, ensure_ascii=False))}")
        print("     音声: signed 16-bit LE / mono のバイナリフレームを実時間ペースで送る")
        print('  2. 送信終了: {"type":"endStream"} → サーバが 1000 で閉じるまで読む')
        print("     注意: handshake は接続後 10 秒以内。5 秒以上先読みすると 1008。")
        if not _has_websockets():
            print("     ※ websockets パッケージが無いため、このモードは実行できません。")
    else:
        print(f"  1. POST {TRANSCRIBE_URL}?sessionId=<uuid>")
        print('     Authorization: Bearer **** / Content-Type: multipart/form-data')
        print(f"     request パート: {json.dumps(req, ensure_ascii=False)}")
        print('     audio パート:  WAV ファイル (filename="smoke.wav", Content-Type: audio/wav)')
        print(f"     期待: 200 {{\"sessionId\", \"transcript\", \"audioDurationMs\"}}")
        _ = boundary
        if info:
            sample = multipart_body(req, b"", boundary, "smoke.wav")
            print(f"     (multipart 本文の組み立てテスト: {len(sample)} バイト / boundary あり)")

    checks = self_test(verbose=False)
    print(f"\nリクエスト形状の自己テスト: {checks['passed']}/{checks['total']} 合格")
    for line in checks["failures"]:
        eprint("  NG: " + line)
    if checks["failures"]:
        return 1
    print("ドライラン完了 (実通信はしていません)。")
    return 0


def _has_websockets() -> bool:
    try:
        import websockets  # noqa: F401
    except ImportError:
        return False
    return True


def self_test(verbose: bool = True) -> dict:
    failures: list[str] = []
    counter = {"total": 0}

    def check(name: str, cond: bool, detail: str = "") -> None:
        counter["total"] += 1
        if cond:
            if verbose:
                print(f"ok  {name}")
        else:
            failures.append(f"{name} {detail}".strip())

    # WAV 解析 (自己生成した無音 WAV をその場で使う。テストデータはコミットしない)
    raw = make_test_wav(0.2, 16000)
    info = parse_wav_bytes(raw, "test.wav")
    check("WAV: 16k mono 16-bit を受理", info["sample_rate"] == 16000 and info["channels"] == 1)
    check("WAV: 長さを計算", abs(info["seconds"] - 0.2) < 0.01, str(info["seconds"]))
    check("WAV: encoding が PCM_16KHZ", info["encoding"] == "PCM_16KHZ")
    info24 = parse_wav_bytes(make_test_wav(0.1, 24000), "test24.wav")
    check("WAV: 24k の encoding", info24["encoding"] == "PCM_24KHZ")
    def mutate(offset: int, value: bytes) -> bytes:
        return raw[:offset] + value + raw[offset + len(value) :]

    for name, bad in (
        ("RIFF でない", b"xxxx" + raw[4:]),
        # 標準 WAV: audio_format@20, channels@22, sample_rate@24, bits@34
        ("bit 深さ違い", mutate(34, (8).to_bytes(2, "little"))),
        ("モノラルでない", mutate(22, (2).to_bytes(2, "little"))),
        ("PCM でない", mutate(20, (3).to_bytes(2, "little"))),
        ("サンプルレート違い", mutate(24, (44100).to_bytes(4, "little"))),
    ):
        try:
            parse_wav_bytes(bad, "bad.wav")
            check(f"WAV: {name} を拒否", False)
        except WavError:
            check(f"WAV: {name} を拒否", True)

    # request パート
    req = transcribe_request(["Japanese"], ["SAS-Watch"], "PUSH_TO_TALK")
    check("request: model は muse-voice-transcribe-1.0", req["model"] == MODEL)
    check("request: audioEncoding は WAV", req["audioEncoding"] == "WAV")
    check("request: languageBias", req["languageBias"] == ["Japanese"])
    check("request: keywords", req["keywords"] == ["SAS-Watch"])

    # multipart
    body = multipart_body(req, raw, "BOUNDARY123", "smoke.wav")
    check("multipart: request パート", b'name="request"' in body)
    check("multipart: audio パート", b'name="audio"; filename="smoke.wav"' in body)
    check("multipart: 終端 boundary", body.rstrip().endswith(b"--BOUNDARY123--"))
    check("multipart: 音声データを含む", raw in body)

    # エラー対処文
    for code in (400, 401, 406, 413, 429, 500):
        check(f"HTTP {code} の対処文がある", code in HTTP_HINTS and bool(HTTP_HINTS[code]))
    for code in (1000, 1008, 1011, 1013):
        check(f"WS close {code} の対処文がある", code in WS_CLOSE_HINTS)

    # 伏字
    red = redact("Authorization: Bearer LLM_1234567890_FAKEKEY")
    check("API キーを伏字にする", "FAKEKEY" not in red and "LLM_****" in red, red)

    result = {
        "total": counter["total"],
        "passed": counter["total"] - len(failures),
        "failures": failures,
    }
    if verbose:
        print(f"self-test: {result['passed']}/{result['total']} 合格")
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Meta Muse Voice Transcribe (ファイル/リアルタイム) のスモークテスト",
    )
    parser.add_argument("wav", nargs="?", help="16k/24k mono 16-bit PCM WAV のパス")
    parser.add_argument("--realtime", action="store_true", help="WebSocket のリアルタイム ASR を使う")
    parser.add_argument("--mode", default="PUSH_TO_TALK", choices=["PUSH_TO_TALK", "ENDPOINTING"])
    parser.add_argument("--language", default="Japanese", help="languageBias (既定: Japanese)")
    parser.add_argument("--keywords", default="", help="カンマ区切りの keywords (例: SAS-Watch,Muse)")
    parser.add_argument("--key", default=None, help="API キー (既定: META_API_KEY)。履歴に残るので非推奨")
    parser.add_argument("--dry-run", action="store_true", help="通信せず形状だけ表示")
    parser.add_argument("--self-test", action="store_true", help="通信なしで内部チェックを実行")
    parser.add_argument("--timeout", type=float, default=120.0, help="HTTP/WebSocket のタイムアウト秒")
    args = parser.parse_args(argv)

    if args.self_test:
        result = self_test(verbose=True)
        if result["failures"]:
            for line in result["failures"]:
                eprint("NG: " + line)
            return 1
        return 0

    key = args.key or os.environ.get("META_API_KEY", "").strip() or os.environ.get("MODEL_API_KEY", "").strip()
    keywords = [k.strip() for k in args.keywords.split(",") if k.strip()]
    language_bias = [args.language] if args.language else []
    req = transcribe_request(language_bias, keywords, args.mode)

    if args.dry_run or not key:
        return dry_run(args.wav, req, args.realtime)
    if not args.wav:
        parser.error("WAV のパスを指定してください (--self-test / --dry-run 以外)")
    if not key.startswith("LLM_"):
        eprint("警告: META_API_KEY が 'LLM_' で始まっていません。キー全体を貼り付けてください。")

    raw, info = load_wav_checked(args.wav)
    print("== Meta Muse Voice Transcribe スモークテスト ==")
    print(f"key={mask_secret(key)}  file={os.path.basename(args.wav)}  mode={args.mode}")

    if args.realtime:
        import asyncio

        return asyncio.run(run_realtime(key, raw, info, req, args.timeout))
    return post_transcribe(key, raw, info, req, args.timeout)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit(130)
