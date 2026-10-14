# アプリ開発ガイド (T19)

アプリは `apps/<id>/` (または外部リポジトリのディレクトリ) に置くだけで、
本体コード (core の Feature 一覧、画面レジストリ、アプリ一覧) を
**編集せず**に組み込める。CMake が `app.cmake` を走査して登録表を
自動生成する。

## 最小のアプリ (雛形)

`apps/_template/` をコピーして `apps/<myapp>/` にリネームするだけで
applist に出て動く。ファイル構成:

```
apps/<id>/
  app.cmake              # 定義 (id・名前・アイコン・有効/無効)
  feature/<id>.hpp       # Feature の宣言 (名前空間 watch::features)
  feature/<id>.cpp       # FeatureDescriptor 実体 + Action 処理
  screen.cpp             # 画面 (LVGL)。ui::kXxxScreen を export
```

`feature/*.hpp` は core/include には置かない — アプリ dir 内の
`feature/*.hpp` を APP_FEATURE_HDR で指す (他の .cpp からは
`#include "feature/<id>.hpp"` で届く)。

## app.cmake の項目

```cmake
set(APP_ID myapp)                    # [a-z0-9-]+。省略時は dir 名
set(APP_NAME "マイアプリ")            # applist の表示名
set(APP_ENABLED 1)                    # 0 でビルド対象外
set(APP_FEATURE_SYM "watch::features::kMyApp")
set(APP_FEATURE_HDR "feature/myapp.hpp")   # core/include or アプリdir相対
# set(APP_ROUTE "watch::Route::Timer")     # 省略で Route::App0 から自動採番
set(APP_SCREEN_SYM "ui::kMyAppScreen")      # 画面の ScreenOps 実体
set(APP_ICON_CP 0xE800)               # font_icon_36 の PUA コードポイント
set(APP_ICON_BG 0x8A8F9C)             # アイコンタイルの色 (RGB)
# set(APP_SUB "myapp_applist_sub")   # applist サブ行関数 (ui::ns、任意)
set(APP_ORDER 50)                     # applist の並び (小さいほど上)
```

## Feature の書き方 (feature/<id>.cpp)

```cpp
#include "myapp.hpp"
namespace watch { namespace features {
namespace {
bool handle(const Action& a, FeatureContext& ctx) {
  if (a.type == ActionType::PrimaryAction) { /* 主操作 */ return true; }
  return false;
}
}
const FeatureDescriptor kMyApp = {
    "myapp", HasScreen, Route::App0,
    nullptr, handle, nullptr, nullptr, nullptr, nullptr, nullptr,
};
}}
```

- `ActionType::PrimaryAction` = BOOT 短押しや画面の主ボタン。
- `ActionType::LongPress` = 画面長押し (何もしない既定)。
- 計測中など画面を消したくない時は `keep_screen_on` を返す
  (画面を離れたら切れる、手動消灯は効く)。
- 状態を永続化するなら `ctx.storage` (KeyValueStore)。キーは **15文字以内**。
- Feature は LVGL を知らない。画面へは `ctx.bus.publish({EventType::Xxx, ..})` で通知。
- ハード非依存に保つと core のホストテストで動かせる。

## 画面の書き方 (screen.cpp)

- `ui::c::` の部品だけで組む (header / content / card / group / row /
  button / switch / slider / line / caption / fit_btn / confirm…)。
- 色とフォントは `ui::theme()` のトークンから。固定色・固定座標は原則禁止
  (410x502・角 R100 の安全域の中だけに置く)。
- `ScreenOps` を `ui::kXxxScreen` として export し、APP_SCREEN_SYM で指す。
- `build(scr)` で組み立て、`on_event` は Feature の Event で部分再描画。

## 外部リポジトリから追加する

リポジトリ外のディレクトリに `<id>/app.cmake` を並べて、ビルド時に
`-DWATCH_EXTRA_APP_DIRS=<dir>` を渡す (複数は `;` 区切り)。
本体のコードは一切編集しない。CI (.github/workflows/sim.yml) で
外部アプリのビルド検証がある。

## 確認

```
cmake -S sim -B sim/build && cmake --build sim/build -j
./sim/build/snapshot --out /tmp/shots
```
→ `03_applist.png` に新しいアプリ行が出る。
