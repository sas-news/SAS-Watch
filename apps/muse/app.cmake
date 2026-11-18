# muse — アプリ定義 (apps/cmake/apps.cmake が走査する)。
# Muse (Phase 2 ⑤ UI 字幕) の返答をページ表示する専用アプリ。
# 接続・送受信は firmware の muse_link (CONFIG_WATCH_ENABLE_MUSE_LINK) が行い、
# 既定 off のファームでは「このファームでは Muse が無効です」を出す。
set(APP_ID muse)
set(APP_NAME "Muse")
set(APP_ENABLED 1)
set(APP_FEATURE_SYM "watch::features::kMuse")
set(APP_FEATURE_HDR "watch/features/muse.hpp")
# walk が APP_ROUTE 省略で動的採番 App0 を使うため、muse は明示的に App1。
# (apps.cmake はディレクトリ名順に採番するので、省略すると muse が App0 を取り
#  walk の Route と衝突する。既存アプリの Route は変えない。)
set(APP_ROUTE "watch::Route::App1")
set(APP_SCREEN_SYM "ui::kMuseScreen")
set(APP_ICON_CP 0xE8B8)   # 既存 font_icon_36 のチャット吹き出し (追加グリフ不要)
set(APP_ICON_BG 0x7B61FF)
set(APP_ORDER 85)         # AI (80) の次。歩数(75) と一覧の下側に並ぶ
