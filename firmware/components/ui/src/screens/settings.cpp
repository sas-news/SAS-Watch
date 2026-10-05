// settings.cpp — 設定: 画面 / 音と振動 / テーマ / ボタン割り当て。
// セクションごとにキャプション + 区切り線つきグループ。
// 「文字盤」「数字フォント」は行タップで選択ビューに切替える (掘り下げ)。
#include <cstdio>
#include <cstring>

#include "../components.hpp"
#include "../faces/faces.hpp"
#include "../theme.hpp"
#include "screens.hpp"
#include "ui/port.hpp"
#include "ui/ui.hpp"

// フォント選択ビューの「12:34」プレビュー用 (生成済みの最小サイズのみ使う)。
LV_FONT_DECLARE(font_fc_oswald_34);
LV_FONT_DECLARE(font_fc_bebas_34);
LV_FONT_DECLARE(font_fc_orbitron_34);
LV_FONT_DECLARE(font_fc_outfit_34);
LV_FONT_DECLARE(font_fc_chakra_34);

namespace {

lv_obj_t* s_col = nullptr;    // メイン列
lv_obj_t* s_pick = nullptr;   // 選択ビュー列 (文字盤/フォント)
lv_obj_t* s_pick_grp = nullptr;
lv_obj_t* s_bri = nullptr;
lv_obj_t* s_off = nullptr;
lv_obj_t* s_vol = nullptr;
lv_obj_t* s_raise_sw = nullptr;
lv_obj_t* s_click_sw = nullptr;
lv_obj_t* s_vib_sw = nullptr;
bool s_updating = false;

void show_main() {
  lv_obj_remove_flag(s_col, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_pick, LV_OBJ_FLAG_HIDDEN);
  lv_obj_scroll_to_y(s_col, 0, LV_ANIM_OFF);
}

void show_pick() {
  lv_obj_add_flag(s_col, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(s_pick, LV_OBJ_FLAG_HIDDEN);
  lv_obj_scroll_to_y(s_pick, 0, LV_ANIM_OFF);
}

// 選択ビューの行: タイトル + 選択中なら sub「使用中」、preview_font があれば
// 右に「12:34」のプレビュー。
lv_obj_t* pick_row(lv_obj_t* grp, const char* label, bool in_use,
                   const lv_font_t* preview, lv_event_cb_t cb, void* ud) {
  const ui::Theme& t = ui::theme();
  lv_obj_t* r = ui::c::row_box(grp, cb, ud);
  // プレビューある時はその分タイトル幅を絞る。
  ui::c::row_text(r, label, in_use ? "使用中" : nullptr,
                  preview ? 244 : 334);
  if (preview) {
    lv_obj_t* p = lv_label_create(r);
    lv_obj_add_flag(p, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_label_set_text(p, "12:34");
    lv_obj_set_style_text_font(p, preview, 0);
    lv_obj_set_style_text_color(p, t.text_dim, 0);
  }
  return r;
}

// 選択ビュー共通の先頭行 (‹ 戻るでメインに戻る)。
void pick_back_row(lv_obj_t* grp) {
  ui::c::back_row(grp, [](lv_event_t*) { show_main(); });
}

const char* font_ja(const char* id) {
  struct { const char* en; const char* ja; } static const kNames[] = {
      {"auto", "自動"},     {"oswald", "Oswald"},
      {"bebas", "Bebas"},   {"orbitron", "Orbitron"},
      {"outfit", "Outfit"}, {"chakra", "Chakra"},
  };
  for (const auto& m : kNames)
    if (std::strcmp(m.en, id) == 0) return m.ja;
  return id;
}

void open_face_picker() {
  const watch::Settings& st = *ui::ctx().settings;
  lv_obj_clean(s_pick_grp);
  pick_back_row(s_pick_grp);
  for (const ui::face::Ops* const* p = ui::face::all(); *p; ++p) {
    const bool inuse = std::strcmp(st.face, (*p)->id) == 0;
    pick_row(s_pick_grp, ui::face::label_ja((*p)->id), inuse, nullptr,
             [](lv_event_t* e) {
               const char* id = static_cast<const char*>(
                   lv_event_get_user_data(e));
               ui::emit_text(watch::ActionType::SetFace, id);
               // FaceChanged → shell が画面を組み直すのでそのままメインに戻る
             },
             const_cast<char*>((*p)->id));
  }
  show_pick();
}

void open_font_picker() {
  const watch::Settings& st = *ui::ctx().settings;
  static const struct {
    const char* id;
    const char* ja;
    const lv_font_t* preview;
  } kFonts[] = {
      {"auto", "自動 (文字盤に合わせる)", nullptr},
      {"oswald", "Oswald", &font_fc_oswald_34},
      {"bebas", "Bebas Neue", &font_fc_bebas_34},
      {"orbitron", "Orbitron", &font_fc_orbitron_34},
      {"outfit", "Outfit", &font_fc_outfit_34},
      {"chakra", "Chakra Petch", &font_fc_chakra_34},
  };
  lv_obj_clean(s_pick_grp);
  pick_back_row(s_pick_grp);
  for (const auto& f : kFonts) {
    const bool inuse = std::strcmp(st.clock_font, f.id) == 0;
    pick_row(s_pick_grp, f.ja, inuse, f.preview,
             [](lv_event_t* e) {
               const char* id = static_cast<const char*>(
                   lv_event_get_user_data(e));
               ui::emit_text(watch::ActionType::SetClockFont, id);
             },
             const_cast<char*>(f.id));
  }
  show_pick();
}

const char* action_jp(const char* name) {
  // core の Action 名 → 日本語表示
  struct { const char* en; const char* jp; } static const kMap[] = {
      {"none", "なし"},       {"back", "戻る"},
      {"home", "ホーム"},     {"primary", "決定"},
      {"screen_off", "画面OFF"}, {"wake", "復帰"},
      {"power_menu", "電源メニュー"}, {"nav.quick", "クイック設定"},
      {"nav.more", "アプリ一覧"},  {"nav.dev", "開発者画面"},
      {"memo.record", "メモ録音"},  {"timer.start", "タイマー開始"},
  };
  for (const auto& m : kMap)
    if (std::strcmp(m.en, name) == 0) return m.jp;
  return name;
}

lv_obj_t* build(lv_obj_t* scr) {
  const ui::Theme& t = ui::theme();
  lv_obj_set_style_bg_color(scr, t.bg, 0);
  ui::c::header(scr, "設定", true);
  s_col = ui::c::content(scr);
  const watch::Settings& st = *ui::ctx().settings;

  // ---- 画面 ----
  ui::c::caption(s_col, "画面");
  lv_obj_t* g_scr = ui::c::group(s_col);
  s_bri = ui::c::slider_row(
      g_scr, "明るさ", "%", 5, 100, static_cast<int32_t>(st.brightness),
      [](lv_event_t* e) {
        if (s_updating) return;
        lv_obj_t* s = lv_event_get_target_obj(e);
        ui::emit(watch::ActionType::SetBrightness,
                 static_cast<uint32_t>(lv_slider_get_value(s)));
      },
      nullptr);
  s_off = ui::c::slider_row(
      g_scr, "画面OFFまで", "秒", 5, 120,
      static_cast<int32_t>(st.screen_off_after_s),
      [](lv_event_t* e) {
        if (s_updating) return;
        lv_obj_t* s = lv_event_get_target_obj(e);
        ui::emit(watch::ActionType::SetScreenOffAfter,
                 static_cast<uint32_t>(lv_slider_get_value(s)));
      },
      nullptr);
  ui::c::row(g_scr, "文字盤", nullptr, ui::face::label_ja(st.face), true,
             [](lv_event_t*) { open_face_picker(); }, nullptr);
  ui::c::row(g_scr, "数字フォント", nullptr, font_ja(st.clock_font), true,
             [](lv_event_t*) { open_font_picker(); }, nullptr);
  s_raise_sw = ui::c::switch_of(ui::c::row_switch(
      g_scr, "腕を上げて画面オン", nullptr, st.raise_to_wake,
      [](lv_event_t*) {
        const watch::Settings& cur = *ui::ctx().settings;
        ui::emit(watch::ActionType::SetRaiseToWake,
                 cur.raise_to_wake ? 0u : 1u);
      },
      nullptr));

  // ---- 音と振動 ----
  ui::c::caption(s_col, "音と振動");
  lv_obj_t* g_snd = ui::c::group(s_col);
  s_vol = ui::c::slider_row(
      g_snd, "音量", "%", 0, 100, static_cast<int32_t>(st.audio_volume),
      [](lv_event_t* e) {
        if (s_updating) return;
        lv_obj_t* s = lv_event_get_target_obj(e);
        ui::emit(watch::ActionType::SetAudioVolume,
                 static_cast<uint32_t>(lv_slider_get_value(s)));
      },
      nullptr);
  s_click_sw = ui::c::switch_of(ui::c::row_switch(
      g_snd, "クリック音", nullptr, st.audio_click,
      [](lv_event_t*) {
        const watch::Settings& cur = *ui::ctx().settings;
        ui::emit(watch::ActionType::SetAudioClick,
                 cur.audio_click ? 0u : 1u);
      },
      nullptr));
  s_vib_sw = ui::c::switch_of(ui::c::row_switch(
      g_snd, "通知で振動", nullptr, st.notify_vibrate,
      [](lv_event_t*) {
        const watch::Settings& cur = *ui::ctx().settings;
        ui::emit(watch::ActionType::SetNotifyVibrate,
                 cur.notify_vibrate ? 0u : 1u);
      },
      nullptr));

  // ---- テーマ ----
  // 内蔵 2 種は時計から切替可。BLE で入れた file テーマは
  // 適用中だけ表示する (選び直すにはスマホから送る)。
  ui::c::caption(s_col, "テーマ");
  lv_obj_t* g_theme = ui::c::group(s_col);
  const char* cur = ui::theme_id();
  const bool is_standard = std::strcmp(cur, "standard") == 0;
  const bool is_light = std::strcmp(cur, "light") == 0;
  ui::c::row(g_theme, "標準 (ダーク)", is_standard ? "使用中" : nullptr,
             nullptr, false,
             [](lv_event_t*) {
               ui::emit_text(watch::ActionType::SetTheme, "standard");
             },
             nullptr);
  ui::c::row(g_theme, "明るい", is_light ? "使用中" : nullptr, nullptr,
             false,
             [](lv_event_t*) {
               ui::emit_text(watch::ActionType::SetTheme, "light");
             },
             nullptr);
  if (!is_standard && !is_light) {
    ui::c::row(g_theme, ui::theme_name(), "使用中", nullptr, false, nullptr,
               nullptr);
  }

  // ---- ボタン割り当て (参照のみ) ----
  ui::c::caption(s_col, "ボタン割り当て");
  lv_obj_t* g_btn = ui::c::group(s_col);
  ui::c::row(g_btn, "BOOT 短押し", nullptr,
             action_jp(st.button_boot_short), false, nullptr, nullptr);
  ui::c::row(g_btn, "BOOT 長押し", nullptr,
             action_jp(st.button_boot_long), false, nullptr, nullptr);
  ui::c::row(g_btn, "BOOT 2連打", nullptr,
             action_jp(st.button_boot_double), false, nullptr, nullptr);
  ui::c::row(g_btn, "PWR 短押し", nullptr,
             action_jp(st.button_pwr_short), false, nullptr, nullptr);
  ui::c::row(g_btn, "PWR 長押し", nullptr,
             action_jp(st.button_pwr_long), false, nullptr, nullptr);

  // ---- その他 ----
  ui::c::caption(s_col, "その他");
  lv_obj_t* g_etc = ui::c::group(s_col);
  char fwver[48];
  std::snprintf(fwver, sizeof(fwver), "現在 v%s", ui::port::fw_version());
  ui::c::row(g_etc, "ファーム更新", nullptr, fwver, true,
             [](lv_event_t*) {
               ui::emit(watch::ActionType::Navigate,
                        static_cast<uint32_t>(watch::Route::Ota));
             },
             nullptr);
  lv_obj_t* info = ui::c::card(s_col);
  char buf[96];
  ui::port::device_info(buf, sizeof(buf));
  ui::c::line(info, buf);

  // ---- 選択ビュー列 (メイン列と同じ位置。普段は隠す) ----
  s_pick = ui::c::content(scr);
  s_pick_grp = ui::c::group(s_pick);
  lv_obj_add_flag(s_pick, LV_OBJ_FLAG_HIDDEN);

  show_main();
  return scr;
}

void on_event(lv_obj_t*, const watch::Event& e) {
  if (e.type != watch::EventType::BrightnessChanged &&
      e.type != watch::EventType::SettingsChanged) {
    return;
  }
  const watch::Settings& st = *ui::ctx().settings;
  s_updating = true;
  if (s_bri) ui::c::slider_set(s_bri, static_cast<int32_t>(st.brightness));
  if (s_off)
    ui::c::slider_set(s_off, static_cast<int32_t>(st.screen_off_after_s));
  if (s_vol) ui::c::slider_set(s_vol, static_cast<int32_t>(st.audio_volume));
  if (s_click_sw)
    lv_obj_set_state(s_click_sw, LV_STATE_CHECKED, st.audio_click);
  if (s_vib_sw)
    lv_obj_set_state(s_vib_sw, LV_STATE_CHECKED, st.notify_vibrate);
  if (s_raise_sw)
    lv_obj_set_state(s_raise_sw, LV_STATE_CHECKED, st.raise_to_wake);
  s_updating = false;
}

}  // namespace

namespace ui {
extern const ScreenOps kSettingsScreen = {watch::Route::Settings, build, on_event};
}
