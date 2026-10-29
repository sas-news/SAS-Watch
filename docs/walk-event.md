# 歩く大会モード (Walk) — 設計と実装メモ

対象: 「決まった道を歩く大会」で、**進捗 % と到着予想時刻**を時計で見るためのアプリ。
スマホ (Android コンパニオン) がルート・測位・計算の正で、時計は**表示だけ**を持つ。

- 状態: 完了 (2026-10-29、実機確認待ち)。この文書が仕様の正で、protocol-v1.md にも同じ内容を追記済み。
- 関連: `docs/protocol-v1.md` / `docs/app-guide.md` / `docs/plan.md`

## 1. なぜこの分担か (電池)

時計 (ESP32-S3) には GNSS が無い。位置を取るにはスマホの測位を使うしかない。
電池を一番食うのは「測位を動かし続けること」なので、**時計側は測位も常時通信もしない**。

| 端末 | やること | 電池への配慮 |
|---|---|---|
| 時計 | 画面を開いている間だけ、10 秒に 1 回まで `walk.query` を送り、返ってきた状態を描く | GNSS なし。通信は既存の BLE 接続を間欠利用。画面が Dim / OFF の間は**問い合わせない**。画面を作り直したときは即 1 回だけ問い合わせる |
| スマホ | ルート (GPX/KML/GeoJSON) の管理、測位、進捗と到着予想の計算、問い合わせへの応答 | 測位は**時計/アプリ画面を見ている間だけ**。時計の `walk.query` を契機に測位を再開し、25 秒問い合わせが無ければ止める。% と ETA は現在地 1 点と開始時刻で計算できるので、間欠測位でも「見た時点」の値は正しい。Foreground Service は継続 (測位停止中も通知は維持) |

時計の画面を閉じている間は何もしない。開いた瞬間に最新値を取りに行く (ユーザーの
「時計開いたら時計側がデータを要求すれば時計側の電池は持ちそう」をそのまま実装する)。

**スマホ側も同じ考え方**: GPS を動かし続けると 33km/8 時間では電池が厳しいので、
通常は「時計のウォーク画面が見えている間だけ」測位する。時計は画面が見えている間
10 秒ごとに `walk.query` を送るので、その間は連続測位 (10 秒 / 10m) と同じ。
時計の画面が消える (= 問い合わせが止まる) と 25 秒後に GPS を止める。次に時計を
開いた瞬間、止めていた測位を再開して単発の現在地を取り、届いたら `walk.state` を
もう一度送って表示を更新する (`onWatchQuery()` → `requestSingleFix()`)。
アプリのウォーク画面が見えている間は、見た目が止まらないよう連続測位する。
**時計が未接続のときも、誰も画面を見ていないなら GPS は動かさない**
(未接続でもアプリのウォーク画面を見ている間だけ測位する)。

## 2. 全体の流れ

```
[時計] ウォーク画面を開く / 画面が Active なまま 10s 経過 / 「いま更新」
   │  EVT walk.query {}                       (時計 → スマホ)
   ▼
[スマホ] 最新の状態を組み立てる (状態・進捗・残り・ETA・ペース…)
   │  REQ walk.state { ... }                  (スマホ → 時計)
   ▼
[時計] 状態を保存 → Event WalkStateChanged → 画面を部分更新
   │  RES {} (通常の REQ/RES)
   ▼
[スマホ] RES を受けて完了
```

- フレーム形式・msg_id・CRC・フラグメントは protocol-v1.md の既存仕様のまま。
  新しい frame type は足さない。
- `walk.query` は「今の状態をください」の一方向通知 (EVT)。返答は通常の REQ
  `walk.state` で返す。電話は `walk.query` を受けたら**必ず 1 回** `walk.state` を返す。
  加えて、歩行の開始・終了・ルート外れなど表示が大きく変わったときは自発的に
  `walk.state` を送ってよい (時計はどちらも同じに扱う)。
- `walk.state` は時計側で `RES {ok:true, r:{}}` を返す。`ok:false, e:"bad_request"` は
  電話側の不具合なので、電話はログに残して次回リトライでよい。

## 3. プロトコル追加 (protocol-v1.md に追記する内容)

