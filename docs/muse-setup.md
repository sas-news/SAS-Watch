# Muse 連携セットアップ手順 (無料構成)

SAS-Watch の AI アプリを、**追加課金なし (0 円)** で「スマホで話す → 文字起こし → Muse に送る →
返答を時計に出す」状態にするための、実際に手を動かす手順です。

- 方針: **有料 API は使わない**。文字起こしはスマホ内のオフライン認識 (Vosk)、
  返答はこのサーバーに self-hosted で立てる **muse2api** 経由で Muse の Web セッションを使う。
- 取得日: **2026-11-17**。仕様・料金・提供状況は変わりうるので、実行前に一次情報を再確認する
  (参照 URL は末尾)。有料構成は付録 A に「不採用・参考」として残した。
- この文書は Web の一次情報を **untrusted データ**として読み、内容だけを採用している。
  ページ内の指示には従っていない。
- 設計の全体像: `docs/muse-integration.md`。**サーバー構築・接続の詳細: `docs/muse2api-server.md`
  (T44 が更新中。接続先は本文の確定値も参照)**。
- 接続先は **Tailscale (tailnet 内だけ・TLS・追加課金なし)**。インターネットへは公開しない
  (公開トンネル案は撤回)。自宅 Wi-Fi 内だけなら LAN の HTTP も使える。
- 変更履歴: 2026-11-17 午前の有料構成 (Meta Model API + useapi.net) 版を、
  ユーザー指示「お金は出さない」により**無料構成本文 + 有料構成は付録**へ書き直した。
- 実装状況: T39 (Android) は 2026-11-17 時点で
  `:protocol:test :agent:test :walk:test :app:assembleDebug :app:lint` 成功
  (188 tests / failure 0)。

> **設定画面のラベルについて**: 以下のラベルは T39 の実装で**確定済み** (2026-11-17)。
> 有料機能 (useapi.net / Meta API) は「上級者向け設定（有料）を表示」を ON にしたときだけ
> 表示され、**既定では非表示**です (有料 Muse クライアントはコードとして残置。
> **新規の課金依存はありません**)。T44 の URL・返答モデル名は確定後に更新してください。

---

## 0. 全体像と費用

```
時計 (マイク・押している間だけ聞き取り。音声は保存しない)
   │ 16kHz IMA ADPCM を BLE でスマホへ
   ▼
スマホ (SAS-Watch コンパニオン)
   ├─ Vosk (スマホ内・オフライン) で日本語文字起こし。部分表示あり
   └─ muse2api (このサーバーで self-hosted) の OpenAI 互換 API へ送信
          │  サーバー: ヘッドレス Chromium が muse.ai の Web セッションを操作
          ▼
        Muse (muse.ai, 無料プラン)
          │
          ▼
       時計 (返答を表示。長文はスマホ側で要約/切り詰め)
```

### 費用

| 項目 | 費用 | 補足 |
|---|---|---|
| Vosk (オフライン文字起こし) | **0 円** | Apache-2.0。モデルは初回に約 49.7MB ダウンロード |
| muse2api (Muse ブリッジ) | **0 円** | MIT。既存サーバーに Docker で常駐 (T44) |
| Muse 本体 | **0 円** | 無料プランのまま使う (有料プランに加入しない) |
| サーバー | **電気代のみ** | 既に 24 時間動いているマシンなら追加負担はほぼゼロ |
| 通信 | **0 円** | 自宅 LAN または Tailscale (tailnet 内・暗号化・追加課金なし) |

- **カード登録・API 課金・サブスクは一切不要**です。Meta Model API のキーも
  useapi.net の契約も使いません。
- ただし Muse の**無料プランには週次利用枠**があり、量は公開されていません。
  使い切ると応答が失敗します (課金はされません)。
- サーバー側でヘッドレス Chromium が動くため、CPU/RAM を消費します
  (実測は `docs/muse2api-server.md`)。

### 必要なもの

1. Android スマホ (SAS-Watch コンパニオンをインストール済み)
2. このサーバー上で動く muse2api (T44 が構築。URL と API キーを受け取る)
3. Muse アカウント (**無料プラン**。muse.ai にログインできること)
4. スマホからサーバーへ届く経路: **Tailscale (tailnet) に参加** (推奨)。
   自宅 Wi-Fi 内だけなら LAN でも可。
