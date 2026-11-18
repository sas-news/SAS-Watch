# handoff.md — 現状 (2026-11-17)

次に作業する人が最初に読む「今どうなっているか」。残タスクと方針は `docs/roadmap.md`。
過去の経緯 (2026-10-15 までの引き継ぎ文書、T14 監査など) は `docs/archive/handoff-2026-10-15.md`。

## 要点

### 2026-11-18: Muse Gadget SDK の調査と s3-206 ビルド (T46/T47)

- Meta 公式の **Muse Gadget SDK** (`facebookincubator/muse-gadget-sdk`, Apache-2.0,
  pinned `ec976f0`) が **SAS-Watch と同じ Waveshare ESP32-S3-Touch-AMOLED-2.06 を
  `s3-206` として公式サポート**していると判明。M5Stack StopWatch と同じ仕組み。
- **T46**: ESP-IDF v6.0.3 を別途入れ、公式ファームを s3-206 向けにビルド成功。
  `muse-gadget.bin` 3,411,968B (app スロット空き 19%)、CJK フォント on
  (日本語字幕)、realtime voice off。`tools/muse-gadget/` に build/flash スクリプト、
  `docs/muse-gadget.md` に手順。実機書き込みは未 (USB 未接続)。
  flash.sh は 32MB 全量バックアップ → 書き込み → 復元案内まで実装。
- **T47**: 公式の Muse チャット層を SAS-Watch ファームへ移植する実現性を調査
  (`docs/muse-port.md`)。音声は **16kHz PCM16 mono の音声ノート**でよく、
  IDF 5.5.1 のまま chat/Noise 層を載せられる見込み。flash 追加は 90〜110KB
  (CJK フォント 1.23MB は SAS-Watch の binfont があるので不要)。ただし
  **VM デバイストークンは Muse アプリの BLE ペアリングでしか得られず**、
  2 本目の TLS+Noise が PSRAM 1.2MB + 内部 RAM 30〜50KB、Wi-Fi 常時接続の
  電池未計測が壁。推奨は C (現行スマホ+muse2api) 維持 + A (公式ファーム) を
  M0 で試す。B-min は M1 (資格情報スパイク) 通過時のみ 4〜7 人日。
- ユーザー作業待ち: 実機 USB 接続、SDK token 発行 (gadgets.muse.ai)、
  Muse アプリ Developer mode でのペアリング。

- **T49 (コンパイル・スパイク)**: SDK の chat 5 ソース + noise_core 7 ソースを
  **ESP-IDF 5.5.1 で無改変ビルド成功** (IDF6 移行不要)。SAS-Watch への増分は
  flash +77,763B / 静的 DIRAM +3,090B / PSRAM +3,864B / IRAM 0B。
  必要スタブ 20 関数 + minimp3 シム。B-min は技術的には GO で 4〜6 人日。
  残る GO 条件は M1 (Muse の VM/デバイストークンを外部調達できるか) と
  実機 TLS 内部 RAM。詳細: `docs/muse-chat-spike.md`。
- **T50 (Phase 1 統合)**: `firmware/components/muse_link/` に SDK chat/Noise 層を
  Apache-2.0 コピーで取り込み、SAS-Watch アダプタ (NVS `muse` 読み出し/Wi-Fi 状態/
  字幕 20 桁×8 行) を実装。`CONFIG_WATCH_ENABLE_MUSE_LINK` 既定 off。
  **off のビルドはサイズ差分 0**、on で flash +97,248B / 静的 DIRAM +5,776
  (空き 14,713B) / 静的 PSRAM +4,704 / IRAM 0。off/on ともビルド成功、
  core 273/273、sim 監査 0。Phase 2 (資格情報投入・Wi-Fi on-demand・
  セッション+RAM トリム+実機 TLS 実測・音声・UI 字幕) は 4〜6 人日で M1 待ち。
  詳細: `docs/muse-bmin.md`。
  - 既知: `firmware/dependencies.lock` の旧絶対パスで現パスの素ビルドが
    configure 失敗 (CI は sed 済み)。T51 候補。
