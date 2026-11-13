# walk — アプリ定義 (apps/cmake/apps.cmake が走査する)。
# 歩く大会モード。表示だけで、測位・進捗計算の正はスマホ (docs/walk-event.md)。
set(APP_ID walk)
set(APP_NAME "ウォーク")
set(APP_ENABLED 1)
set(APP_FEATURE_SYM "watch::features::kWalk")
set(APP_FEATURE_HDR "watch/features/walk.hpp")
# APP_ROUTE は省略 → watch::Route::App0 が自動で振られる (このアプリが最初)。
set(APP_SCREEN_SYM "ui::kWalkScreen")
set(APP_ICON_CP 0xE50A)   # Material Symbols "hiking"
set(APP_ICON_BG 0x3D8BFF)
set(APP_SUB "walk_applist_sub")
set(APP_ORDER 5)          # 大会中に開きやすいよう一覧の先頭近く