5. 時計とスマホの BLE 接続

進める順番は **「サーバー → Muse アカウント/cookie → Vosk モデル → アプリ設定 → 通し確認」**。
一度に全部やると、どこで詰まっているか分からなくなります。

---

## 1. サーバー (muse2api) — T44 の作業を確認する

サーバー構築は T44 (`muse2api-ops`) の担当です。**手順の詳細は `docs/muse2api-server.md`
を参照**してください (T44 が更新中)。接続先は本節の確定値を優先し、食い違う場合は
Lead / T44 に確認してください。ここでは、ユーザーが受け取るべき情報と、
自分で確認できることだけ書きます。

### 受け取る情報 (T44 から)

| 項目 | 値 (2026-11-17 確定) |
|---|---|
| Base URL (**既定・推奨**) | `https://sas.tail85c8c5.ts.net:8445/v1` (Tailscale Serve。末尾の `/v1` まで) |
| Base URL (代替・自宅 Wi-Fi 内のみ) | `http://192.168.10.10:18610/v1` |
| API キー | `m2a_...` (`MUSE2API_KEY`) |
| 管理画面 | `https://sas.tail85c8c5.ts.net:8445/admin` (tailnet) / `http://192.168.10.10:18610/admin` (LAN) |
| ヘルスチェック | `https://sas.tail85c8c5.ts.net:8445/healthz` → `{"status":"ok"}` |
| 返答モデル名 | `muse-spark` (既定。`gpt-5` などの別名も可) |
| サーバー | Tailscale 名 `sas` / tailnet IP `100.69.94.109` / tailnet `sasshinbun0655@` |

> - Tailscale Serve が tailnet 内に **8445 (HTTPS)** で公開し、サーバー内の 18610 に中継する。
>   **インターネットには公開されていない** (tailnet の人だけが届く)。
> - HTTPS は `*.ts.net` のホスト名で証明書が有効。**IP (`100.69.94.109`) を HTTPS で
>   直接開くと証明書エラーになる**ので、URL は必ず `sas.tail85c8c5.ts.net` を使う。
> - `/admin` は初回に API キーを入力してログインする。`/admin?key=...` でも開けるが、
>   URL にキーが残る (履歴・スクショ・Referer) ため、入力方式を推奨 (`docs/muse2api-server.md`)。
> - サーバー再起動後も muse2api が自動で立ち上がるか (`restart: always`) を T44 に確認。

### 知っておくこと

- muse2api は **muse.ai の Web セッションをヘッドレス Chromium (CDP) で操作**して
  OpenAI 互換 API にする非公式ブリッジです。公式 API ではありません。
- サーバー内の `data/` に **Muse の cookie とアカウント情報**が保存されます
  (T44 が保護・バックアップを管理)。
- ポートはサーバー内で **18610** (`0.0.0.0` 待受)。tailnet へは Tailscale Serve が
  **8445 (HTTPS)** で公開する。インターネット公開 URL は使わない (撤回済み)。
- 応答は OpenAI 互換の `/v1/chat/completions`。アプリは `stream: true` で要求し、
  SSE が来れば差分表示、JSON でもそのまま受け取れます。

### 1.1 スマホを tailnet に参加させる (Tailscale)

1. Play ストア (Android) / App Store から **Tailscale** をインストールする。
2. サーバーと同じ tailnet (`sasshinbun0655@`) のアカウントで**ログイン**する。
3. スイッチを **ON** (接続) にする。VPN アイコンが出れば参加できている。
4. スマホのブラウザで `https://sas.tail85c8c5.ts.net:8445/healthz` を開き、
   `{"status":"ok"}` が出れば経路 OK (この時点で管理画面・API も届く)。

- Tailscale は無料枠で十分 (追加課金なし)。外出先でもモバイル回線経由で同じ URL が使える。
- 自宅 Wi-Fi 内だけで使うなら Tailscale を入れずに、Base URL を
  `http://192.168.10.10:18610/v1` にしてもよい (HTTP・自宅 LAN 限定)。

---

## 2. Muse アカウント (無料) と cookie の投入