- **T52 (Phase 2 ① 資格情報)**: `muse_link_provision/clear/get_status/
  console_line` を追加。NVS は SDK 互換 (`muse` namespace, keys host/vm/token,
  token 1023B)。シリアルは大文字 `M` で行モード (`muse status` /
  `muse set host|vm|token` / `muse clear`)。token はログ・status に出さない。
  off build はサイズ不変、on は 0x22dd50 (+100,816B from off、
  T50 から +3,568B)。大きいバッファは PSRAM へ移して DIRAM は T50 より減。
  パーサのホストテスト 22 ケース (`firmware/components/muse_link/test/`)。
  M1 手順は `docs/muse-m1.md` (公式ファームの NVS から nvs_tool.py で
  host/vm/token を抽出 → SAS-Watch へ投入)。残 Phase 2 は 3.5〜5.5 人日。
- **T53 (M1 ツール)**: `tools/muse-gadget/export_credentials.py` (NVS から
  host/vm/token を抽出、値を出さない) と `m0_m1.sh` (既定 dry-run の
  M0→M1 ランブック、feed-serial は応答確認後に投入) を追加、テスト 50 件 OK。
  発見: flash.sh のバックアップは M0 前なので資格情報を含まない →
  ペアリング後に NVS を再ダンプ。`M` 行モードは 1 行ごとに M が必要。
- **T54 (Phase 2 ②④)**: Wi-Fi on-demand (所有権方式、アイドル 60 秒) と
  16kHz PCM→WAV/base64 音声アダプタ (SDK 形式、逐次変換、上限 60 秒) を実装。
  off 0x215380 不変、on 0x22e730。ホストテスト all pass、core 273、sim 監査 0。
  Phase 2 残りは ③ セッション+RAM+実機TLS / ⑤ UI字幕 / ⑥ 回帰で 2.5〜4 人日。




### 2026-11-17 フェーズ: Muse 連携 + リアルタイム文字起こし + 軽量化 (実装済み・実機確認待ち)

ユーザー方針: 実用化を進める。**軽量化 = 時計ファームの軽量化**、
**Muse = Meta の personal AI agent に直接メッセージ**、**有料 API は使わない (0円)**、
**音声は保存しない (録音ではなく音声入力)**、まず文字起こし + Muse 連携。

- **時計: リアルタイム音声入力 (T38)**
  - AI 画面の「話しかける」で、聞き取り開始と同時に 16kHz IMA ADPCM を
    BULK kind=`"dictation"` でスマホへストリーミング (PSRAM 16KB リング、ファイル無し)。
    スマホから REQ `agent.partial{id,text,final?}` で部分文字起こしが届き、聞き取り中に表示。
    停止で BULK_END → スマホが確定 → `agent.partial{final:true}` → `agent.reply`。
  - **音声は保存しない** (dictation 中は fopen/ADP1/tmp を作らない)。上限は
    `kAgentMaxRecordSec = 180` 秒の安全弁のみ (3 秒/30 秒の自動終了はしない)。
    UI は「聞き取り中 X 秒 (最大 180)」「音声は保存されません」。
  - dictation 中だけ BLE 接続間隔 15〜30ms + 2M PHY を要求し、停止で 100〜200ms に戻す。
  - 検証: core **273/273**、sim 監査 0 件、firmware build 成功。実機未確認は
    BLE 実効 8KB/s の持続・部分表示遅延 1〜2 秒・電池 (`TODO(hw)`)。
- **スマホ: Vosk オフライン文字起こし + muse2api (T39)**
  - BULK dictation 受信 → ADPCM を連続デコード (offset 飛びで decoder reset) →
    **Vosk 日本語** (`vosk-model-small-ja-0.22`, 49.7MB を初回 DL, 完全オフライン) →
    partial/final を時計へ。5 秒無音/END/切断で確定。音声はメモリのみで破棄。
  - 応答は **自前サーバーの muse2api (OpenAI 互換)** に `stream:true` で接続。
    Base URL 既定 `https://sas.tail85c8c5.ts.net:8445/v1`、モデル `muse-spark`。
    LAN 代替 `http://192.168.10.10:18610/v1`。
  - 有料クライアント (Meta realtime ASR / useapi.net Muse) はコード残置だが
    「上級者向け設定（有料）」ON のときだけ表示。新規課金依存なし。
  - 検証: Android 188 テスト (protocol 66 / agent 43 / walk 53 / app 26)、
    assembleDebug・lint (Error 0) 成功。APK は arm64-v8a のみで **39.6MiB**。
