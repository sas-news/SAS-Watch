# tools/muse2api — muse2api 運用補助

muse2api (muse.ai を OpenAI 互換 API にする self-hosted ブリッジ) の運用に使う補助スクリプト。
サーバー構成・手順・セキュリティ・トラブルシュートは `docs/muse2api-server.md` を参照。

| ファイル | 用途 |
|---|---|
| `import_cookies.py` | Muse のログイン cookie を **stdin から**受け取り、muse2api の管理 API (`POST /admin/accounts`) に投入する。cookie も API キーも画面・ファイル・ログに残さない |

## import_cookies.py

```bash
# サーバー上 (キーはデプロイ先 .env から読む)
python3 tools/muse2api/import_cookies.py \
  --url http://127.0.0.1:18610 \
  --key-file /home/sasnews/deepseek-harness/default-workspace/services/muse2api/.env \
  --label pc-chrome
# ↑ 実行後に Chrome DevTools でコピーした Cookie 文字列を貼り付け、Ctrl-D

# 解析だけ確認 (送信しない)
python3 tools/muse2api/import_cookies.py --dry-run

# PC から実行する場合: PC が tailnet 参加なら Tailscale Serve 経由が推奨
#   python3 import_cookies.py --url https://sas.tail85c8c5.ts.net:8445
# PC が同一 Wi-Fi なら LAN:  python3 import_cookies.py --url http://192.168.10.10:18610
```

- 入力は `name=value; name2=value2` のほか、JSON オブジェクト、EditThisCookie 風の
  `[{"name":…,"value":…,"expirationDate":…}]` も可。
- キーは `環境変数 MUSE2API_KEY` → `--key-file` → 非表示プロンプト の順で取得。
- 成功時はアカウント ID・cookie 個数・`essential_missing` だけを表示する
  (cookie とキーの値は表示しない)。
- 必須 cookie は `hatch_sess` (あると良い: `hatch_vml` / `hatch_gw` / `hatch_native_auth_device`)。

通常は管理画面からダウンロードできる Chrome 拡張 (方式 A) が簡単。
DevTools から手動で投入したい場合や、PC から tailnet/LAN 経由で投入したい場合に本スクリプトを使う。
外出先の既定 URL は `https://sas.tail85c8c5.ts.net:8445` (Tailscale Serve, tailnet 限定・TLS)。
公開 Cloudflare トンネル (`muse.sasnews.dev`) は撤回済み。