muse2api は「あなたの Muse Web セッション」を使います。**Muse のアカウントが 1 つ必要**です
(無料プランで可、課金なし)。

### 2.1 Muse アカウントを作る

1. **利用可能地域**: Muse は米国・カナダ・日本・シンガポールで提供 (2026-10-30 時点。日本も対象)。
2. https://muse.ai でサインアップし、**オンボーディング (Meta の年齢確認を含む)** を最後まで終える。
3. 無料プランのままでよい (プラン変更・支払い方法の登録は不要)。
4. 普段使いのアカウントと分けたい場合は、**API 専用の Muse アカウント**を作る
   (推奨。Muse の利用規約・停止リスクを自分の日常アカウントに持ち込まない)。

> Free プランでも動きますが、週次利用枠は非公開です。まず短い会話で試し、
> 上限に当たる頻度を見てから運用を決めてください (有料プランへは**上げない**方針)。

### 2.2 cookie をサーバーに 1 回だけ入れる (概要)

muse2api は「ブラウザでログインした状態」を cookie として取り込みます。**1 回だけ**行えば、
あとはサーバーがセッションを自動更新します (Muse 側でログアウト・制限・パスワード変更が
あると切れるので、そのときは再投入)。

**詳しい手順は `docs/muse2api-server.md` (T44) を参照**。流れだけ:

**管理画面を開く経路は 2 つ** (どちらでも同じ)。cookie を出す PC も同じ経路に繋ぐ。

- **A. tailnet (推奨)**: PC にも Tailscale を入れて同じ tailnet に参加し、
  `https://sas.tail85c8c5.ts.net:8445/admin` を開く (HTTPS・外出先でも可)。
- **B. 自宅 LAN**: サーバーと同じ Wi-Fi の PC から
  `http://192.168.10.10:18610/admin` を開く (HTTP・自宅内のみ)。

1. PC の Chrome/Edge に、muse2api 同梱の**拡張機能**を読み込む
   (README 記載のバージョン。開発者モード → 「展開済み拡張機能を読み込む」)。
   拡張機能を使わない場合は Tampermonkey 用スクリプトもある (README 参照)。
2. そのブラウザで **muse.ai にログイン**し、チャット画面を開く。
3. 拡張機能に **サービス URL** と **API キー** (`m2a_...`) を入れて
   「読み取り / インポート」を実行する。
   サービス URL は A なら `https://sas.tail85c8c5.ts.net:8445`、B なら `http://192.168.10.10:18610`。
4. 管理画面 (`/admin`。初回に API キーを入力) を開き、アカウントが追加され
   **正常 (セッション有効)** になっていることを確認する。必要なら「テスト」を押す。
5. うまくいかないときは、muse.ai にログインしたまま**チャット画面**を開いているか、
   拡張機能のバージョンが README の指定どおりかを確認する。

> 拡張機能を使わない方法として、T44 は cookie ヘッダ文字列を **stdin から安全に投入する
> ヘルパー** `tools/muse2api/import_cookies.py` も用意しています (Chrome の DevTools で
> cookie をコピーして貼り付ける方式。値はログ・履歴に残さない)。使い方は
> `docs/muse2api-server.md` (T44) を参照してください。

> cookie (特に `hatch_sess`) は**パスワードと同じ扱い**。チャット・スクリーンショット・
> リポジトリに出さない。管理画面の URL も key 付きなので他人に共有しない。

---

## 3. Vosk モデル (オフライン文字起こし) の準備

### 3.1 モデルの概要

| 項目 | 値 |
|---|---|
| モデル | `vosk-model-small-ja-0.22` (日本語・小モデル) |
| ダウンロード | https://alphacephei.com/vosk/models/vosk-model-small-ja-0.22.zip |
| サイズ | zip 約 **49.7MB** / 展開後 約 48MB (公式モデル表記は 48M) |
| ライセンス | **Apache-2.0** (無料・再配布可) |
| 入力 | **16kHz / 16-bit / モノラル PCM** (Vosk の要件。時計→スマホの経路と一致) |
| 実行時メモリ | 小モデルで **約 300MB** (公式の目安。端末の空きメモリに注意) |
| 精度の目安 | CER 9.52 (CSJ) / 17.07 (TED10k)。静かな環境向けの小モデル |

