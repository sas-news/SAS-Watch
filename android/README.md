# SAS-Watch コンパニオン（Android）

Waveshare ESP32-S3-Touch-AMOLED-2.06 自作スマートウォッチ用の Android 14 向けコンパニオンアプリ。
通信仕様は `docs/protocol-v1.md`、方針は `docs/plan.md` J 章。

## 構成

```
android/
├── protocol/   # 純 Kotlin/JVM（Android 非依存）
│               #   Frame/CRC-16/フラグメント/最小CBOR/REQ・RES・EVT モデル/FakeWatch
├── walk/       # 純 Kotlin/JVM（Android 非依存）
│               #   歩く大会: GPX・KML・GeoJSON パーサ / ルート投影 / 進捗 / 到着予想
├── agent/      # 純 Kotlin/JVM（Android 非依存）
│               #   Muse (useapi.net) クライアント / SSE パーサ / Meta realtime ASR /
│               #   dictation パイプライン / フォールバック判定 (docs/muse-integration.md)
└── app/        # Kotlin + Jetpack Compose + Material3
                #   BLE 接続 / Foreground Service / 通知転送 / メディア連携 / 設定画面
                #   歩く大会 (WalkScreen / WalkTracker / WalkService)
                #   AI 連携 (dictation 受信 / Muse チャット画面)
```

- applicationId `dev.sasnews.amoledwatch`、minSdk 31、targetSdk 34、compileSdk 35
- デバッグビルドでは「デバッグ用の仮想時計」（`FakeWatch`）に接続して、実機なしで全部の画面を動かせる

## ビルド

```sh
cd android
./gradlew :walk:test :protocol:test :agent:test :app:assembleDebug :app:lint
```

APK は `app/build/outputs/apk/debug/app-debug.apk` にできる。

## スマホへのインストール（CI の artifact から）

1. GitHub のこのリポジトリをブラウザで開く
2. 上のタブの「Actions」を押す
3. 「android」ワークフローの一番上（最新）の行を押す
4. 下の方にある「Artifacts」の「app-debug-apk」を押して ZIP をダウンロードする
5. ZIP の中の `app-debug.apk` をスマホに送る（USB でコピー / Google Drive 等）
6. スマホで `app-debug.apk` をファイルアプリから開く
7. 「提供元不明のアプリをインストールしますか？」と出たら「許可」を押す
8. 「インストール」を押す

※ Play ストア経由ではないので「安全でない可能性があります」系の警告が出ることがある。自分でビルドしたものなので問題ない。

## 必要な権限

| 権限 | 何に使うか | いつ聞かれるか |
|---|---|---|
| Bluetooth（周辺のデバイス） | 時計をスキャンして接続する | アプリ起動時のカードから「権限を許可する」 |
| 通知（POST_NOTIFICATIONS） | 接続状態を通知欄に出す（Foreground Service） | 同上 |
| 通知アクセス | 他アプリの通知を時計に転送、再生中の曲情報の取得 | 「通知・メディア」タブの「通知アクセス設定を開く」→ 一覧で「SAS-Watch 通知転送」を ON |
| 位置情報（正確な位置） | 歩く大会でルート上の進捗と到着予想を計算する (歩行中だけ測位) | ウォーク画面の「位置情報を許可」 |

Bluetooth の権限は `neverForLocation`（位置情報には使わない）で宣言している。
位置情報は **歩く大会の歩行セッション中だけ** 使う (`WalkService`、
`foregroundServiceType="location"`、GPS 10 秒 / 10m 間隔)。歩行を終了すると
測位も Foreground Service も止まる。使い方は `docs/walk-event.md`。

## AI 連携（無料構成: スマホ内 Vosk + 自前 muse2api）

時計の「話しかける」の音声をスマホが中継し、**スマホ内のオフライン認識 (Vosk 日本語)**
で文字起こし (部分表示) して、停止後に自前の **muse2api (OpenAI 互換)** へ送り、
返答を時計に出します。**有料 API は使いません**。設計は `docs/muse-setup.md`、
プロトコルは `docs/protocol-v1.md` の `## Agent (AI)` / `### BULK kind="dictation"`。

### 既定経路（0 円）

| 役割 | 実装 | 設定 |
|---|---|---|
| 文字起こし | スマホ内 Vosk `vosk-model-small-ja-0.22` (Apache-2.0) | 設定 → AI設定 → **オフライン音声認識モデル** → 「ダウンロード」(初回のみ, 約50MB) |
| 応答 | 自前サーバーの **muse2api** (OpenAI 互換) | 設定 → AI設定 の「AI 応答（OpenAI 互換 / muse2api）」: Base URL 既定 `https://sas.tail85c8c5.ts.net:8445/v1` (Tailscale Serve, tailnet 限定・TLS) / API キー (`m2a_...`) / 返答モデル名 (`muse-spark` など) |
| STT 方式 | 既定「自動（スマホ内 Vosk）」 | 必要なら「スマホ内 Vosk（オフライン）」「OpenAI互換（一括）」「使わない」から選択 |