- **サーバー: muse2api (T44)**
  - `czg86389-hub/muse2api` v1.6.11 を Docker で常駐 (リポジトリ外
    `services/muse2api`, port 18610, `MUSE2API_KEY`, data/ 永続化, restart:always)。
    アイドル ~46MiB/0.14%CPU。
  - **公開トンネルは使わない** (muse.sasnews.dev は撤回済みで 404)。既定は
    **Tailscale Serve の tailnet 限定 HTTPS** `https://sas.tail85c8c5.ts.net:8445/v1`。
    スマホに Tailscale アプリ + 同じ tailnet (`sasshinbun0655@`) が必要。
  - 管理画面は `/admin` (`/dashboard` は 404)。Muse の cookie を 1 回投入する
    (PC を tailnet に入れて `/admin`、または LAN から)。投入手順は
    `docs/muse2api-server.md`、ヘルパー `tools/muse2api/import_cookies.py`。
  - **未完了 (ユーザー作業)**: スマホの tailnet 接続確認、cookie 投入、実チャット確認。
- **軽量化 (T41/T43)**
  - 監査: `docs/lightweight.md` (flash/SRAM/PSRAM/描画/電池、S/A/B 優先度)。
  - 内蔵文字盤フォント 26 個を数字+記号の 14 グリフにサブセット化し
    **862KB → 123KB (−738KB)**。app bin 2,917,104 → 2,183,984B、
    **app 空き 30.4% → 47.9%**。sim 75 画面の before/after 差分 0、監査 0、
    line_height/カーニングを固定して見た目維持。実機の数字盤目視が残り。
  - 次の候補: cert bundle / WiFi 未使用機能 / ログ抑制、Theme の PSRAM 化、
    bake チャンク化、light sleep 復活 (docs/lightweight.md §2.3/§6)。
- **セットアップ (ユーザー向け)**: `docs/muse-setup.md` (0円構成) と
  `docs/muse2api-server.md`。Muse 連携の設計は `docs/muse-integration.md`。
- **未コミット**: このフェーズの変更はまだ git commit していない (Lead がまとめる)。


- 歩く大会モード (ウォーク、T37) を追加 (2026-10-29)。**スマホがルート・測位・
  進捗/ETA の正**で、時計は表示だけ。時計はウォーク画面が Active で見えている間だけ
  10 秒に 1 回 EVT `walk.query` を送り、スマホが REQ `walk.state` で返す
  (Dim / ScreenOff / 別画面では送らない)。スマホは歩行セッション中だけ GPS (10s/10m)
  + Foreground Service。GPS は「時計/アプリ画面を見ている間だけ」で、時計の
  問い合わせが 25 秒途切れると止まる (Foreground Service と通知は維持)。
  時計が未接続でも、画面を見ていなければ GPS は動かない。
  設計・使い方・大会当日の手順は `docs/walk-event.md`、
  BLE 仕様は `docs/protocol-v1.md` の「歩く大会 (walk)」節。
  - 時計: `apps/walk/` (表示専用アプリ)。applist に「ウォーク」行 (サブ行に進捗%)。
    `walk.state` は core の dispatch でパースし、`ble_glue` が Feature へ渡す。
  - スマホ: `android/walk` (GPX・投影・ETA の純 Kotlin) + WalkRepository /
    WalkTracker / WalkService / WalkScreen (デバイスタブの「ウォーク」カードから開く)。
  - 検証済み (PC): core 255 テスト、sim 安全域監査 0 件、`:walk:test` など
    Android 141 テスト + lint 0 errors、firmware build (app 空き 30%)。
    実機 (OPPO A79 5G + 実機時計) で GPX/KML 取込・walk.query/walk.state 往復・
    省電力測位 (25 秒で GPS 停止 / 時計を開くと再開) を確認。
    周回/折返しルートの進捗 (始点=終点でも 0% から増える) は JVM テストで再現・修正済み。
  - **実機未確認**: 時計⇄スマホの walk.query / walk.state 往復、0xE50A (hiking)
    アイコンの描画、10 秒ポーリングの電池影響、Android 14 の FGS(location) 起動・
    再生成と GPS 10s/10m の実挙動、SAF で他アプリの GPX を読む権限。
    背景での START_STICKY 再生成時に Android の FGS 開始制限で測位が再開しない
    可能性 (アプリを開けば復帰) も実機で確認。
- 既存バグ修正 (2026-10-29): `shell.cpp` の `kRouteScreen[]` が Route 0..18 の
  19 要素しかなく、自動採番アプリ (Route::App0 = 32) の画面で範囲外参照していた。
  40 要素 (App0..App7 = -1) に拡張し、`route_name()` がアプリ id (例 "walk") を返す。