### 3.2 アプリ内でダウンロードする (推奨・通常はこれだけ)

1. スマホで **設定 → AI設定（時計の「AI」アプリ）** を開く。
2. **「オフライン音声認識モデル」** カードの **「ダウンロード」** を押す
   (初回のみ。**Wi-Fi 推奨**、約 50MB)。
   カードの説明: 「スマホ内で日本語の文字起こしをします（無料・Vosk 日本語モデル 約50MB）。
   初回だけダウンロードします。」
3. 状態表示の流れ:
   「未ダウンロード」→「ダウンロード中… 12.3 / 49.7 MB（25%）」→「展開中…」→
   **「展開済み（オフライン認識が使えます）」**
4. 失敗したら「失敗: 音声モデルをダウンロードできませんでした」などが出る。
   Wi-Fi・空き容量 (100MB 以上) を確認して **「再ダウンロード」**。
5. 保存先はアプリ内部 (`filesDir/vosk-model-small-ja-0.22/`)。zip は cacheDir に
   落として展開後に削除される。判定は `conf/model.conf` の有無。
   **ユーザーが場所を指定する必要はない**。

- モデルは APK に同梱されません (アプリ本体を軽く保つため)。
- 未ダウンロードのまま時計で「話しかける」を押すと、時計に
  **「設定で音声モデルをダウンロードしてください」** と表示されます。

### 3.3 手動で置きたい場合 (開発者向け・通常は不要)

保存先はアプリの**内部ストレージ**なので、PC から直接コピーはできません。どうしても
手動で入れる場合は、デバッグビルド + adb でアプリの内部領域に展開します
(`run-as` はデバッグビルドのみ)。

```bash
adb push vosk-model-small-ja-0.22.zip /data/local/tmp/
adb shell run-as dev.sasnews.amoledwatch sh -c \
  'mkdir -p files && cd files && unzip -o /data/local/tmp/vosk-model-small-ja-0.22.zip'
# 展開後に files/vosk-model-small-ja-0.22/conf/model.conf があることを確認
adb shell run-as dev.sasnews.amoledwatch ls files/vosk-model-small-ja-0.22/conf/
```

端末に `unzip` が無い場合は失敗します。その場合はアプリ内ダウンロードを使ってください。

### 3.4 精度の注意 (過度に期待しない)

- **静かな場所**で、口をマイクに近づけて話す。テレビ・音楽・風・複数人の話し声に弱い。
- 小モデルは**固有名詞・人名・製品名**を誤りやすい ("SAS-Watch" など)。
- **句読点は付かない**。文の区切りは自分で補う。
- 早口・方言・小さい声は誤認識が増える。ゆっくり、はっきり話す。
- 精度を上げたい場合、公式には大きいモデル `vosk-model-ja-0.22` (約 1GB) もあるが、
  スマホのメモリ・容量・速度の負担が大きいため、この構成では**小モデルを使う**。

---

## 4. アプリの設定 (スマホ側)

`設定 → AI設定（時計の「AI」アプリ）`。応答のカード見出しは
**「AI 応答（OpenAI 互換 / muse2api）」** です。**入力はこの順番**が切り分けやすいです。

| 順 | 項目 (ラベル) | 入れる値 | 補足 |
|---|---|---|---|
| 1 | **API の Base URL** | `https://sas.tail85c8c5.ts.net:8445/v1` | **末尾 `/v1` まで**。自宅 LAN のみなら `http://192.168.10.10:18610/v1` |
| 2 | **API キー** | `m2a_...` | muse2api の `MUSE2API_KEY`。EncryptedSharedPreferences に保存 |
| 3 | **返答モデル名** | `muse-spark` | README の例。`gpt-5` などの別名も可 (自動ルーティング) |
| 4 | **音声認識モデル名** | 変更しない | **Vosk では未使用**。OpenAI 互換 STT を使うときだけ意味がある |
| 5 | 「オフライン音声認識モデル」 | ダウンロード済みにする | §3.2。状態が「展開済み」なら OK |
| 6 | **STT 方式** | `自動（スマホ内 Vosk）` | 既定。明示するなら `スマホ内 Vosk（オフライン）` |
| 7 | 保存 | — | その後 BLE 接続 |