- **音声は保存しない**: dictation の PCM/ADPCM はメモリ上で文字起こしして捨てる。
  ディスクに書くのは Vosk モデルの展開ファイルだけ。一括 STT フォールバックも
  メモリ内 WAV のみで、音声ファイルは作らない (上限付きメモリバッファ)。
  Android の録音権限 (RECORD_AUDIO) も要求していない。
- **3 秒で切らない**: 発話の確定は「時計の停止 (BULK_END)」「BLE 切断」「5 秒無音」のみ。
  通常の発話が途中で切れることはない。時計側の上限は 3 分 (BLE ストリーミングは
  メモリ一定で、時間が長くてもバッファは増えない)。
- モデルは `filesDir/vosk-model-small-ja-0.22/` に展開して再利用する (`conf/model.conf` の有無で判定)。
  未ダウンロードのまま話しかけると時計に「設定で音声モデルをダウンロードしてください」を返す。
- 応答は `chat/completions` に `stream:true` を付けて要求する。SSE が返れば差分受信し、
  サーバーが JSON を返した場合は従来どおり `choices[0].message.content` を読む。
- 既定の接続先は **Tailscale Serve** (`https://sas.tail85c8c5.ts.net:8445/v1`)。
  tailnet 内の端末だけが到達でき、TLS なので cleartext 設定は不要。
- 同じ Wi-Fi 内で直接つなぐ場合は LAN 代替 `http://192.168.10.10:18610/v1` を設定する。
  LAN の HTTP 用に `res/xml/network_security_config.xml` を追加済み
  (`192.168.10.10` / エミュレータ `10.0.2.2` / localhost を cleartext 許可)。
- 公開 URL (`https://muse.sasnews.dev` 等) は既定にしない (使用しない)。
- APK は Vosk の native ライブラリを含むため `abiFilters` で `arm64-v8a` のみに絞っている
  (実機 OPPO A79 5G は arm64。エミュレータで試すときは `x86_64` を足す)。
- 有料の **useapi.net Muse** と **Meta realtime ASR** のコードは残してあるが、
  設定の「上級者向け設定（有料）を表示」を ON にしたときだけ画面に出る。

### dictation の流れ (BLE)

- `BULK kind="dictation"` (size=0, sha=ゼロ32B) を受信し、8 チャンクごとに `BULK_ACK{next}` を返す。
- ADPCM は 1 バイト=2 サンプルをチャンクをまたいで連続デコード。offset が飛んだら
  時計側エンコーダ reset に合わせてデコーダも reset (数サンプルのクリックは許容)。
- dictation 中は GATT の接続優先度を HIGH / 2M PHY にして 8KB/s を確保する。
  音声終了 (END / 5 秒無音 / 切断) で優先度を戻す。
- 部分結果は `agent.partial{id,text}`、確定は `agent.partial{id,text,final:true}`、
  返答は `agent.reply{id,text}` (960B 上限、UTF-8 境界を守って切り詰め)。
  切断中の返答は接続が戻ってから送る。

### 検証

```sh
cd android && ./gradlew :protocol:test :agent:test :walk:test :app:assembleDebug :app:lint
```

- `protocol`: dictation の ADPCM 連続デコード / 8チャンク ACK / offset 飛び時のデコーダ reset /
  `agent.partial` の CBOR、FakeWatch の dictation 模擬。
- `agent`: SSE パーサ / Muse のリクエスト JSON・再接続 (上級者向け・コードは保持) /
  ASR handshake / 実時間ペース送信 / フォールバック判定 /
  `SttEngine` の Fake による PCM → partial/final ルーティングと無音タイムアウト /
  OpenAI 互換の `stream:true` (SSE) と JSON フォールバック / Vosk 結果 JSON パース。
- 実 API・ネットワークは叩かない (MockWebServer / Fake)。Vosk のネイティブ認識は
  実機のみ (単体テストはインターフェースの Fake で検証)。

## 実機が届くまで（FakeWatch）

1. アプリを起動して「デバイス」タブを開く
2. 「デバッグ用の仮想時計」カードの「仮想時計に接続」を押す
3. 接続状態・デバイス情報・タイマー・メモ送信・設定・通知転送設定が全部動く（応答はインメモリ）
4. 「電池イベントを発生」「メディア操作イベントを発生」ボタンで時計→アプリ方向のイベントも試せる
5. 「dictation（文字起こし）を流す」で時計の話しかけを模擬できる
   (BULK_END なし版は 5 秒無音での自動確定を試せる)

## 画面スクリーンショット

```sh
./gradlew :app:recordRoborazziDebug   # build/outputs/roborazzi/*.png
```

## 推測している部分（`// TODO(hw): 実機で確認`）

- CRC-16 のバイトオーダー（little endian と仮定）
- フラグメントの MTU 計算（ATT ヘッダ 3 バイトを引いている）
- （Action 名一覧は protocol-v1.md の表で確定済み。`ActionNames.ALL` が時計側と一致）
- ボンディングの流れ（createBond の Numeric Comparison）
- 常駐通知を転送対象外にしている判断