- 2026-10-28: テーマの大きな PNG アセットが実機で読めない不具合を修正。
- 対象個体: Waveshare ESP32-S3-Touch-AMOLED-2.06 (32MB flash / PSRAM 8MB)。スピーカーは無効、
  マイクは録音できる (T18 解決: I2S RX/DMA を `audio::init` で予約し、
  `esp_codec_dev_read` の戻り値解釈を修正。実機で 5.5s/88K サンプル確認。
  2026-10-22 にメモ画面からの録音・保存もユーザーが確認)。
  音声メモのスマホ転送はエラーで、音声メモをほぼ使わないため先送り (T30)。
- 設定画面のテーマ節に assets の zip テーマが出る (T28)。sim では行タップで
  `settings.theme` が変わりテーマが適用されることを確認。実機は書き込み・起動・
  設定画面生成 (bench 90ms) まで確認済みで、タップと再起動持続だけ未確認。
- 歩数計: QMI8658C の HW ペドメータを修正 (CTRL9 の ACK 漏れと INT1
  ハンドシェイク、パラメータの毎ブート再投入、STATUS1.bit4=Pedometer を
  起床と誤判定していた件)。2026-10-23 に実機で歩数が増えることをユーザーが確認。
- 音声メモの文字起こし (T31): 2026-10-24 実機で成功 (音声取得 → 電話の STT →
  `memo.text.set` → `memo.saved`)。本文は音声メモの text (NVS、最大 256B) に保存。
  電話は音声を cache するだけ (本文の正は時計)。
- 同梱テーマ 4 つ (mame/cosmos/cute/cyber) と日本語フォント 3 つ (jp_20/26/32 の
  binfont、計 ~1.4MB) は assets パーティションに USB で書いてある (AGENTS.md)。
  フォントは起動時に `ui::init_display` で PSRAM へ読み、失敗時は内蔵の最小
  フォント (ASCII+かな+記号) にフォールバックする (T24)。
  BLE でのテーマ転送は不安定で先送り (T25)。
- テーマ文字盤の v6 拡張 (2026-10-25): `face_layout` に `time_h`/`time_m`
  (時刻の時・分を別要素で置く)、`shadow`/`shadow_dx`/`shadow_dy` (文字の影)、
  `battery_gauge` (残量バー + 充電色 + 突起) を追加。既存テーマは未指定なら
  従来どおり。仕様は docs/theme-format.md の「v6 キー詳細」。LVGL の
  `text_outline` は vector graphics 無効で使えないため影で実装している。
- v6.1 (2026-10-25): `outline`/`outline_w` (全周の縁取り、同フォントのラベルを
  上下左右にずらして重ねる) と `behind` (chara より背面に描く) を追加。
  `ThemeFaceElem::set` は uint32 (bit16..18)。最初の適用先は `themes/sopia/`
  (Bungee 数字 + 立ち絵の後ろに時刻)。実機書き込み済み (2026-10-25)。
  縁取りは 1 要素あたり最大 5 ラベル増えるので、96px 級に多用するときは `b` で確認。
- v6.2 (2026-10-25): `highlight`/`highlight_dy` (本体を `highlight_dy` 上に
  ずらして背面に置き、ストローク上端だけ明るく見せる上光) を追加。
  数字フォントのサイズ許可値に **112** を追加 (tokens と `face_layout.*.font`)。
  `tools/font_mono.py --yscale` でグリフを縦に伸ばせる (Bungee 112px + 1.14
  → 縦長数字)。電池は `battery` (テキスト) を `battery_gauge` の矩形に重ねて
  % を内側に出す (gauge を先に作るのでテキストが上になる)。
- 同日追加 (v6.3): `font` は 8..200 の任意 px (列挙をやめた)。テキスト要素の
  `w` は枠幅 = その中で中央寄せ (ゲージに重ねる数値用)。右/下基準の要素は
  テキスト確定後に絶対座標で置し直す (`settle()`) — 空ラベルの align で
  画面外にはみ出していた不具合の修正。クランプ後の位置も `LV_ALIGN_TOP_LEFT`
  で確定させないとレイアウト時に align で戻される。
  電池の表示は `%` を外して数値のみ。
- 2026-10-28: テーマの大きな PNG アセットが実機で読めない不具合を修正。
  `theme_res::img_load` が lodepng で RGBA (~820KB) を確保して err=83 に
  なっていた (sim は通るので気づかず、星夜もるの立ち絵が出なかった)。
  アリーナを出力先に `decode_png` (行ストリーミング, 作業 ~50KB) で展開する。