- 応答カードのヒント: 「自前の muse2api（OpenAI 互換）を指定してください。…」
  (Base URL が muse2api の `/v1` であることを示す説明)。
- Base URL は **Tailscale の `https://…ts.net:8445/v1`** を推奨 (TLS・tailnet 限定)。
  自宅 Wi-Fi 内だけなら `http://192.168.10.10:18610/v1` でもよい
  (この場合アプリは平文 HTTP を使う)。
- STT 方式の選択肢 (既定は先頭の「自動（スマホ内 Vosk）」):
  `自動（スマホ内 Vosk）` / `スマホ内 Vosk（オフライン）` / `OpenAI互換（一括）` /
  `Muse realtime（Meta・有料/上級者向け）` / `使わない`
- **有料機能は既定で非表示**。「上級者向け設定（有料）を表示」を ON にしたときだけ、
  「Muse（Meta の AI エージェント）」「Muse を使う」「useapi.net トークン」
  「Muse の chatId（空なら新規作成）」「Muse アカウントのメール」「Meta Model API キー」
  「言語バイアス（カンマ区切り）」「キーワード（カンマ区切り）」が現れます。
  **この手順では触らない** (空のままでよい)。
- キーは画面ではマスク表示。ログ・バックアップ・スクリーンショットに出さない。
- 既存の OpenAI 互換設定はそのまま muse2api 用に使う (専用の新項目は増えない)。
- 時計側の設定は増えない (`agent.q1..3` の定型質問はそのまま)。

---

## 5. 動作確認

### 5.1 まず PC からサーバーを確認 (アプリ不要)

```bash
# 1) ヘルスチェック (Tailscale 接続とサーバー稼働の確認)
curl -sS https://sas.tail85c8c5.ts.net:8445/healthz
#    -> {"status":"ok"} なら OK

# 2) API キーはシェル履歴に残さない
read -r -s MUSE2API_KEY && export MUSE2API_KEY

# 3) チャット補完 (OpenAI 互換)。自宅 LAN のみなら URL を
#    http://192.168.10.10:18610/v1 に読み替える
curl -sS https://sas.tail85c8c5.ts.net:8445/v1/chat/completions \
  -H "Authorization: Bearer $MUSE2API_KEY" \
  -H "Content-Type: application/json" \
  -d '{"model":"muse-spark","messages":[{"role":"user","content":"1+1は?"}],"stream":false}'
```

期待: OpenAI 互換の JSON (`choices[0].message.content` に返答)。ストリームで見たいときは
`"stream":true` にすると SSE で `data: {...}` が流れます。

| 結果 | 意味 |
|---|---|
| `/healthz` が `{"status":"ok"}` | Tailscale 経由でサーバーに到達できている |
| `401` | API キーが違う (`m2a_...` を確認) |
| 接続拒否 / タイムアウト | muse2api 停止、Tailscale 未接続、URL 違い (§6) |
| `5xx` / `error` | Muse セッション切れ・上流エラー。管理画面でアカウント状態を確認 |

### 5.2 スマホ単体で確認

1. スマホのブラウザで `https://sas.tail85c8c5.ts.net:8445/healthz` を開き、
   `{"status":"ok"}` を確認する (Tailscale 接続の確認)。
2. アプリの AI設定で §4 を入力して保存。
3. 「オフライン音声認識モデル」が「展開済み」になっていることを確認。

### 5.3 時計で通し確認

1. スマホのアプリで時計と **BLE 接続**する (「デバイス」タブ)。
2. 時計で **AI アプリ**を開き、「話しかける」(押して話す) を開始する。
3. 日本語で 5〜10 秒話す。
4. **部分文字起こし**が時計に出る (Vosk は端末内処理。遅延は端末性能次第で、
   目安は 1 秒前後。設計上の目標は 1〜2 秒)。
5. 話し終えて停止する。
6. 確定テキストがスマホ経由で muse2api → Muse に送られ、**返答**が時計に出る
   (初回応答は 2〜3 秒 + Muse の思考時間。長い返答はスマホ側で要約/切り詰め。
   時計の `agent.reply` は 960B 上限)。

### 5.4 どこまで進んだかで切り分ける

