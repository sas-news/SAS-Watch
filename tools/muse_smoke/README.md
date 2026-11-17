# tools/muse_smoke — Muse / Muse Voice Transcribe のスモークテスト (有料構成用・現在は未使用)

> **現在の SAS-Watch では使いません (2026-11-17 方針変更)**。
> ユーザー指示「お金は出さない」により、アプリは無料構成
> (**スマホ内 Vosk + self-hosted muse2api**) に切り替わりました。
> このツールは有料構成 (Meta Model API / useapi.net) を再検討するとき用の**参考**として
> 残しています。無料構成の疎通確認は `docs/muse-setup.md` §5 の curl を使ってください。
> 鍵が無い状態のドライラン/自己テストだけなら、課金も通信も発生せず安全に実行できます。

SAS-Watch の AI アプリが (有料構成で) 使う 2 つの外部 API を、**アプリを経由せず**に
切り分けするための小さなツールです。設計は `docs/muse-integration.md`、ユーザー向け手順は
`docs/muse-setup.md` (無料構成が本文、有料構成は付録 A) を参照してください。

- `muse_smoke.py` — useapi.net の Muse API (チャット作成 → 送信 → SSE 受信)
- `asr_smoke.py` — Meta Model API の Muse Voice Transcribe (ファイル / リアルタイム)

どちらも **Python 3.9 以降の標準ライブラリだけ**で動きます (`asr_smoke.py --realtime` だけは
`websockets` が任意)。テストデータや鍵はリポジトリに置きません。

## 鍵の扱い (最初に読む)

- 鍵・トークン・パスワードは **環境変数だけで渡す**。ファイル、シェルスクリプト、リポジトリ、
  会話ログに書かない。
- `--token` / `--key` 引数も用意していますが、シェル履歴に残るので**使わない**でください。
- 画面に出すログは自動で伏字になります (`user:****` / `LLM_****`)。それでも共有時は注意。
- 鍵が無い状態でも「ドライラン」と「自己テスト」で、送るリクエストの形を検証できます。
  **実通信は鍵がある時だけ**です。

```bash
# 端末を閉じれば消える渡し方 (履歴に残さない)
read -r -s USEAPI_TOKEN && export USEAPI_TOKEN
read -r -s META_API_KEY && export META_API_KEY
```

---

## 1. muse_smoke.py (useapi.net Muse API)

### 環境変数

| 変数 | 必須 | 用途 |
|---|---|---|
| `USEAPI_TOKEN` | 実通信に必須 | useapi.net の API トークン (`user:...` 全体)。未設定ならドライラン |
| `MUSE_EMAIL` | 任意 | チャット作成時に渡す Muse アカウントのメール。複数アカウントを接続している時は必須 |
| `MUSE_CHAT_ID` | 任意 | 既存チャットの chatId。空なら `POST /chats` で新規作成し、chatId を表示する |

### 実行例

```bash
# 1) 鍵なし: 送るリクエストの形だけ確認 (実通信なし)
python3 tools/muse_smoke/muse_smoke.py

# 2) 通信なしの自己テスト (24 項目)
python3 tools/muse_smoke/muse_smoke.py --self-test

# 3) 実通信: 新規チャットを作って 1 往復
USEAPI_TOKEN='user:...' MUSE_EMAIL='you@example.com' \
  python3 tools/muse_smoke/muse_smoke.py --message 'こんにちは。短く返してください。'

# 4) 同じチャットで続ける (chatId は 3) の出力からコピー)
USEAPI_TOKEN='user:...' MUSE_CHAT_ID='c-u12345-email:...-chat:...-bot:muse' \
  python3 tools/muse_smoke/muse_smoke.py --message 'さっきの続きです。'
```

### 何を確認できるか

- チャット作成 `POST /v1/muse/chats` → `201 {chatId, email, messageId}`
- SSE `GET /v1/muse/chats/{chatId}/stream` のイベント (`status` / `activity` / `text` /
  `message.done` / `turn.done` / `approval` / `error` / `idle` / `reconnect`)
- 既存チャットへの送信 `POST /v1/muse/chats/{chatId}/messages` → `202`
- chatId の URL エンコード (`:` と `@` を含むため必須)
- 401 / 402 / 404 / 412 / 429 / 596 などで返るエラーの日本語の意味と対処
- 承認要求 (`approval`) が来たら「スマホの Muse で承認」を表示して止まる (時計からは承認できない)

### 出力の読み方

```
[create] chatId = c-u12345-email:…-bot:muse
[status] running
[activity] Generating image
[assistant] OK
[turn.done] status=completed files=0
結果: OK (turn.done を受信)
```

`turn.done` が来れば 1 往復の成功です。来ない場合は `[error]` / `[status]` を確認し、
`docs/muse-setup.md` の切り分け表へ。ストリームは 15 秒ごとに keepalive、10 分無音で `idle`、
60 分で `reconnect` が来ます (どちらも異常ではありません)。

> 初回に表示された chatId を `MUSE_CHAT_ID` に入れると、同じ Muse 会話を続けられます。
> アプリの「Muse の chatId」設定に貼る値もこれです。

---

## 2. asr_smoke.py (Meta Muse Voice Transcribe)

### 環境変数

