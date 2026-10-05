// settings.cpp — 設定: 明るさ、画面OFFまでの秒数、テーマ、ボタン割り当て、端末情報。
#include <cstdio>
#include <cstring>

#include "../components.hpp"
#include "../faces/faces.hpp"
#include "../theme.hpp"
#include "screens.hpp"
#include "ui/port.hpp"
#include "ui/ui.hpp"

namespace {

lv_obj_t* s_bri = nullptr;
lv_obj_t* s_off = nullptr;
lv_obj_t* s_vol = nullptr;
lv_obj_t* s_click_l = nullptr;
bool s_updating = false;

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
  lv_obj_t* col = ui::c::content(scr);
  const watch::Settings& st = *ui::ctx().settings;

  s_bri = ui::c::slider_of(ui::c::slider_row(
      col, "明るさ", 5, 100, static_cast<int32_t>(st.brightness),
      [](lv_event_t* e) {
        if (s_updating) return;
        lv_obj_t* s = lv_event_get_target_obj(e);
        ui::emit(watch::ActionType::SetBrightness,
                 static_cast<uint32_t>(lv_slider_get_value(s)));
      },
      nullptr));

  s_off = ui::c::slider_of(ui::c::slider_row(
      col, "画面OFFまで (秒)", 5, 120,
      static_cast<int32_t>(st.screen_off_after_s),
      [](lv_event_t* e) {
        if (s_updating) return;
        lv_obj_t* s = lv_event_get_target_obj(e);
        ui::emit(watch::ActionType::SetScreenOffAfter,
                 static_cast<uint32_t>(lv_slider_get_value(s)));
      },
      nullptr));

  s_vol = ui::c::slider_of(ui::c::slider_row(
      col, "音量", 0, 100, static_cast<int32_t>(st.audio_volume),
      [](lv_event_t* e) {
        if (s_updating) return;
        lv_obj_t* s = lv_event_get_target_obj(e);
        ui::emit(watch::ActionType::SetAudioVolume,
                 static_cast<uint32_t>(lv_slider_get_value(s)));
      },
      nullptr));

  lv_obj_t* snd = ui::c::card(col);
  ui::c::line(snd, "ボタンのクリック音");
  s_click_l = ui::c::list_row(
      snd, "クリック音",
      st.audio_click ? "ON" : "OFF",
      [](lv_event_t*) {
        const watch::Settings& cur = *ui::ctx().settings;
        ui::emit(watch::ActionType::SetAudioClick,
                 cur.audio_click ? 0u : 1u);
      },
      nullptr);

  // テーマ: 内蔵 2 種は時計から切替可。BLE で入れた file テーマは
  // 適用中だけ表示する (選び直すにはスマホから送る)。
  lv_obj_t* tc = ui::c::card(col);
  ui::c::line(tc, "テーマ");
  const char* cur = ui::theme_id();
  const bool is_standard = std::strcmp(cur, "standard") == 0;
  const bool is_light = std::strcmp(cur, "light") == 0;
  ui::c::list_row(tc, "標準 (ダーク)", is_standard ? "使用中" : nullptr,
                  [](lv_event_t*) {
                    ui::emit_text(watch::ActionType::SetTheme, "standard");
                  },
                  nullptr);
  ui::c::list_row(tc, "明るい", is_light ? "使用中" : nullptr,
                  [](lv_event_t*) {
                    ui::emit_text(watch::ActionType::SetTheme, "light");
                  },
                  nullptr);
  if (!is_standard && !is_light) {
    char lbl[80];
    std::snprintf(lbl, sizeof(lbl), "%s", ui::theme_name());
    ui::c::list_row(tc, lbl, "使用中", nullptr, nullptr);
  }