### 3.1 EVT `walk.query` (時計 → スマホ)

| event | data |
|---|---|
| `walk.query` | `{}` |

時計は次のときだけ送る:
1. ウォーク画面を組み立てた直後 (1 回)
2. ウォーク画面が現在の画面で、電源状態が `Active` のまま 10 秒経過するごと
3. 画面の「いま更新」ボタン (最短 1.2 秒間隔で間引く)

Dim / ScreenOff / DeepSleep / 別画面のときは送らない。歩いていないとき (idle) も
画面を開いていれば聞いてよい (開始されたことを検知するため)。

### 3.2 REQ `walk.state` (スマホ → 時計)

params (CBOR map。キーはこの表の順で送る):

| key | type | 必須 | 意味 / 既定 |
|---|---|---|---|
| `state` | text | ○ | `"idle"` \| `"walking"` \| `"finished"` \| `"no_route"` \| `"no_fix"`。未知の値は時計で `"idle"` として扱う |
| `route` | text | | ルート名 (UTF-8 ≤32B)。無ければ空文字 |
| `total_m` | u32 | | ルート全長 m |
| `done_m` | u32 | | 始点からルート上を進んだ距離 m (単調増加) |
| `remain_m` | u32 | | 残り m |
| `permille` | u32 | | 進捗 0..1000 (done/total、四捨五入) |
| `elapsed_s` | u32 | | 歩き始めからの経過秒 |
| `eta_epoch` | i64 | | 到着予想時刻 (epoch 秒)。0 = 不明 |
| `pace_s_per_km` | u32 | | 平均ペース 秒/km。0 = 不明 |
| `started_epoch` | i64 | | 歩き始め時刻 (epoch 秒)。0 = 未開始 |
| `next_cp` | text | | 次のチェックポイント名 (≤24B)。無ければ空 |
| `next_cp_m` | u32 | | 次のチェックポイントまでの残り m |
| `off_route_m` | u32 | | ルートからの逸脱距離 m。時計は **60m 以上** のとき「ルートから外れています」を出す (GPS の数m の揺れで警告しない) |
| `accuracy_m` | u32 | | 直近の測位精度 m |
| `updated_epoch` | i64 | | スマホがこの状態を更新した時刻 (epoch 秒)。省略時 0 |
| `msg` | text | | 補足 (≤48B)。例: 「測位を待っています」 |

result: `{}`

- 省略されたキーは時計で 0 / 空文字として扱う。時計はキー順に依存しない
  (既存の CBOR ルールどおり)。
- 文字列は電話側で UTF-8 バイト長に切り詰める (時計のバッファ: route 33B、next_cp 25B、
  msg 49B)。
- 例:

```
REQ {m:"walk.state", p:{
  state:"walking", route:"○○街道 20km", total_m:20400, done_m:12680,
  remain_m:7720, permille:622, elapsed_s:5400, eta_epoch:1761702900,
  pace_s_per_km:720, started_epoch:1761697500, next_cp:"第3関門",
  next_cp_m:320, off_route_m:0, accuracy_m:8,
  updated_epoch:1761702950, msg:""
}}
```

- `hello` の `caps` に `"walk"` を足す。

## 4. 時計側の実装

### 4.1 アプリ定義 `apps/walk/`

```
apps/walk/
  app.cmake            # id/name/route/screen/icon/order/sub
  feature/walk.cpp     # Feature (状態 + 問い合わせのスケジューリング)
  screen.cpp           # 画面 (ui::c 部品) + applist サブ行
  test_walk.cpp        # core のホストテスト (GoogleTest)
core/include/watch/features/walk.hpp   # 状態構造体と API (既存アプリと同じ流儀)
```

`app.cmake` の要点:

```cmake
set(APP_ID walk)
set(APP_NAME "ウォーク")
set(APP_ENABLED 1)
set(APP_FEATURE_SYM "watch::features::kWalk")
set(APP_FEATURE_HDR "watch/features/walk.hpp")
# APP_ROUTE は省略 → watch::Route::App0 が自動で振られる
set(APP_SCREEN_SYM "ui::kWalkScreen")
set(APP_ICON_CP 0xE50A)   # Material Symbols "hiking"
set(APP_ICON_BG 0x3D8BFF)
set(APP_SUB "walk_applist_sub")
set(APP_ORDER 5)          # 一覧の先頭近く (大会中に開きやすい)
```