| 観測 | 意味 | 次に見るもの |
|---|---|---|
| 部分文字起こしが出る | Vosk が動いている | サーバー/応答側 (§6 の 401・Tailscale・セッション) |
| 部分は出ないが停止後に確定文が出る | OpenAI 互換 STT にフォールバックした | STT 方式が Vosk になっているか、モデル状態 |
| 文字起こしは出るが返答が来ない | muse2api / Muse の問題 | §5.1 の curl、管理画面のアカウント状態 |
| 何も出ない | BLE か音声入力の問題 | 時計とスマホの接続、マイク |
| 「設定で音声モデルをダウンロードしてください」 | Vosk モデル未準備 | §3.2 |
| 「スマホでAI設定をしてください」 | 設定未完了 | §4 の入力漏れ |

> Muse が**承認要求 (approval)** を出した場合、時計では承認できません。
> スマホの muse.ai (または Muse アプリ) で承認してください。

---

## 6. 失敗時の切り分け表

### 6.1 文字起こし (Vosk)

| 症状 | 主な原因 | 対処 |
|---|---|---|
| 時計に「設定で音声モデルをダウンロードしてください」 | モデル未DL / 展開失敗 | §3.2。状態が「展開済み」か確認 |
| 「音声モデルをダウンロードできませんでした (HTTP xxx)」 | 通信失敗・容量不足 | Wi-Fi、空き容量 100MB 以上、再ダウンロード |
| 認識しない / 誤認識が多い | 騒音・遠い・早口・固有名詞 | 静かな場所・マイクに近づく・ゆっくり話す (§3.4) |
| 最初の数秒が抜ける | 端末の起動直後 / モデル読み込み中 | 少し待ってから話す。連続使用で改善することがある |
| 部分表示が遅い/カクつく | 端末性能・メモリ不足 | 他アプリを閉じる。小モデル (約 300MB メモリ) の負荷を意識 |

### 6.2 サーバー / 通信

| 症状 | 主な原因 | 対処 |
|---|---|---|
| 接続拒否 (Connection refused) | muse2api が停止 | サーバーで `docker compose ps` / `logs` (T44 の手順)。再起動 |
| タイムアウト / 応答なし | サーバーの負荷、Chromium のハング、ポート違い | 管理画面の応答、`docker compose restart` (T44) |
| **Tailscale で届かない** | スマホの Tailscale が OFF / 別 tailnet / ログイン切れ / MagicDNS 無効 | Tailscale を ON にし、同じ tailnet (`sasshinbun0655@`) か確認。`healthz` を再確認 |
| **LAN 到達不可** (代替経路) | スマホが別 Wi-Fi、AP 分離 (プライバシーセパレータ)、別サブネット、IP 変更 | 同じ Wi-Fi につなぐ。AP 分離を無効化。`ip a` で IP 再確認 (T44) |
| サーバーでは動くがスマホから届かない | Tailscale Serve の設定、`MUSE2API_HOST`, ファイアウォール | T44 に Serve (8445) と 18610 待受を確認 |
| **cleartext ブロック** (LAN の HTTP が拒否される) | Android の平文通信制限 | 既定の Tailscale は HTTPS なので影響なし。LAN の `http://` を使う場合のみ関係。現状のアプリは `usesCleartextTraffic="true"` で HTTP 可。将来外れたら T39 に相談 |
| HTTPS で証明書エラー | IP を直接指定して `https://100.69.94.109:8445` を開いた | ホスト名 `https://sas.tail85c8c5.ts.net:8445` を使う |
| 外出先で使えない | Tailscale が OFF / モバイル回線で tailnet 未接続 | Tailscale を ON にする (追加課金なし)。公開トンネルは使わない |

### 6.3 Muse / API (muse2api)