  // 文字盤: 6種を縦に並べる。選択中に「使用中」。
  lv_obj_t* fc = ui::c::card(col);
  ui::c::line(fc, "文字盤");
  for (const ui::face::Ops* const* p = ui::face::all(); *p; ++p) {
    const bool inuse = std::strcmp(st.face, (*p)->id) == 0;
    ui::c::list_row(fc, ui::face::label_ja((*p)->id),
                    inuse ? "使用中" : nullptr,
                    [](lv_event_t* e) {
                      const char* id = static_cast<const char*>(
                          lv_event_get_user_data(e));
                      ui::emit_text(watch::ActionType::SetFace, id);
                    },
                    const_cast<char*>((*p)->id));
  }

  // 時計の数字フォント: auto + 5種。選択中に「使用中」。
  lv_obj_t* cf = ui::c::card(col);
  ui::c::line(cf, "時計の数字フォント");
  {
    static const struct { const char* id; const char* ja; } kFonts[] = {
        {"auto", "自動 (文字盤に合わせる)"},
        {"oswald", "Oswald"},
        {"bebas", "Bebas Neue"},
        {"orbitron", "Orbitron"},
        {"outfit", "Outfit"},
        {"chakra", "Chakra Petch"},
    };
    for (const auto& f : kFonts) {
      const bool inuse = std::strcmp(st.clock_font, f.id) == 0;
      ui::c::list_row(cf, f.ja, inuse ? "使用中" : nullptr,
                      [](lv_event_t* e) {
                        const char* id = static_cast<const char*>(
                            lv_event_get_user_data(e));
                        ui::emit_text(watch::ActionType::SetClockFont, id);
                      },
                      const_cast<char*>(f.id));
    }
  }

  lv_obj_t* btn = ui::c::card(col);
  ui::c::line(btn, "ボタン割り当て");
  ui::c::list_row(btn, "BOOT 短押し", action_jp(st.button_boot_short), nullptr,
                  nullptr);
  ui::c::list_row(btn, "BOOT 長押し", action_jp(st.button_boot_long), nullptr,
                  nullptr);
  ui::c::list_row(btn, "BOOT 2連打", action_jp(st.button_boot_double), nullptr,
                  nullptr);
  ui::c::list_row(btn, "PWR 短押し", action_jp(st.button_pwr_short), nullptr,
                  nullptr);
  ui::c::list_row(btn, "PWR 長押し", action_jp(st.button_pwr_long), nullptr,
                  nullptr);

  // ファーム更新: 現在バージョンを出しつつ進捗画面へ。
  lv_obj_t* fw = ui::c::card(col);
  ui::c::line(fw, "ファーム更新");
  char fwver[48];
  std::snprintf(fwver, sizeof(fwver), "現在 v%s", ui::port::fw_version());
  ui::c::list_row(fw, "ファーム更新", fwver,
                  [](lv_event_t*) {
                    ui::emit(watch::ActionType::Navigate,
                             static_cast<uint32_t>(watch::Route::Ota));
                  },
                  nullptr);

  lv_obj_t* info = ui::c::card(col);
  ui::c::line(info, "端末情報");
  char buf[96];
  ui::port::device_info(buf, sizeof(buf));
  ui::c::line(info, buf);
  return scr;
}

void on_event(lv_obj_t*, const watch::Event& e) {
  if (e.type != watch::EventType::BrightnessChanged &&
      e.type != watch::EventType::SettingsChanged) {
    return;
  }
  const watch::Settings& st = *ui::ctx().settings;
  s_updating = true;
  if (s_bri)
    lv_slider_set_value(s_bri, static_cast<int32_t>(st.brightness),
                        LV_ANIM_OFF);
  if (s_off)
    lv_slider_set_value(s_off, static_cast<int32_t>(st.screen_off_after_s),
                        LV_ANIM_OFF);
  if (s_vol)
    lv_slider_set_value(s_vol, static_cast<int32_t>(st.audio_volume),
                        LV_ANIM_OFF);
  if (s_click_l) {
    // list_row のサブラベルは child 1。
    lv_obj_t* sub = lv_obj_get_child(s_click_l, 1);
    if (sub) lv_label_set_text(sub, st.audio_click ? "ON" : "OFF");
  }
  s_updating = false;
}

}  // namespace

namespace ui {
extern const ScreenOps kSettingsScreen = {watch::Route::Settings, build, on_event};
}
