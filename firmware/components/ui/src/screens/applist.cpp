// applist.cpp — アプリ一覧 (More)。色タイル + 行をグループに並べる。
//   テーマ "icons" でアプリ id のタイル画像を差し替えられる
//   (docs/theme-format.md)。id は画面名と揃える。
#include "../components.hpp"
#include "../faces/faces.hpp"
#include "../theme.hpp"
#include "app_ui.hpp"   // 生成: apps/*/app.cmake の行 (T19)
#include "screens.hpp"
#include "ui/face_data.hpp"
#include "ui/ui.hpp"

#include <cstdio>

namespace {

void nav_click(lv_event_t* e) {
  const auto r = static_cast<watch::Route>(
      reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
  ui::emit(watch::ActionType::Navigate, static_cast<uint32_t>(r));
}

lv_obj_t* app_row(lv_obj_t* grp, const char* app_id, const char* icon,
                  uint32_t icon_bg, const char* name, const char* sub,
                  watch::Route r) {
  // テーマのアイコン画像があればタイルの代わりに使う (無ければ文字タイル)。
  return ui::c::row_icon_img(
      grp, ui::theme_icon(app_id), icon, lv_color_hex(icon_bg), name, sub,
      true, nav_click,
      reinterpret_cast<void*>(static_cast<uintptr_t>(r)));
}

lv_obj_t* build(lv_obj_t* scr) {
  const ui::Theme& t = ui::theme();
  lv_obj_set_style_bg_color(scr, t.bg, 0);
  ui::c::header(scr, "アプリ", true);
  lv_obj_t* col = ui::c::content(scr);
  lv_obj_t* g = ui::c::group(col);

  lv_obj_t* rows[16];
  watch::Route routes[16];
  size_t rn = 0;
  const auto add = [&](const char* id, const char* icon, uint32_t bg,
                       const char* name, const char* sub, watch::Route r) {
    rows[rn] = app_row(g, id, icon, bg, name, sub, r);
    routes[rn] = r;
    rn++;
  };

  // アプリ行は生成テーブル (apps/*/app.cmake → kSasAppRows、T19)。
  // 追加は app.cmake を置くだけで本体は編集しない。
  for (size_t i = 0; i < ui::kSasAppRowCount && rn < 16; ++i) {
    const ui::SasAppRow& a = ui::kSasAppRows[i];
    const char* sub = a.sub ? a.sub() : nullptr;
    add(a.id, a.icon, a.icon_bg, a.name,
        (sub && sub[0]) ? sub : nullptr, a.route);
  }
  add("settings", "\xEE\xA2\xB8", 0x8A8F9C, "設定", nullptr, watch::Route::Settings);

  // 押しっぱスクロールの軽量化: 静的な行一覧を1枚の画像に焼き、
  // 各行のクリックは透明ゾーンで復元する (再描画が全行再描画から
  // 画像ブリットに変わる)。
  ui::c::BakeZone zones[12];
  lv_obj_update_layout(scr);
  for (size_t i = 0; i < rn; i++) {
    zones[i] = ui::c::bake_rect(col, rows[i], nav_click,
                                reinterpret_cast<void*>(
                                    static_cast<uintptr_t>(routes[i])));
  }
  ui::c::bake_scroll(col, g, zones, rn);
  return scr;
}

}  // namespace

namespace ui {
extern const ScreenOps kMoreScreen = {watch::Route::More, build, nullptr};
}
