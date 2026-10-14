# _template — 最小アプリの雛形。apps/ 直下の "_" 始まりはスキャン対象外。
# 使い方: このディレクトリを apps/<myapp>/ にコピーするだけで
#         applist に出て動く (Route は App0 から自動採番)。
set(APP_ID template)
set(APP_NAME "サンプル")
set(APP_ENABLED 1)
set(APP_FEATURE_SYM "watch::features::kTemplateApp")
set(APP_FEATURE_HDR "feature/template_app.hpp")
# APP_ROUTE は省略 → watch::Route::App<N> が自動で振られる
set(APP_SCREEN_SYM "ui::kTemplateAppScreen")
set(APP_ICON_CP 0xE800)   # exposure_plus_1 (font_icon_36)
set(APP_ICON_BG 0x8A8F9C)
