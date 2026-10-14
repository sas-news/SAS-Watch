// media.cpp — 音楽操作: 曲名・アーティスト・再生状態 + 縦並びの操作ボタン。
// ボタン → MediaCommand Action → MediaCmdRequested Event → EVT "media.cmd"。
#include "components.hpp"

LV_FONT_DECLARE(font_icon_36);
#include "theme.hpp"
#include "screens/screens.hpp"
#include "ui/media_art.hpp"
#include "ui/ui.hpp"
#include "watch/features/media.hpp"

#include <cstdio>

namespace {

struct M {
  lv_obj_t* title = nullptr;
  lv_obj_t* artist = nullptr;
  lv_obj_t* state = nullptr;
  lv_obj_t* art = nullptr;    // アルバムアート画像 (無ければ音符アイコン)
  lv_obj_t* note = nullptr;
};
M s;

void refresh() {
  if (!s.title) return;
  const watch::features::MediaState& m = watch::features::media_state();
  // アート: art id が一致する受信画像があれば出す、無ければ音符アイコン。
  const lv_image_dsc_t* dsc = ui::media_art(m.art);
  lv_image_set_src(s.art, dsc);
  if (dsc) {
    lv_obj_add_flag(s.note, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_remove_flag(s.note, LV_OBJ_FLAG_HIDDEN);
  }
  if (!m.valid) {
    lv_label_set_text(s.title, "再生情報がありません");
    lv_label_set_text(s.artist, "スマホで音楽を再生するとここに出ます");
    lv_label_set_text(s.state, "");
    return;
  }
  lv_label_set_text(s.title, m.title[0] ? m.title : "(曲名なし)");
  lv_label_set_text(s.artist, m.artist);
  lv_label_set_text(s.state, m.playing ? "再生中" : "一時停止中");
}

void cmd_btn(lv_obj_t* card, const char* text, watch::MediaCmd cmd) {
  ui::c::button(
      card, text,
      [](lv_event_t* e) {
        const auto c = static_cast<watch::MediaCmd>(
            reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
        ui::emit(watch::ActionType::MediaCommand, static_cast<uint32_t>(c));
      },
      reinterpret_cast<void*>(static_cast<uintptr_t>(cmd)));
}

lv_obj_t* build(lv_obj_t* scr) {
  const ui::Theme& t = ui::theme();
  lv_obj_set_style_bg_color(scr, t.bg, 0);
  ui::c::header(scr, "音楽", true);
  lv_obj_t* col = ui::c::content(scr);
  s = M{};

  lv_obj_t* info = ui::c::card(col);
  // アート枠 120x120 (image 未受信時は音符グリフ)。image は
  // label を兼ねた obj にする (lv_image と label の出し分けが簡単)。
  s.art = lv_image_create(info);
  lv_obj_set_size(s.art, 120, 120);
  lv_obj_center(s.art);
  // 画像が無い時に出す音符はラベルとして別に持つ方が自然だが、
  // lv_image の src が無い状態で中身を出すため text は使わず、
  // 子ラベルにする。
  s.note = lv_label_create(s.art);
  lv_obj_set_style_text_font(s.note, &font_icon_36, 0);
  lv_obj_set_style_text_color(s.note, t.text_dim, 0);
  lv_obj_set_style_text_opa(s.note, LV_OPA_60, 0);
  lv_label_set_text(s.note, "\xee\x90\x85");
  lv_obj_center(s.note);
  lv_obj_set_style_radius(s.art, 12, 0);
  lv_obj_set_style_bg_color(s.art, t.surface2, 0);
  lv_obj_set_style_bg_opa(s.art, LV_OPA_COVER, 0);
  lv_obj_set_style_clip_corner(s.art, true, 0);
  s.state = lv_label_create(info);
  lv_obj_set_style_text_font(s.state, t.font_body, 0);
  lv_obj_set_style_text_color(s.state, t.text_dim, 0);
  s.title = lv_label_create(info);
  lv_obj_set_width(s.title, LV_PCT(100));
  lv_obj_set_style_text_font(s.title, t.font_body, 0);
  lv_obj_set_style_text_color(s.title, t.text, 0);
  lv_label_set_long_mode(s.title, LV_LABEL_LONG_DOT);
  s.artist = lv_label_create(info);
  lv_obj_set_width(s.artist, LV_PCT(100));
  lv_obj_set_style_text_font(s.artist, t.font_body, 0);
  lv_obj_set_style_text_color(s.artist, t.text_dim, 0);
  lv_label_set_long_mode(s.artist, LV_LABEL_LONG_DOT);

  lv_obj_t* ops = ui::c::card(col);
  cmd_btn(ops, "再生 / 一時停止", watch::MediaCmd::PlayPause);
  cmd_btn(ops, "次へ", watch::MediaCmd::Next);
  cmd_btn(ops, "前へ", watch::MediaCmd::Prev);
  cmd_btn(ops, "音量＋", watch::MediaCmd::VolUp);
  cmd_btn(ops, "音量－", watch::MediaCmd::VolDown);

  refresh();
  return scr;
}

void on_event(lv_obj_t*, const watch::Event& e) {
  if (e.type == watch::EventType::MediaStateChanged ||
      e.type == watch::EventType::MediaArtReceived) {
    refresh();
  }
}

}  // namespace

namespace ui {
extern const ScreenOps kMediaScreen = {watch::Route::Media, build, on_event};
}