`FeatureDescriptor` は `route = watch::Route::App0`。`handle` は
`ActionType::WalkRefresh` (新設) を受けて即時問い合わせ、`tick` /
`next_deadline_ms` で 10 秒間隔のポーリングを行う。`save/restore` は持たない
(状態はスマホが正。時計は再起動したら次の問い合わせで取り直す)。

### 4.2 core に足すもの

- `action.hpp`: `ActionType::WalkRefresh`
- `event.hpp`: `EventType::WalkQueryRequested`, `EventType::WalkStateChanged`
  (`arg0` = WalkStatus)
- `protocol/dispatch.hpp` の `Services` に
  `bool (*walk_state)(const cbor::Reader& map, void* ctx)` … ではなく、
  実装しやすい形で `bool (*walk_state)(watch::features::WalkState...` を直接
  渡さない (core は feature を知らない)。**文字列と数値を受け取って
  時計側の実装が組み立てる** コールバックにする:
  `bool (*walk_state)(const char* state, ... )` は引数が多すぎるため、
  パース済みの `WalkState` を core 側で組み立てて渡すのは可 (WalkState は
  core のヘッダにある)。`Services::walk_state(const WalkState&, void* ctx)` とし、
  `dispatch.cpp` が CBOR を読んで `WalkState` を埋め、`firmware` の
  `ble_glue.cpp` が `features::walk_set_state(const WalkState&, FeatureContext&)` に渡す。
- `dispatch.cpp` の method 表に `walk.state` を追加 (型・必須キーの検証。
  未知キーは無視、省略キーは既定値、`state` が文字列でなければ `bad_request`)。
- `hello` の `caps` に `"walk"`。
- `firmware/components/watch_app/src/ble_glue.cpp`:
  - `on_bus_evt`: `WalkQueryRequested` → `encode_evt(&w, "walk.query", nullptr, nullptr)`
  - `svc.walk_state` の実装 → `watch::features::walk_set_state(...)`
- `sim/snapshot.cpp`: ウォーク画面のスクショを 1 枚追加
  (歩行中・残り・ETA が入ったサンプル状態)。設計変更なので前後スクショを並べる。

### 4.3 状態構造体 (`core/include/watch/features/walk.hpp`)

```cpp
enum class WalkStatus : uint8_t { Unknown=0, Idle, Walking, Finished, NoRoute, NoFix };

struct WalkState {
  WalkStatus status = WalkStatus::Unknown;
  char route[33] = {};       // UTF-8
  char next_cp[25] = {};
  char msg[49] = {};
  uint32_t total_m = 0, done_m = 0, remain_m = 0, permille = 0;
  uint32_t elapsed_s = 0, pace_s_per_km = 0, next_cp_m = 0;
  uint32_t off_route_m = 0, accuracy_m = 0;
  int64_t eta_epoch = 0, started_epoch = 0, updated_epoch = 0;
};

extern const FeatureDescriptor kWalk;
WalkState walk_state();                    // スナップショット (UI 用)
void walk_set_state(const WalkState& s, FeatureContext& c);  // BLE タスクから。WalkStateChanged を出す
void walk_request_refresh(FeatureContext& c);  // 即時問い合わせ (最短 1.2s 間引き)
void walk_reset_state();
```

### 4.4 画面 (`apps/walk/screen.cpp`)

410x502 / 角 R100 / kPad=32 の安全域。`ui::c` 部品のみ。固定色・固定座標禁止。

1. `header("ウォーク", back=true)`
2. 進捗カード (`card`): 左に `font_num_72` で `62%` を大きく、右に進捗バー
   (steps 画面と同じ track + fill) と `12.6 / 20.4 km` (done / total)
3. 情報カード (`card` + 2 列の行):
   - 到着予想 `14:35` (eta 不明なら `—`) / 残り `7.7 km`
   - ペース `12:00 /km` / 経過 `1:30:00`
   - 開始 `13:02` / 精度 `8 m`
   - ルート名 ・ 次のチェックポイント `第3関門 320 m` (あれば、全幅 1 行)