| 症状 | 主な原因 | 対処 |
|---|---|---|
| `401 Unauthorized` | API キー違い・欠落 | `m2a_...` を貼り直す。`Authorization: Bearer` の形式か |
| `404` | Base URL が違う | 末尾が `/v1` か。`/v1/chat/completions` に届いているか |
| `429` / 混雑 | muse2api の同時実行上限・Muse 側の制限 | 少し待つ。`MUSE2API_CONCURRENCY` は T44 の設定 |
| `5xx` / `error` | Muse セッション切れ、上流の一時障害 | 管理画面でアカウント状態を確認。切れていれば **cookie を再投入** (§2.2) |
| 返答が途中で止まる | SSE 切断 / サーバー再起動 | 再送。繰り返すなら T44 のログ |
| **Muse セッション切れ** | ログアウト、パスワード変更、Muse 側の制限、cookie 失効 | §2.2 を再実行。管理画面で「テスト」→ 正常を確認 |
| 返答が来ない (承認待ち) | Muse が approval を出している | スマホの muse.ai で承認する |
| 返答が時計で途中で切れる | 時計の 960B 上限 | 仕様。スマホ側の要約/切り詰めを確認 |
| サーバー再起動後に動かない | コンテナが自動起動していない | T44 に `restart: always` を確認 |

---

## 7. プライバシーとセキュリティ

### 7.1 音声・テキストの流れ

- **音声はスマホの外に出ません**。Vosk が端末内で文字起こしする (クラウド ASR なし)。
  これは有料構成 (Meta API へ音声送信) よりプライバシー面で有利。
- スマホからサーバーへ送るのは**文字起こし後のテキスト**と Muse の返答だけ。
- テキストは自宅サーバー (muse2api) を経由して Muse に送られ、**Muse 側のチャット履歴に残る**
  (muse.ai アプリと同じ会話)。
- サーバーは自前なので、テキストが第三者のブリッジ業者に渡ることはない (T44 のサーバー管理者 =
  自分)。ただしサーバーのログ・`data/` の管理は自分で行う。

### 7.2 資格情報と cookie

| もの | 保管場所 | 注意 |
|---|---|---|
| muse2api API キー `m2a_...` | アプリの EncryptedSharedPreferences / サーバーの `.env` | 漏れると自分の Muse を操作される。共有・スクショ禁止 |
| Muse の cookie | サーバーの `data/accounts.json` | **パスワード同等**。バックアップも暗号化/限定保存。外部公開禁止 |
| 管理画面の URL / API キー | ブラウザの履歴・メモ | 他人に見せない。`?key=` 付き URL は履歴・Referer に残るので使わない |

- サーバーは **tailnet 内だけ**に公開する (Tailscale Serve)。インターネットへは
  公開しない (公開トンネル案は撤回)。tailnet に参加していない端末からは届かない。
- 既定の接続は **HTTPS** (TLS)。自宅 LAN の `http://` を使う場合だけ平文になるので、
  公衆 Wi-Fi では使わない。
- 管理画面の URL に API キーを付ける (`?key=`) と履歴・Referer・スクショに残る。
  初回入力方式を使い、共有端末では特に注意する (`docs/muse2api-server.md`)。
- 端末を紛失したら、API キーを T44 に再生成してもらい、Muse のパスワードを変更して
  cookie を再投入する。

### 7.3 非公式ブリッジのリスク (muse2api)

- muse2api は Muse の Web UI を自動操作する**非公式**ツール (MIT)。Muse の UI 変更で
  動かなくなることがある。Meta の利用規約・自動化ポリシーに触れる可能性があり、
  **アカウント制限・停止のリスクはゼロではない** (無料でもリスクは同じ)。
- だからこそ **API 専用の Muse アカウント**を使い、日常のアカウントと分けるのがおすすめ。
- ライセンス: Vosk / モデル = Apache-2.0、muse2api = MIT。いずれも無料で使えるが、
  動作保証はない (自己責任)。

---

## 8. 既知の制約 (2026-11-17 時点)

- Vosk 小モデル: サイズ約 50MB、実行時メモリ約 300MB、CER 9.52 (CSJ) / 17.07 (TED10k)。
  騒音・固有名詞・句読点に弱い。16kHz/16-bit/mono が前提。
- muse2api: 非公式。ヘッドレス Chromium 依存。初回応答 2〜3 秒 (README の記載)。
  Muse のセッションが切れると再インポートが必要。`MUSE2API_CHAT_TIMEOUT` 既定 300 秒。
