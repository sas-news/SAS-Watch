// home.cpp — Home 画面 = 文字盤 (watch face)。
//   実体は faces/face_*.cpp に分かれており、settings.face で選んだ
//   文字盤の Ops に組み立て・Event 配送を委譲するだけ。
//   文字盤が使う色・フォントは faces 側で ui::theme() から取る。
#include "../faces/faces.hpp"
#include "screens.hpp"
#include "ui/ui.hpp"

namespace {

const ui::face::Ops* s_face = nullptr;

lv_obj_t* build(lv_obj_t* scr) {
  s_face = ui::face::current();
  return s_face->build ? s_face->build(scr) : scr;
}

void on_event(lv_obj_t*, const watch::Event& e) {
  if (s_face && s_face->on_event) s_face->on_event(e);
}

}  // namespace

namespace ui {
extern const ScreenOps kHomeScreen = {watch::Route::Home, build, on_event};
}