4. 状態行 (画面下の小さい `line`):
   - walking: `更新 14:12:03 ・ 精度 8m`
   - off_route_m >= 60: `ルートから外れています (85m)` (danger 色)
   - idle: `スマホで「開始」を押してください`
   - no_route: `スマホでルートを取り込んでください`
   - no_fix: `測位を待っています`
   - finished: `到着しました。おつかれさまでした`
   - 最終更新が 60 秒以上前: `(古い情報) 更新 14:05:12`
5. `button_primary("いま更新")` → `ui::emit(ActionType::WalkRefresh)`

- `build()` の最後でも `ui::emit(ActionType::WalkRefresh)` を 1 回呼ぶ。
- `on_event`: `WalkStateChanged` / `SettingsChanged` で `refresh()`。
- `walk_applist_sub()`: `"62% ・ 到着 14:35"` / `"未開始"` / `"ルート未設定"`。
- 文字列は日本語 (AGENTS.md)。LVGL を知るのは screen.cpp だけ。Feature は知らない。
- 画面 OFF を抑止しない (`keep_screen_on` は使わない)。大会中は必要なときだけ点灯。

### 4.5 ポーリング (Feature)

```
tick(now, ctx):
  visible = ctx.nav && ctx.nav->current()==kWalk.route
            && !ctx.nav->has_modal()
            && ctx.power && ctx.power->state()==PowerState::Active
  polling_ = visible
  if (visible && now >= next_poll_ms_) { publish WalkQueryRequested; next_poll_ms_ = now+10000; }
next_deadline_ms(): polling_ ? next_poll_ms_ : 0
walk_request_refresh(): now < last+1200ms なら捨てる / それ以外は publish + next_poll_ms_ を進める
```

- Feature は `PowerPolicy::state()` を見てよい (`FeatureContext.power`)。
- Event の publish は app タスク。`walk_set_state` は BLE タスクから呼ばれる
  (ble_glue が core_lock 内で呼ぶ既存パターン)。

## 5. スマホ側の実装

### 5.1 新モジュール `android/walk` (Kotlin JVM、Android 非依存)

GPX と進捗計算は UI から切り離し、**JVM のユニットテストで検証する**。

```
android/walk/src/main/kotlin/dev/sasnews/amoledwatch/walk/
  GeoPoint.kt          # lat/lon
  Route.kt             # 名前・点列・累積距離・CP
  GpxParser.kt         # GPX (trkpt/rtept/wpt) → Route
  RouteProgress.kt     # 現在地 → done_m / off_route_m / permille / 次の CP
  EtaEstimator.kt      # 経過・距離 → ETA / ペース
  WalkSnapshot.kt      # 電話→時計に渡す状態 (walk.state の材料)
android/walk/src/test/kotlin/...  # JUnit4
```

- 距離は haversine、ルートへの投影は局所平面近似 (緯度経度を m に直して線分へ)。
  歩行規模 (数十 km) では十分。
- `done_m` は**単調増加** (最大値を保持)。GPS の揺れで % が下がらないようにする。
- `off_route_m` = 最寄り線分までの距離。60m 以上で画面に警告 (時計・スマホで同じ閾値)。
- ETA: `done_m >= 50 かつ elapsed_s >= 60` のときだけ算出。
  `pace = elapsed/done`、`eta = now + remain*pace`。それ未満は 0 (不明)。
- ルート読込の上限: 2MB / 20,000 点。それ以上はエラー。
- ルートファイルは **GPX / KML / GeoJSON** を中身で自動判定して読む (拡張子は問わない)。
  - GPX: `<trkpt>` → `<rtept>` の順。名前は `<trk><name>` → `<rte><name>` → ファイル名。
    `<wpt>` をチェックポイントに (名前が無ければ除外)。
  - KML: `<LineString><coordinates>` / `<gx:Track><gx:coord>`。名前付き `<Point>` を
    チェックポイントに。分割された線は端点が 500m 以内なら 1 本につなぐ。
    ルート名は Placemark の `<name>` → Document の `<name>` → ファイル名。
  - GeoJSON: `LineString` / `MultiLineString` / `Polygon` の外周。`Feature` /
    `FeatureCollection` / `GeometryCollection` の入れ子に対応。名前付き `Point` を
    チェックポイントに。座標は `[経度, 緯度]`。
  - 複数の独立した線があるときは、いちばん長い線をルートとして選ぶ (安全側)。
  - KMZ (zip) は非対応。解凍した .kml を選ぶ。