- 2026-10-28: テーマが `face_layout` を持つとき、**テーマ選択で文字盤設定を
  `theme` に切り替える**ようにした (`shell.cpp` の `adopt_theme_face`)。
  これが無いと、テーマを選んでもトップは前の文字盤 (bold) のままで
  「立ち絵が出てこない」ように見える (そぴあ・星夜もるで発生)。
  工場出荷の `face="bold"` のままなら起動時にも自動採用する。
- 同日追加 (v6.4): 文字盤のラベルは `LV_OBJ_FLAG_USER_1` を付けて、sim の
  余白監査を 6px・角欠け判定を免除 (大きな数字を画面端まで使うため)。
  他画面のテキストは従来どおり 32px 余白。
  **注意**: 実機の assets に入っている cute.zip は、リポジトリの `android/.../themes/cute.zip`
  より古い (bg.png が b6dd640 以前のもの)。必要なら assets イメージを作り直して書く。
- PWR 短押しの画面 ON/OFF は 2026-10-25 に実機で確認。押下ごとに
  `board.buttons: PWR down level=1` / `up level=0` / `short level=0` が出て、
  画面OFF 中の押下は `power 2 -> 0`、点灯中の押下は `power 0 -> 2`
  (押下から適用まで ~130ms)。**Dim (輝度20%) 中の押下は消灯ではなく点灯**に倒す
  (`core/src/runtime.cpp`。Dim は暗くて消灯に見えるため)。消灯中の押下は
  割り当てに関係なく `Wake` を積む (`watch_app/src/input.cpp`)。
- 2026-10-19 に描画を作り直した (T29)。standard テーマで全面再描画 85〜112ms → 40〜68ms。
  経緯と数値は `docs/perf.md`。
- 外部コンポーネントへのパッチは `firmware/vendor/` にある (BSP / esp_lvgl_port / sh8601)。
  `managed_components/` は編集しない。

## 実機で確定した値

| 項目 | 値 | 根拠 |
|---|---|---|
| 描画パイプライン | PSRAM の全面描画バッファ 1 枚 + 内蔵 8.2KB バウンス 2 枚 | docs/perf.md |
| LVGL タスク / SPI ISR | 両方 core1 固定 | spi_bus_lock のマルチコア競合 (IDFGH-17166) で UI が凍った |
| 全面再描画 (standard) | 40〜68ms (`b` ベンチ) | docs/perf.md |
| 全面再描画 (cute/cyber) | 80〜157ms (設定は最大 ~160ms)。スキンの大きい部品は近似塗りに置き換え済み | docs/perf.md |
| 内蔵 SRAM | 起動後の空き ~21KB、表示中 ~18-21KB (LVGL ヒープを PSRAM に移して安定化) | `b` の int_free |
| AI dictation リング | PSRAM 16KB (audio::init で確保、8KB フォールバック。録音中はファイルを作らない) | T38 |
| BLE dictation | 接続間隔 15〜30ms / latency 0 / 2M PHY を要求 (停止・切断で 100〜200ms に戻す) | T38 (実機未確認) |
| Android APK | 39.6MiB (arm64-v8a のみ, Vosk native 込み) | `assembleDebug` |
| PSRAM | standard で空き ~3.4MB (アリーナ不要)。肌テーマで ~750KB、 mame で ~450KB 空き (binfont 常駐 ~1.4MB + アリーナ) | `b` の psram_free |
| app 領域 | 4MB 中の空き **~48%** (T43 で内蔵文字盤フォントを 14 グリフにサブセット化、−738KB) | `idf.py build` 末尾 (2026-11-17: 0x215380) |
| VSYS power-off 閾値 | 2600 mV (既定維持) | AXP2101 reg 0x24 |
| 角 R / safe-area | R≈100px / 32〜36px | 実機写真の解析 |
| パネル窓オフセット | x_gap=22, y_gap=0 | 同上 |
| 待機中の再描画 | 0 (無操作で redraw なし) | LVGL sysmon |
| `deep_sleep_after_s` | BLE `settings.set` で即時反映 | core/protocol/dispatch.cpp |
| touch の I2C NACK | 解消。FT3168 スリープ中のセーフティポーリングを INT (GPIO38) ゲートで止めた | vendor/esp_lvgl_port |
| 起床ゲート (腕上げ) | `静止3連続 && deg>=30 && dir.z<=-0.65 && dir.x>=-0.35` (左手首で計測)。WoM は 224mg + blanking 12 (~570ms) | roadmap T27 |

