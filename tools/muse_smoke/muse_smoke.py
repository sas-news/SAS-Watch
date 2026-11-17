#!/usr/bin/env python3
"""useapi.net Muse API のスモークテスト (SAS-Watch / T40)。

※ 2026-11-17 方針変更: これは有料構成 (Meta Model API / useapi.net) 用のツール。
   現在の無料構成 (スマホ内 Vosk + self-hosted muse2api) では使いません。
   無料構成の疎通は docs/muse-setup.md §5 の curl を参照。

環境変数:
  USEAPI_TOKEN   useapi.net の API トークン (user:...)。未設定ならドライラン。
  MUSE_EMAIL     Muse アカウントのメール (任意)。チャット作成時に使う。
  MUSE_CHAT_ID   既存の chatId (任意)。空なら POST /chats で新規作成する。

使い方:
  python3 tools/muse_smoke/muse_smoke.py                 # 鍵が無ければドライラン
  python3 tools/muse_smoke/muse_smoke.py --dry-run       # 強制的にドライラン
  python3 tools/muse_smoke/muse_smoke.py --self-test     # 通信なしの自己テスト
  USEAPI_TOKEN='user:...' MUSE_EMAIL='you@example.com' \
      python3 tools/muse_smoke/muse_smoke.py --message 'こんにちは'

鍵は環境変数だけで渡す (ファイル・シェル履歴・リポジトリに書かない)。
外部 HTTP 通信は標準ライブラリのみ。Python 3.9 以降。
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid

BASE = "https://api.useapi.net/v1/muse"
DEFAULT_MESSAGE = "SAS-Watch のスモークテストです。「OK」とだけ返してください。"
DEFAULT_TIMEOUT = 180.0
MAX_MESSAGE = 5000
USER_AGENT = "sas-watch-muse-smoke/1.0"

# HTTP エラーコード -> 日本語の意味と対処 (useapi.net Muse API の一次情報より)。
ERROR_HINTS = {
    400: "リクエスト不正。message 必須・5000 文字以内、chatId の形式、account 未追加を確認。",
    401: "トークン不正。USEAPI_TOKEN 全体 (user: 以降を含む) を貼り直す。",
    402: "サブスク切れ。useapi.net の支払い状況を確認する。",
    403: "chatId / assetId が別の API トークンのもの。トークンの取り違えを確認。",
    404: "アカウント未接続、またはチャット削除済み。Setup Muse (POST /accounts) をやり直す。",
    412: "Muse の週次利用枠が切れている。GET /accounts/{email} で quota を確認 (resetsAt まで待つ)。",
    429: "Muse 側のレート制限、またはストリーム過多。約 1 分待って再試行。",
    502: "Muse がメッセージを受け付けなかった。再試行する。",
    503: "Muse に到達できなかった。少し待って再試行する。",
    504: "送信中に接続が切れた。メッセージが届いている可能性があるので GET /chats で確認してから再送。",
    596: "アカウント要対応 (Meta のセキュリティチェック / パスワード変更)。"
    "muse.ai にブラウザでログインして確認し、POST /accounts で同じメール・パスワードを再登録する。",
}


def eprint(*args: object) -> None:
    print(*args, file=sys.stderr)


def redact(text: object) -> str:
    """トークンらしき文字列を伏せ字にする (ログ用)。"""
    s = str(text)
    # user:<number>-<secret>
    s = re.sub(r"user:\d+-[A-Za-z0-9._~+/=-]+", "user:****", s)
    # LLM_<id>_<secret> (Meta Model API キーが紛れ込んだ場合)
    s = re.sub(r"LLM_[0-9]+_[A-Za-z0-9._~+/=-]+", "LLM_****", s)
    s = re.sub(r"Bearer\s+[A-Za-z0-9._~+/=:-]{8,}", "Bearer ****", s)
    return s


def mask(value: str | None) -> str:
    """メールや chatId 用。先頭と末尾だけ見せる。"""
    if not value:
        return "(未設定)"
    if len(value) <= 8:
        return "****"
    return value[:5] + "…" + value[-4:]


def mask_secret(value: str | None) -> str:
    """鍵・トークン用。種類と長さだけ見せ、値は出さない。"""
    if not value:
        return "(未設定)"
    prefix = ""
    if value.startswith("user:"):
        prefix = "user:"
    elif value.startswith("LLM_"):
        prefix = "LLM_"
    return f"{prefix}**** ({len(value)} 文字)"


def quote_chat_id(chat_id: str) -> str:
    """chatId は : と @ を含むので URL エンコードする (useapi.net の要件)。"""
    return urllib.parse.quote(chat_id, safe="")


def build_chat_body(message: str, email: str | None) -> dict:
    if not message:
        raise ValueError("message が空です")
    if len(message) > MAX_MESSAGE:
        raise ValueError(f"message が長すぎます ({len(message)} > {MAX_MESSAGE})")
    body: dict[str, object] = {"message": message}
    if email:
        body["email"] = email
    return body


def build_headers(token: str, accept: str = "application/json") -> dict:
    return {
        "Authorization": f"Bearer {token}",
        "Content-Type": "application/json",
        "Accept": accept,
        "User-Agent": USER_AGENT,
    }


# ---------------------------------------------------------------------------
# HTTP
# ---------------------------------------------------------------------------


def http_json(
    method: str, url: str, token: str, body: dict | None = None, timeout: float = 30.0
) -> tuple[int, dict]:
    data = None
    if body is not None:
        data = json.dumps(body, ensure_ascii=False).encode("utf-8")
    req = urllib.request.Request(url, data=data, method=method, headers=build_headers(token))
    try:
        with urllib.request.urlopen(req, timeout=timeout) as res:
            raw = res.read().decode("utf-8", "replace")
            return res.status, (json.loads(raw) if raw.strip() else {})
    except urllib.error.HTTPError as exc:
        raw = exc.read().decode("utf-8", "replace")
        try:
            payload = json.loads(raw)
        except json.JSONDecodeError:
            payload = {"raw": raw[:800]}
        return exc.code, payload
    except urllib.error.URLError as exc:
        raise SystemExit(f"接続に失敗しました: {exc.reason}") from exc


def explain_http(code: int, payload: dict) -> str:
    hint = ERROR_HINTS.get(code, "未知のエラー。レスポンス本文を確認する。")
    err = redact(payload.get("error", ""))
    return f"HTTP {code}: {hint}" + (f"\n  server: {err}" if err else "")


# ---------------------------------------------------------------------------
# SSE
# ---------------------------------------------------------------------------


def iter_sse(lines):
    """SSE の行 (bytes / str) をイベント dict に変換して順に返す。"""
    data: list[str] = []
    for raw in lines:
        line = raw.decode("utf-8", "replace") if isinstance(raw, bytes) else raw
        line = line.rstrip("\r\n")
        if line == "":
            if data:
                text = "\n".join(data)
                data = []
                try:
                    yield json.loads(text)
                except json.JSONDecodeError:
                    yield {"event": "parse-error", "raw": text[:300]}
            continue
        if line.startswith(":"):
            continue
        if line.startswith("data:"):
            data.append(line[5:].lstrip())
        elif line.startswith("id:"):
            continue  # 再開用。このスクリプトでは表示しない
    if data:
        try:
            yield json.loads("\n".join(data))
        except json.JSONDecodeError:
            pass


def open_stream(chat_id: str, token: str, timeout: float):
    url = f"{BASE}/chats/{quote_chat_id(chat_id)}/stream"
    req = urllib.request.Request(
        url,
        headers={
            "Authorization": f"Bearer {token}",
            "Accept": "text/event-stream",
            "Cache-Control": "no-cache",
            "User-Agent": USER_AGENT,
        },
    )
    try:
        return urllib.request.urlopen(req, timeout=timeout)
    except urllib.error.HTTPError as exc:
        raw = exc.read().decode("utf-8", "replace")
        try:
            payload = json.loads(raw)
        except json.JSONDecodeError:
            payload = {"raw": raw[:800]}
        raise SystemExit("ストリームを開けませんでした。\n" + explain_http(exc.code, payload))


# ---------------------------------------------------------------------------
# 実行
# ---------------------------------------------------------------------------


def print_event_summary(ev: dict) -> None:
    kind = ev.get("event")
    if kind == "activity":
        text = redact(ev.get("text", ""))
        if text and not text.startswith("is "):
            print(f"[activity] {text}")
    elif kind == "status":
        print(f"[status] {ev.get('status')}")
    elif kind == "approval":
        print(
            "[approval] Muse が承認を求めています。時計からは承認できないため、"
            "スマホの Muse チャット画面で承認してください。"
        )
        if ev.get("question"):
            print(f"           {redact(ev['question'])}")
    elif kind == "error":
        print(f"[error] {redact(ev.get('message', ''))} (code={ev.get('code')})")


def consume_stream(
    resp, created: bool, token: str, email: str | None, message: str, chat_id: str
) -> bool:
    """ストリームを読む。created=False のときは replay 完了後にメッセージを送る。"""
    tty = sys.stdout.isatty()
    texts: dict[str, str] = {}
    printed: set[str] = set()
    printing = created
    replied = False
    sent = False

    try:
        for ev in iter_sse(resp):
            kind = ev.get("event")
            if kind == "replayed":
                if not printing:
                    printing = True
                if not sent and not created:
                    code, payload = send_message(token, chat_id, message)
                    print(f"[send] POST /chats/{{chatId}}/messages -> HTTP {code}")
                    if code not in (200, 202):
                        print(explain_http(code, payload))
                        return False
                    sent = True
                continue

            if kind in ("initialized", "keepalive", "replayed"):
                continue
            if kind == "message":
                continue  # 自分が送ったメッセージのエコー
            if not printing:
                continue

            if kind == "text":
                mid = ev.get("messageId", "assistant")
                piece = ev.get("text", "")
                if ev.get("full"):
                    texts[mid] = piece
                else:
                    texts[mid] = texts.get(mid, "") + piece
                current = texts[mid]
                if tty:
                    sys.stdout.write("\r\x1b[K[assistant] " + redact(current)[-120:])
                    sys.stdout.flush()
                continue
            if kind == "message.done":
                mid = ev.get("messageId", "assistant")
                text = redact(texts.get(mid, ""))
                if tty:
                    sys.stdout.write("\r\x1b[K")
                print(f"[assistant] {text}")
                printed.add(mid)
                continue
            if kind == "turn.done":
                # message.done が来ない実装でも本文を出せるようにする。
                if tty:
                    sys.stdout.write("\r\x1b[K")
                for mid, text in texts.items():
                    if mid not in printed:
                        print(f"[assistant] {redact(text)}")
                print(f"[turn.done] status={ev.get('status')} files={len(ev.get('files') or [])}")
                replied = True
                break
            if kind in ("approval", "error", "idle", "reconnect"):
                print_event_summary(ev)
                if kind in ("error", "idle", "reconnect"):
                    print(
                        "  ストリームが終了しました。必要なら Last-Event-ID / ?after= で"
                        " 再接続してください。"
                    )
                break
            print_event_summary(ev)
    except (TimeoutError, OSError) as exc:
        eprint(f"\nストリーム読み取りがタイムアウト/切断しました: {exc}")
    finally:
        resp.close()

    if not replied:
        # turn.done が来なくても、text が取れていれば成功扱いに近いと分かるようにする。
        if texts:
            joined = redact("".join(texts.values()))
            print(f"[summary] turn.done 未受信。受信済みテキスト: {joined[:400]}")
        return False
    return True


def create_chat(token: str, message: str, email: str | None, timeout: float) -> str:
    body = build_chat_body(message, email)
    print(f"[create] POST {BASE}/chats  (message {len(message)} 文字, email={mask(email)})")
    code, payload = http_json("POST", f"{BASE}/chats", token, body, timeout=timeout)
    if code != 201:
        raise SystemExit("チャットを作成できませんでした。\n" + explain_http(code, payload))
    chat_id = payload.get("chatId")
    if not chat_id:
        raise SystemExit(f"応答に chatId がありません: {redact(payload)}")
    print(f"[create] chatId = {redact(chat_id)}")
    print(f"[create] messageId = {payload.get('messageId')}")
    return str(chat_id)


def send_message(token: str, chat_id: str, message: str, timeout: float = 30.0) -> tuple[int, dict]:
    url = f"{BASE}/chats/{quote_chat_id(chat_id)}/messages"
    return http_json("POST", url, token, build_chat_body(message, None), timeout=timeout)


def run(args: argparse.Namespace) -> int:
    token = args.token or os.environ.get("USEAPI_TOKEN", "").strip()
    email = args.email or os.environ.get("MUSE_EMAIL", "").strip() or None
    chat_id = args.chat_id or os.environ.get("MUSE_CHAT_ID", "").strip()
    message = args.message or DEFAULT_MESSAGE

    if not token:
        return dry_run(email=email, chat_id=chat_id, message=message, why="USEAPI_TOKEN 未設定")
    if not token.startswith("user:"):
        eprint("警告: USEAPI_TOKEN が 'user:' で始まっていません。トークン全体を貼り付けてください。")

    print("== useapi.net Muse API スモークテスト ==")
    print(f"token={mask_secret(token)}  email={mask(email)}  chatId={mask(chat_id)}")
    started = time.monotonic()

    created = not chat_id
    if created:
        chat_id = create_chat(token, message, email, args.http_timeout)
    else:
        print(f"[reuse] chatId = {redact(chat_id)} (既存チャットに送信)")

    resp = open_stream(chat_id, token, timeout=args.timeout)
    ok = consume_stream(resp, created, token, email, message, chat_id)

    print(f"\n所要 {time.monotonic() - started:.1f} 秒")
    if ok:
        print("結果: OK (turn.done を受信)")
        if created:
            print("ヒント: 次回から MUSE_CHAT_ID に上の chatId を設定すると同じ会話を継続できます。")
        return 0
    print("結果: 応答を完了できませんでした。上の [error]/[status] を確認してください。")
    return 2


def dry_run(email: str | None, chat_id: str, message: str, why: str) -> int:
    """実通信せず、送るリクエストの形だけを検証・表示する。"""
    print("== useapi.net Muse API スモークテスト (ドライラン) ==")
    print(f"理由: {why}")
    print(f"USEAPI_TOKEN: {mask_secret(os.environ.get('USEAPI_TOKEN'))}")
    print(f"MUSE_EMAIL:   {mask(email)}")
    print(f"MUSE_CHAT_ID: {mask(chat_id)}")
    print("\n送信するリクエスト (トークンは伏字):")

    if chat_id:
        print(f"\n  1. GET  {BASE}/chats/{quote_chat_id(chat_id)}/stream")
        print('     headers: {"Authorization": "Bearer ****", "Accept": "text/event-stream"}')
        print(f"\n  2. POST {BASE}/chats/{quote_chat_id(chat_id)}/messages")
        print("     body: " + redact(json.dumps(build_chat_body(message, None), ensure_ascii=False)))
        print("     期待: 202 {chatId, email, messageId}")
    else:
        print(f"\n  1. POST {BASE}/chats")
        print("     body: " + redact(json.dumps(build_chat_body(message, email), ensure_ascii=False)))
        print("     期待: 201 {chatId, email, messageId}")
        print(f"\n  2. GET  {BASE}/chats/<chatId>/stream")
        print('     headers: {"Authorization": "Bearer ****", "Accept": "text/event-stream"}')
        print("     期待: initialized → message/activity/text/turn.done (SSE)")

    checks = self_test(verbose=False)
    print(f"\nリクエスト形状の自己テスト: {checks['passed']}/{checks['total']} 合格")
    for line in checks["failures"]:
        eprint("  NG: " + line)
    if checks["failures"]:
        return 1
    print("ドライラン完了 (実通信はしていません)。")
    return 0


# ---------------------------------------------------------------------------
# 自己テスト (通信なし)
# ---------------------------------------------------------------------------


def self_test(verbose: bool = True) -> dict:
    import io

    failures: list[str] = []
    counter = {"total": 0}

    def check(name: str, cond: bool, detail: str = "") -> None:
        counter["total"] += 1
        if cond:
            if verbose:
                print(f"ok  {name}")
        else:
            failures.append(f"{name} {detail}".strip())

    # chatId のエンコード (: と @ が残らない)
    cid = "c-u12345-email:a+b@example.com-chat:272b-chat-bot:muse"
    enc = quote_chat_id(cid)
    check("chatId が URL エンコードされる", ":" not in enc and "@" not in enc, enc)
    check("chatId に %3A が含まれる", "%3A" in enc, enc)
    check("chatId に %40 が含まれる", "%40" in enc, enc)
    check("URL デコードで元に戻る", urllib.parse.unquote(enc) == cid)

    # message の長さ検証
    try:
        build_chat_body("x" * (MAX_MESSAGE + 1), None)
        check("5000 文字超の message を拒否", False)
    except ValueError:
        check("5000 文字超の message を拒否", True)
    check("5000 文字ちょうどの message を許可", bool(build_chat_body("x" * MAX_MESSAGE, None)))

    # email は任意
    check("email 省略時は body に含めない", "email" not in build_chat_body("hi", None))
    check("email 指定時は body に含める", build_chat_body("hi", "a@b.c")["email"] == "a@b.c")

    # ヘッダ
    h = build_headers("user:1-abc")
    check("Authorization は Bearer 全体", h["Authorization"] == "Bearer user:1-abc")
    check("Content-Type は application/json", h["Content-Type"] == "application/json")

    # SSE パーサ (複数イベント・data 行分割・コメント行)
    sample = (
        b": comment\n"
        b"data: {\"event\":\"initialized\",\"chatId\":\"c-1\",\"after\":0}\n\n"
        b"data: {\"event\":\"text\",\n"
        b"data:  \"messageId\":\"m1\",\"text\":\"hello\"}\n\n"
        b"data: {\"event\":\"turn.done\",\"status\":\"completed\",\"files\":[]}\n\n"
    )
    events = list(iter_sse(io.BytesIO(sample)))
    kinds = [e.get("event") for e in events]
    check("SSE: 3 イベントを解析", kinds == ["initialized", "text", "turn.done"], str(kinds))
    check("SSE: data 複数行を連結", events[1].get("text") == "hello", str(events[1]))

    # 伏字
    red = redact("Authorization: Bearer user:12345-FAKETOKEN and LLM_1234_FAKEKEY")
    check("トークンを伏字にする", "FAKETOKEN" not in red and "FAKEKEY" not in red, red)

    # エラー説明の網羅
    for code in (400, 401, 402, 403, 404, 412, 429, 502, 503, 504, 596):
        check(f"HTTP {code} の対処文がある", explain_http(code, {"error": "x"}).startswith("HTTP"))

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
        description="useapi.net Muse API のスモークテスト (鍵が無ければドライラン)",
    )
    parser.add_argument("--message", default=None, help=f"送るメッセージ (既定: {DEFAULT_MESSAGE!r})")
    parser.add_argument("--email", default=None, help="Muse アカウントのメール (既定: MUSE_EMAIL)")
    parser.add_argument("--chat-id", default=None, help="既存 chatId (既定: MUSE_CHAT_ID)")
    parser.add_argument("--token", default=None, help="API トークン (既定: USEAPI_TOKEN)。履歴に残るので非推奨")
    parser.add_argument("--dry-run", action="store_true", help="通信せずリクエスト形状だけ表示")
    parser.add_argument("--self-test", action="store_true", help="通信なしで内部チェックを実行")
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT, help="ストリーム全体のタイムアウト秒")
    parser.add_argument("--http-timeout", type=float, default=30.0, help="単発 HTTP のタイムアウト秒")
    args = parser.parse_args(argv)

    if args.self_test:
        result = self_test(verbose=True)
        if result["failures"]:
            for line in result["failures"]:
                eprint("NG: " + line)
            return 1
        return 0
    if args.dry_run:
        return dry_run(
            email=args.email or os.environ.get("MUSE_EMAIL") or None,
            chat_id=args.chat_id or os.environ.get("MUSE_CHAT_ID", "").strip(),
            message=args.message or DEFAULT_MESSAGE,
            why="--dry-run",
        )
    return run(args)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit(130)