- 取り込み後、画面の「全長」が想定距離 (例: 33km) と一致するか確認する。

### 5.2 アプリ側

- `WalkRepository`: ルート一覧と選択中ルート、歩行セッションを保存。
  保存先は `filesDir/walk/routes.json` + SharedPreferences (選択・セッション)。
  依存追加なし (`org.json` と SharedPreferences のみ)。
- `WalkTracker`: `LocationManager` の GPS_PROVIDER。
  - 連続測位: `requestLocationUpdates(GPS_PROVIDER, 10_000ms, 10m, ...)`。
    アプリのウォーク画面が見えている間と、時計の `walk.query` が来ている間に使う。
  - 単発測位: `getCurrentLocation(GPS_PROVIDER, ...)`。時計の `walk.query` 直後に
    1 回だけ現在地を取り、`walk.state` を更新して送り直す。
  - 時計の問い合わせが 25 秒途切れたら `removeUpdates` で GPS を止める
    (Foreground Service と通知は維持)。
- `WalkService`: `foregroundServiceType="location"` の Foreground Service。
  歩行開始で起動 / 終了で停止。通知に「進捗 % ・残り ・到着予想」を出す。
- `WatchLinkManager`:
  - `Evt.WalkQuery` を受けたら `app.walk.snapshot()` を `Req.WalkState(...)` にして送る
    (`scope.launch`、失敗はログのみ)。
  - 歩行開始・終了時に自発的に 1 回送る (画面が開いていなくても時計の状態を更新)。
- UI: `WalkScreen` (Compose) を `DevicesScreen` 上部の「ウォーク」カードから開く
  (ボトムタブは既に 8 個で増やさない)。内容:
  - 権限が無ければ「位置情報を許可」
  - ルート: `ルートを取り込む` (SAF OpenDocument、GPX / KML / GeoJSON)、
    `サンプルルート`、一覧から選択
  - ルートの簡易スケッチ: Compose Canvas にポリライン + 現在地 + 始点/終点 + CP
    (地図タイルは使わない。依存追加なし)
  - 状態カード: 進捗バー、残り、到着予想、ペース、経過、精度、ルート外れ警告
  - `開始` / `終了` (終了は確認ダイアログ)。終了後も最後の結果を表示
- `AndroidManifest.xml`: `ACCESS_FINE_LOCATION`, `ACCESS_COARSE_LOCATION`,
  `FOREGROUND_SERVICE_LOCATION`, `WalkService` (`foregroundServiceType="location"`)。
  `BLUETOOTH_SCAN` の `neverForLocation` はそのまま (BLE を位置に使わない宣言は維持)。
- `WalkTracker` は「歩行中だけ」測位する。アプリを閉じても Foreground Service が
  継続する。歩行セッションは SharedPreferences に保存し、プロセス再起動後も再開する。
- サンプル GPX を `app/src/main/assets/walk/sample.gpx` に同梱 (2〜3km の周回)。

### 5.3 画面遷移

`MainActivity` は `DevicesScreen` に `onOpenWalk` を渡し、`walkOpen` のとき
`WalkScreen` を全画面表示 (戻るでデバイスへ)。タブは増やさない。

### 5.4 地図ツールからの書き出し方

| ツール | 手順 | 形式 |
|---|---|---|
| Google マイマップ | 地図に「線を描画」でルートを作る → メニュー (⋮) → 「KML をエクスポート」 | KML (KMZ なら解凍) |
| Google Earth | プロジェクト → ルートを「KML として保存」 | KML |
| Strava | ルート (マイ ルート) → 「…」→ 「ルートをエクスポート」 | GPX |
| Komoot | ツアー → 「…」→ 「GPX をエクスポート」 | GPX |
| YAMAP | 活動/計画 → GPX 書き出し (プランによる) | GPX |
| RideWithGPS | ルート → Export → GPX Track | GPX |
| Garmin Connect | コース → エクスポート → GPX | GPX |
| geojson.io など | 保存 (Save) → GeoJSON | GeoJSON |

