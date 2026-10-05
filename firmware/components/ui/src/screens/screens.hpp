// screens.hpp — Screen 層の内部インターフェース。
// create() は画面ルート (lv_obj_create(nullptr) で作った screen) に
// 子を組み立てる。on_event() は core Event で再描画する。
#pragma once

#include "../theme.hpp"
#include "ui/ui.hpp"

namespace ui {

struct ScreenOps {
  watch::Route route;
  lv_obj_t* (*create)(lv_obj_t* scr);
  // Event を受けて部分再描画。nullptr 可。
  void (*on_event)(lv_obj_t* root, const watch::Event& e);
};

// 画面を持つ Route → ScreenOps。持たない Route は nullptr。
const ScreenOps* screen_ops(watch::Route r);

}  // namespace ui
