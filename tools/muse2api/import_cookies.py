#!/usr/bin/env python3
"""muse2api に Muse (muse.ai) のログイン cookie を投入するヘルパー。

* cookie は **標準入力 (stdin) からだけ** 受け取る。コマンドライン引数・環境変数・
  ファイルには cookie を書かない (シェル履歴や ps に残さないため)。
* API キーは 環境変数 MUSE2API_KEY / --key-file / 非表示プロンプト の順で取得し、
  画面・ログには一切表示しない。
* 送信先は /admin/accounts (POST, Authorization: Bearer <MUSE2API_KEY>)。
  既定の curl 例:
      curl -X POST http://127.0.0.1:18610/admin/accounts \
           -H "Authorization: Bearer $MUSE2API_KEY" \
           -H "Content-Type: application/json" -d '{"cookie_header":"..."}'
  このスクリプトは同じことを stdin 経由で安全に行う。

使い方 (例):
    # 1) デプロイ先 .env のキーを使う場合 (サーバー上で実行)
    python3 import_cookies.py --url http://127.0.0.1:18610 \
        --key-file /home/sasnews/deepseek-harness/default-workspace/services/muse2api/.env \
        --label py-import

    # 2) キーを環境変数で渡す場合 (ユーザー PC から実行)
    #    Windows PowerShell: $env:MUSE2API_KEY="m2a_..." ; python import_cookies.py --url https://muse.sasnews.dev
    python3 import_cookies.py --url http://192.168.10.10:18610

    # 3) まず解析だけ確認 (送信しない)
    python3 import_cookies.py --dry-run

    # stdin には Chrome DevTools でコピーした Cookie ヘッダ文字列を貼り付け:
    #   hatch_sess=...; hatch_native_auth_device=...; __Host-...=...
    # または JSON: {"hatch_sess": "...", ...} /
    #   [{"name":"hatch_sess","value":"...","expirationDate":1790000000}, ...]

終了コード: 0=成功, 2=入力エラー, 3=認証エラー, 4=サーバーが 4xx/5xx, 5=接続エラー
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import urllib.error
import urllib.request
from typing import Dict, Optional, Tuple


def eprint(*args: object) -> None:
    print(*args, file=sys.stderr)


# --------------------------------------------------------------------------- #
# API キーの取得 (値は絶対に表示しない)
# --------------------------------------------------------------------------- #
def _read_key_from_dotenv(path: str) -> Optional[str]:
    """デプロイ先の .env から MUSE2API_KEY だけ読む (他行は無視、値は表示しない)。"""
    try:
        with open(path, "r", encoding="utf-8") as f:
            for line in f:
                s = line.strip()
                if s.startswith("MUSE2API_KEY="):
                    return s.split("=", 1)[1].strip().strip("'\"")
    except OSError as exc:
        eprint(f"キーファイルを読めません: {path} ({exc.__class__.__name__})")
    return None


def resolve_api_key(args: argparse.Namespace) -> Optional[str]:
    if args.key_file:
        return _read_key_from_dotenv(args.key_file)
    env_key = os.environ.get("MUSE2API_KEY", "").strip()
    if env_key:
        return env_key
    try:
        import getpass

        key = getpass.getpass("MUSE2API_KEY (入力は表示されません): ").strip()
    except (KeyboardInterrupt, EOFError):
        return None
    return key or None


# --------------------------------------------------------------------------- #
# stdin の cookie 解析
# --------------------------------------------------------------------------- #
def parse_cookie_text(text: str) -> Tuple[Dict[str, str], Dict[str, int]]:
    """`name=value; name2=value2` 形式の Cookie ヘッダ文字列を dict にする。"""
    cookies: Dict[str, str] = {}
    for part in text.split(";"):
        part = part.strip()
        if not part or "=" not in part:
            continue
        name, value = part.split("=", 1)
        name, value = name.strip(), value.strip()
        if name:
            cookies[name] = value
    return cookies, {}


def parse_stdin(raw: str) -> Tuple[Dict[str, str], Dict[str, int]]:
    """stdin を Cookie 文字列 / JSON オブジェクト / EditThisCookie 風 JSON 配列として解釈する。"""
    text = raw.strip()
    if not text:
        raise ValueError("入力が空です。cookie 文字列を貼り付けてください。")

    if text.startswith("{"):
        data = json.loads(text)
        if not isinstance(data, dict):
            raise ValueError("JSON はオブジェクト形式で貼り付けてください。")
        cookies = {str(k): str(v) for k, v in data.items() if v is not None}
        return cookies, {}

    if text.startswith("["):
        data = json.loads(text)
        if not isinstance(data, list):
            raise ValueError("JSON 配列の形式が想定と違います。")
        cookies, expires = {}, {}
        for item in data:
            if not isinstance(item, dict) or "name" not in item:
                continue
            name = str(item["name"])
            cookies[name] = str(item.get("value", ""))
            exp = item.get("expirationDate")
            if exp:
                try:
                    expires[name] = int(float(exp))
                except (TypeError, ValueError):
                    pass
        return cookies, expires

    return parse_cookie_text(text)


def read_stdin() -> str:
    if sys.stdin.isatty():
        eprint("cookie を貼り付けてください (貼り付け後に Ctrl-D / Windows は Ctrl-Z → Enter):")
    return sys.stdin.read()


# --------------------------------------------------------------------------- #
# 送信
# --------------------------------------------------------------------------- #
def post_account(args: argparse.Namespace, key: str,
                 cookies: Dict[str, str], expires: Dict[str, int]) -> int:
    payload: Dict[str, object] = {}
    if exp := {k: v for k, v in expires.items() if v > 0}:
        payload["expires"] = exp
    # cookie_header 形式の方が upstream の parse_cookie_text と挙動が一致する。
    # (値に ';' や '=' を含む cookie でも JSON dict より壊れにくい)
    header = "; ".join(f"{k}={v}" for k, v in cookies.items())
    if args.label:
        payload["label"] = args.label
    payload["cookies"] = cookies
    payload["cookie_header"] = header

    body = json.dumps(payload).encode("utf-8")
    url = args.url.rstrip("/") + "/admin/accounts"
    req = urllib.request.Request(
        url,
        data=body,
        method="POST",
        headers={
            "Content-Type": "application/json",
            "Authorization": f"Bearer {key}",
            "Accept": "application/json",
        },
    )
    try:
        with urllib.request.urlopen(req, timeout=args.timeout) as resp:
            raw = resp.read()
    except urllib.error.HTTPError as exc:
        eprint(f"投入失敗: HTTP {exc.code}")
        if exc.code in (401, 403):
            eprint("API キーが違う可能性があります (表示はしません)。")
            return 3
        try:
            eprint(exc.read().decode("utf-8", "replace")[:400])
        except Exception:
            pass
        return 4
    except urllib.error.URLError as exc:
        eprint(f"接続失敗: {args.url} ({exc.reason})")
        return 5

    try:
        data = json.loads(raw.decode("utf-8"))
    except json.JSONDecodeError:
        eprint("サーバーの応答が JSON ではありません。")
        return 4

    added = data.get("added") or []
    eprint("投入成功 (cookie の値は表示しません):")
    print(f"  count          : {data.get('count', len(added))}")
    for acc in added:
        print(f"  account_id     : {acc.get('id')}")
        print(f"  label          : {acc.get('label')}")
        print(f"  cookie_count   : {acc.get('cookie_count')}")
        print(f"  expires_at     : {acc.get('expires_at')}")
    missing = data.get("essential_missing") or []
    if missing:
        print(f"  essential_missing: {', '.join(missing)}")
    if data.get("warning"):
        print(f"  warning        : {data['warning']}")
    print("  next           : 管理画面 /admin の「テスト」でセッションとチャット入力を確認してください。")
    return 0


def main(argv: Optional[list[str]] = None) -> int:
    p = argparse.ArgumentParser(
        description="muse2api に Muse の cookie を安全に投入する (cookie は stdin から)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__.split("使い方", 1)[1] if "使い方" in __doc__ else None,
    )
    p.add_argument("--url", default=os.environ.get("MUSE2API_URL", "http://127.0.0.1:18610"),
                   help="muse2api のベース URL (既定: %(default)s)")
    p.add_argument("--label", default="", help="アカウントの表示名 (例: pc-chrome)")
    p.add_argument("--key-file", default="",
                   help="MUSE2API_KEY=... を含む .env のパス (値は読み取るだけで表示しない)")
    p.add_argument("--timeout", type=float, default=30.0, help="HTTP タイムアウト秒 (既定: 30)")
    p.add_argument("--dry-run", action="store_true",
                   help="解析結果の個数だけ表示し、サーバーには送らない")
    args = p.parse_args(argv)

    try:
        cookies, expires = parse_stdin(read_stdin())
    except (ValueError, json.JSONDecodeError) as exc:
        eprint(f"入力エラー: {exc}")
        return 2
    if not cookies:
        eprint("cookie を 1 つも解析できませんでした。")
        return 2

    if args.dry_run:
        print(f"解析結果: cookie {len(cookies)} 個, expires {len(expires)} 個 (送信していません)")
        return 0

    key = resolve_api_key(args)
    if not key:
        eprint("API キーが取得できませんでした (MUSE2API_KEY / --key-file を確認)。")
        return 3
    return post_account(args, key, cookies, expires)


if __name__ == "__main__":
    sys.exit(main())