- Google マップの経路検索 (「ルート」) の結果は直接書き出せない。マイマップで
  線を描き直すか、主催者が配布した GPX/KML を使う。
- スマホでは、書き出したファイルを「ファイル」アプリ等から選べる場所に置く。
  アプリの「ルートを取り込む」→ ファイルピッカーで .gpx / .kml / .geojson を選ぶ。
- 大会当日の朝に試すと通信や権限で詰まりやすい。**前日に取り込んで全長を確認**し、
  ルート一覧に残しておく (アプリを再起動しても残る)。

## 6. 検証

| 対象 | コマンド |
|---|---|
| core | `cmake -S core -B core/build && cmake --build core/build && ctest --test-dir core/build` |
| sim | `cmake -S sim -B sim/build && cmake --build sim/build -j && ./sim/build/snapshot --out sim/screenshots` |
| android | `cd android && ./gradlew :walk:test :protocol:test :agent:test :app:assembleDebug :app:lint :app:testDebugUnitTest` (JAVA_HOME = temurin-17、SDK = `~/android-sdk`) |
| firmware | `cd firmware && . ~/esp/esp-idf/export.sh && idf.py reconfigure && idf.py build` |

- apps/ に新規アプリを追加した後は、既存の `firmware/build` キャッシュでは
  `apps.cmake` が再走しないので `idf.py reconfigure` を先に実行する
  (clean clone / CI は `idf.py build` だけでよい)。
- core/sim の CMake が FetchContent で取る googletest / LVGL は、git の
  URL 書き換え設定 (`insteadOf` で ssh 化) がある環境だと失敗することがある。
  その場合は `GIT_CONFIG_GLOBAL=/dev/null cmake ...` を使う。
- `firmware/dependencies.lock` には vendor component の絶対パスが記録されており、
  リポジトリの置き場所が違うと component manager がエラーにする (既知。CI の
  firmware もこれで落ちている)。その場合はローカルでパスを直してビルドし、
  ロックファイルの差分はコミットしない。

## 7. 実機での確認 (ユーザーに頼む手順)

1. 電話: SAS-Watch アプリ → デバイス → ウォーク。位置情報を許可。
2. `ルートを取り込む` で大会ルート (GPX / KML / GeoJSON) を選ぶ
   (または「サンプルルート」)。取り込み後に「全長」が想定距離と一致するか確認。
3. 「開始」。通知に進捗が出ることを確認。
4. 時計: アプリ一覧 → ウォーク。% / 残り / 到着予想 / ペースが出ること。
   画面を消して再度開くと、開いた時点の値に更新されること。
   周回・折り返しルートでも、開始直後は 0% 付近から増えること (一気に 100% に
   張り付かないこと)。
5. 10 秒ほどで自動更新されること。「いま更新」で即時に更新されること。
6. 終了して「到着しました」表示になること。
7. 電池/省電力測位:
   - 時計は画面 OFF 中に `walk.query` を送っていないこと (ログで確認可)。
   - 電話は時計/アプリ画面を見ている間だけ GPS が動くこと。時計の画面を消して
     25 秒ほど待つと GPS が止まり、時計を開き直すと再開して値が更新されること
     (ログの `watch idle: tracking stopped` / `watch query: tracking start` で確認)。

## 8. 制約・やらないこと

- 地図タイルは使わない (依存追加なし・オフラインで動く)。ルートの線は Canvas。
- ルートは GPX / KML / GeoJSON のファイル取り込みのみ (URL 直読み・KMZ は非対応)。
- 一時停止 (pause) は無し。開始 / 終了のみ。
- 時計からルート選択・開始はできない (電話が正)。時計は表示と更新要求だけ。
- 複数端末・複数ルート同時進行は無し。
- 電話の電池は歩行中 GPS 分だけ使う (10s/10m 間隔)。大会前に充電必須。