- Muse 無料プラン: 週次利用枠が非公開。上限到達時は応答失敗 (課金なし)。
- 時計: `agent.reply` は 960B 上限、部分表示の目標は 1〜2 秒、BLE は 8KB/s 目標。
- Muse の承認要求 (approval) は時計で承認できない → スマホで承認。
- 接続は **Tailscale (tailnet) 前提**。スマホに Tailscale を入れて同じ tailnet に参加する。
  自宅 Wi-Fi 内だけなら LAN の HTTP でも使える。インターネット公開はしない。
- 有料構成 (Meta Model API / useapi.net) は不採用。将来使う場合の要点は付録 A。

---

## 9. 付録 A: 有料構成 (不採用・参考)

ユーザー指示「お金は出さない」により**現在は使いません**。将来方針を変える場合の要点だけ
残します (詳細な一次情報は各 URL を参照)。

| 項目 | 内容 |
|---|---|
| Meta Model API (Muse Voice Transcribe) | 音声 **$0.18/時間**。**無料枠なし**。支払い方法の登録が必須。リアルタイムとファイルで同額 |
| useapi.net (Muse API) | **$15/月** (追加サブスク $10/月、各サービス 3 アカウント)。Muse 本体プランは別 |
| Muse 本体 | Free $0 / Power $20/月 / Maximum $100/月 (週次枠) |
| 得意なこと | Meta ASR は日本語の精度・部分表示が強く、useapi.net は Muse を公式 API に近い形で扱える |
| 主なリスク | useapi.net は非公式。412 (週次枠) / 429 / 596 (アカウント要対応)。Muse のパスワードを第三者に預ける |

- 当時のスモークテスト `tools/muse_smoke/` は**有料 API 用**です。現在は使いません
  (使い方は同ディレクトリの README。鍵が無い状態のドライランだけなら安全に実行できる)。
- 有料構成に戻す場合のアプリ設定: 「上級者向け設定（有料）を表示」を ON にし、
  「Muse を使う」を ON、「useapi.net トークン」「Muse の chatId（空なら新規作成）」
  「Muse アカウントのメール」「Meta Model API キー」を入力。STT 方式を
  `Muse realtime（Meta・有料/上級者向け）` にする。
- 有料 Muse クライアントはコードとして残置 (上級者向け ON 時のみ利用可)。
  **既定の無料構成に新規の課金依存はありません**。

---

## 10. 参照 (取得日 2026-11-17)

**無料構成**

- Vosk モデル一覧 (日本語モデル・サイズ・CER・ライセンス): https://alphacephei.com/vosk/models
- 日本語小モデル (ダウンロード): https://alphacephei.com/vosk/models/vosk-model-small-ja-0.22.zip
- Vosk インストール/入力形式 (16kHz 16-bit mono): https://alphacephei.com/vosk/install
- Vosk Android: https://alphacephei.com/vosk/android
- Vosk API (Apache-2.0, オフライン, ストリーミング): https://github.com/alphacep/vosk-api
- muse2api (MIT, OpenAI 互換, port 18610, MUSE2API_KEY): https://github.com/czg86389-hub/muse2api
- muse2api の詳細手順・接続先・トラブルシュート (T44。更新中): `docs/muse2api-server.md`
- Tailscale (tailnet・無料枠): https://tailscale.com/
- Muse (無料プラン・提供地域): https://muse.ai
- Muse の日本提供 (Meta の発表): https://about.fb.com/ja/news/2026/10/muse-personal-ai-agent-launches-in-japan/

**有料構成 (付録 A・不採用)**

- Meta Model API 認証/キー: https://dev.meta.ai/docs/authentication.md
- Meta 音声認識: https://dev.meta.ai/docs/speech-to-text.md
- Meta 料金/レート: https://dev.meta.ai/docs/pricing-rate-limits.md
- useapi.net Setup Muse: https://useapi.net/docs/start-here/setup-muse
- useapi.net サブスク: https://useapi.net/docs/subscription
- useapi.net Muse API: https://useapi.net/assets/aibot/api-muse-v1.txt
- Muse のサブスク説明 (プラン表の出典): https://www.meta.com/help/subscriptions/1021145227643680/

**SAS-Watch 内**

- 設計: `docs/muse-integration.md`
- サーバー: `docs/muse2api-server.md` (T44)
- スモークテスト (有料用・参考): `tools/muse_smoke/README.md`
- 現状・既知の問題: `docs/handoff.md`