| 変数 | 必須 | 用途 |
|---|---|---|
| `META_API_KEY` | 実通信に必須 | Meta Model API キー (`LLM_...`)。未設定ならドライラン |
| `MODEL_API_KEY` | 予備 | Meta のサンプルと同じ名前でも可 (両方あれば `META_API_KEY` を優先) |

### 音声ファイル

**mono / 16-bit PCM / 16 kHz または 24 kHz の WAV** だけ受け付けます
(サーバ側の仕様。10 分・32MB 上限)。mp3/m4a などは ffmpeg で変換してください。

```bash
ffmpeg -i input.m4a -ac 1 -ar 24000 -c:a pcm_s16le -map_metadata -1 /tmp/rec.wav
```

テスト用の 1 秒無音 WAV は、リポジトリにファイルを置かずにその場で作れます:

```bash
python3 - <<'EOF'
import wave
with wave.open('/tmp/silence16k.wav', 'wb') as w:
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(16000)
    w.writeframes(b'\x00\x00' * 16000)
EOF
```

### 実行例

```bash
# 1) 鍵なし: WAV の形式検証 + multipart の組み立て確認 (実通信なし)
python3 tools/muse_smoke/asr_smoke.py /tmp/rec.wav

# 2) 通信なしの自己テスト (28 項目。WAV はその場で生成)
python3 tools/muse_smoke/asr_smoke.py --self-test

# 3) ファイル一括: POST /v1/asr/transcribe
META_API_KEY='LLM_...' python3 tools/muse_smoke/asr_smoke.py /tmp/rec.wav

# 4) リアルタイム: wss://api.meta.ai/v1/asr/realtime (要 websockets)
python3 -m pip install websockets
META_API_KEY='LLM_...' python3 tools/muse_smoke/asr_smoke.py /tmp/rec.wav --realtime

# 任意: 用語バイアス
META_API_KEY='LLM_...' python3 tools/muse_smoke/asr_smoke.py /tmp/rec.wav \
  --language Japanese --keywords 'SAS-Watch,Muse'
```

### 何を確認できるか

- WAV が仕様どおりか (RIFF/WAVE、PCM、mono、16-bit、16k/24k) と長さ・上限
- multipart (`request` JSON + `audio`) の組み立て
- ファイル ASR の応答 (`transcript`, `audioDurationMs`, `turns`)
- リアルタイムの handshake → PCM フレーム送信 → `endStream` → final 受信
- 400 / 401 / 406 / 413 / 429 / 500 と、WebSocket close 1000 / 1008 / 1011 / 1013 の意味
  (1008 は「バックログ / 実時間より遅い送信 / 不正リクエスト」)

リアルタイム送信は **実時間ペース** です (`0.08` 秒ごとに 80ms 分)。早送りしないので、
10 秒の音声なら約 10 秒かかります。`websockets` が無い場合はメッセージを出して終了します。

### 出力の読み方

```
[transcribe] audio: 3.20 秒 / 16000 Hz / mono / 16-bit / 100 KiB
[transcribe] HTTP 200 / 1.4 秒
[transcribe] sessionId = sas-watch-smoke-...
[transcribe] transcript = こんにちは、テストです。
```

`transcript` が返れば ASR 単体は成功です。アプリで部分文字起こしが出ない時は、
まずこのツールで切り分けてください。

---

## 鍵が無い状態で検証できること (まとめ)

| 検証項目 | コマンド | 実通信 |
|---|---|---|
| useapi.net へ送る JSON / ヘッダ / URL エンコード / SSE パーサ | `muse_smoke.py` (ドライラン) | なし |
| useapi.net のエラーコード別の対処文 | `muse_smoke.py --self-test` | なし |
| WAV の形式・長さ・上限チェック | `asr_smoke.py <wav>` (ドライラン) | なし |
| multipart の組み立て (request + audio) | `asr_smoke.py <wav>` (ドライラン) | なし |
| Meta API のエラー/close code の対処文、伏字処理 | `asr_smoke.py --self-test` | なし |
| 実際の文字起こし・Muse の返答 | 鍵を設定して実行 | あり |

## トラブル時の早見

| 症状 | 確認 |
|---|---|
| `HTTP 401` (useapi) | `USEAPI_TOKEN` が `user:` から始まる全体か。前後の空白・改行 |
| `HTTP 404` (useapi) | Muse アカウントが未接続。`docs/muse-setup.md` の Setup Muse を実施 |
| `HTTP 412` (useapi) | Muse の週次枠切れ。`GET /v1/muse/accounts/{email}` の `quota.percentUsed` / `resetsAt` |
| `HTTP 429` (useapi) | Muse 側のレート、または開いているストリーム過多。約 1 分待つ |
| `HTTP 596` (useapi) | アカウント要対応 (セキュリティチェック / パスワード変更)。muse.ai で確認して再登録 |
| `HTTP 400` (ASR) | WAV 形式 (mono/16-bit/16k or 24k) か 10 分超でないか |
| close 1008 (ASR) | 5 秒以上先読みしていないか、実時間ペースか、handshake が 10 秒以内か |
| `websockets` が無い | `python3 -m pip install websockets`、または `--realtime` を外す |

## 注意

- useapi.net は Meta 非公式の第三者サービスです。実アカウントで試す前に
  `docs/muse-setup.md` のプライバシー/セキュリティ節を読んでください。
- このツールはメッセージを実際の Muse チャットに送るので、テスト用の短い文にする。
- 生成物 (ログ・WAV) はコミットしない。鍵をファイルに保存しない。