## 未解決・注意

- BLE のテーマ転送 (T25)。
- 音声メモをスマホへ送るとエラーになる (T30)。原因は BULK 送信の mbuf 枯渇で、
  2026-10-24 に修正 (チャンクを MTU 収まりの 232B に)。実機で 30KB/13s の
  転送を確認。アプリは音声をキャッシュし、2 回目以降は再転送しない。
- 設定画面のファイルテーマ選択 (T28) は実装済み。実機はタップと再起動持続が未確認。
- 歩数計 (HW ペドメータ) の修正は実機で歩数増加を確認済み (2026-10-23)。
- 重いテーマ (cute/cyber) で固まることがある (ユーザー報告)。2026-10-24 に
  真因を特定して修正: テーマ切替時にアクティブ画面を直接削除していた点と、
  戻る2連打で遷移アニメが残ったまま旧画面を削除していた点。一時画面を挟む
  方式と、削除前の `lv_anim_delete` で対処。実機の手動確認待ち。
- Home と文字盤の整理はユーザーの判断待ち (T22、`docs/home-face-proposal.md`)。
- 起動時の `E sh8601: swap_xy is not supported by this panel` は vendor の
  esp_lvgl_port をパッチして消した (回転なしボードでは呼ぶ意味がない)。
- 肌テーマ (cute/cyber) ではアプリ一覧の bake (~850KB) がほぼ確実に失敗する
  (T24 の binfont 常駐 ~1.4MB + アリーナで、最大連続空きは ~500KB)。
  見た目は同じだがスクロールが ~180ms と遅い。bake 確保失敗時はスキン
  キャッシュを捨てて一度だけ再試行する。テーマアリーナは「資産領域」と
  「画面ブロック 640KiB×2」に分割して別個確保する — フォント常駐後は
  単一 ~3MB の連続確保が失敗して mame の資産が欠けていたため。
- スキン (9-slice) の大きい部品は 1D 走査で近似塗りに切り替えた (fill/band、詳細は
  docs/perf.md)。cute は sim 差分 ~0、cyber は縞の補間差のみ (max<=17) で性能が
  およそ 1.3〜1.8 倍になった。縁ストリップは LVGL の伸縮補間が見た目の一部なので
  近似していない (やると cute で max=146 の誤差が出る)。

## 実機作業の注意

- アプリの書き込みは `0x20000` (ota_0)。ほかのオフセットはパーティションを壊す。手順は firmware/README.md。
- ポートは `/dev/ttyACM0` ↔ `/dev/ttyACM1` で変わる。毎回 `ls /dev/ttyACM*`。
- `idf.py flash` は cmake を再実行する。`IDF_CCACHE_ENABLE=1` 無しだと全リビルド。
- ログは `python -m esp_idf_monitor --port /dev/ttyACM0 build/sas_watch.elf` を `timeout` 付きで
  ファイルに落とすのが安定。自動操作は pyserial で 1 文字コマンド (`b` `t` `v` など) を送る。
  素の `cat` は制御線の扱いで受信できないことがある — 1 セッションで open→送信→受信まで行う
  (DTR/RTS を自分でいじるとチップが download モードでリセットループするので触らない)。
- 画面 OFF 中の入力 (シリアルの `t` もタッチ扱い) は「起こすだけ」で処理されない。
  物理ボタンも同じで、消灯中の押下は割り当てに関係なく `Wake` だけを積む
  (`watch_app/src/input.cpp`)。Dim 中の PWR 短押しは消灯ではなく点灯。
- ボタンは押すたびに `board.buttons: PWR down level=1` / `up level=0` /
  `short level=0` を出す。PWR が効かないときは、まずこの 3 行が出るかで
  「ボタンが読めていない」か「Action が届いていない (`watch_app.input` /
  `watch_app.power: power N -> M`)」を切り分ける。
- Android 13 以降のサイドロードでは「制限された設定」を許可しないと通知アクセスを ON にできない
  (設定 → アプリ → SAS-Watch → ⋮ → 制限された設定を許可)。

## 検証済み環境

ESP-IDF 5.5.1 / LVGL 9.5.0 / esp_lvgl_port 2.9.0 (vendor) / NimBLE / C++17 (例外・RTTI なし)。
