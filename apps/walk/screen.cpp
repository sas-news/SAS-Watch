// walk.cpp — ウォーク画面 (歩く大会)。docs/walk-event.md §4.4。
//   表示だけのアプリ。数値はスマホから walk.state で届く。
//   部品は ui::c と theme トークンのみ (固定色・固定座標を使わない)。
//   画面を組み立てたら WalkRefresh を 1 回出す (開いた瞬間に最新値を要求)。
#include "components.hpp"
#include "screens/screens.hpp"
#include "theme.hpp"
#include "ui/ui.hpp"

#include <cstdio>

#include "watch/features/walk.hpp"
#include "watch/settings.hpp"

LV_FONT_DECLARE(font_num_72);

namespace {

using watch::features::WalkState;
using watch::features::WalkStatus;

// ルート外れ警告の閾値 (m)。スマホ側の警告判定 (>60m) と揃える。
// GPS の測位誤差は 8m 程度あり数 m の逸脱は常に起きるので、0m 超では
// 警告しない (docs/walk-event.md §5.1)。
constexpr uint32_t kOffRouteWarnM = 60;

lv_obj_t* s_num = nullptr;      // 62% (font_num_72)
lv_obj_t* s_fill = nullptr;     // 進捗バー
lv_obj_t* s_dist = nullptr;     // 12.6 / 20.4 km
lv_obj_t* s_eta = nullptr;      // 到着予想
lv_obj_t* s_remain = nullptr;   // 残り
lv_obj_t* s_pace = nullptr;     // ペース
lv_obj_t* s_elapsed = nullptr;  // 経過
lv_obj_t* s_start = nullptr;    // 開始
lv_obj_t* s_route = nullptr;    // ルート名
lv_obj_t* s_next_cp = nullptr;  // 次のチェックポイント (無ければ隠す)
lv_obj_t* s_status = nullptr;   // 状態行 (更新時刻・警告など)

int64_t tz_offset_min() {
  const watch::Settings* s = ui::ctx().settings;
  return s ? s->tz_offset_min : 0;
}

int64_t now_epoch() {
  const watch::FeatureContext* f = ui::ctx().fctx;
  return f ? f->clock.epoch_s() : 0;
}

// epoch 秒 → ローカル HH:MM。0 は不明なので "—"。
void fmt_hhmm(char* buf, size_t cap, int64_t epoch) {
  if (epoch <= 0) {
    std::snprintf(buf, cap, "—");
    return;
  }
  const int64_t local = epoch + tz_offset_min() * 60;
  int64_t sec = ((local % 86400) + 86400) % 86400;
  std::snprintf(buf, cap, "%02d:%02d", static_cast<int>(sec / 3600),
                static_cast<int>((sec % 3600) / 60));
}

// epoch 秒 → ローカル HH:MM:SS (状態行の「更新」用)。
void fmt_hhmmss(char* buf, size_t cap, int64_t epoch) {
  if (epoch <= 0) {
    std::snprintf(buf, cap, "—");
    return;
  }
  const int64_t local = epoch + tz_offset_min() * 60;
  int64_t sec = ((local % 86400) + 86400) % 86400;
  std::snprintf(buf, cap, "%02d:%02d:%02d", static_cast<int>(sec / 3600),
                static_cast<int>((sec % 3600) / 60),
                static_cast<int>(sec % 60));
}

// 秒 → "1:30:00" (経過)。
void fmt_elapsed(char* buf, size_t cap, uint32_t sec) {
  std::snprintf(buf, cap, "%lu:%02lu:%02lu",
                static_cast<unsigned long>(sec / 3600),
                static_cast<unsigned long>((sec % 3600) / 60),
                static_cast<unsigned long>(sec % 60));
}

// 秒/km → "12:00 /km"。
void fmt_pace(char* buf, size_t cap, uint32_t sec_per_km) {
  if (sec_per_km == 0) {
    std::snprintf(buf, cap, "—");
    return;
  }
  std::snprintf(buf, cap, "%lu:%02lu /km",
                static_cast<unsigned long>(sec_per_km / 60),
                static_cast<unsigned long>(sec_per_km % 60));
}

// メートル → "7.7 km" / "320 m"。
void fmt_meters(char* buf, size_t cap, uint32_t m) {
  if (m >= 1000) {
    std::snprintf(buf, cap, "%lu.%lu km", static_cast<unsigned long>(m / 1000),
                  static_cast<unsigned long>((m % 1000) / 100));
  } else {
    std::snprintf(buf, cap, "%lu m", static_cast<unsigned long>(m));
  }
}

// ---- 部品ヘルパ -----------------------------------------------------------

lv_obj_t* mk_label(lv_obj_t* parent, const lv_font_t* font, lv_color_t color,
                   const char* text) {
  lv_obj_t* l = lv_label_create(parent);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, color, 0);
  return l;
}

// 1 行 1 項目 (キー + 値)。値ラベルを返す。
// 半幅セルなので値は固定幅 + 1 行高で末尾省略にする
// (DOTS は高さが可変だと 2 行目が見えてしまう)。
lv_obj_t* kv_cell(lv_obj_t* row, const char* key) {
  const ui::Theme& t = ui::theme();
  lv_obj_t* cell = lv_obj_create(row);
  lv_obj_remove_style_all(cell);
  lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_grow(cell, 1);
  lv_obj_set_height(cell, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(cell, 6, 0);
  mk_label(cell, t.font_body, t.text_dim, key);
  lv_obj_t* v = mk_label(cell, t.font_body, t.text, "—");
  lv_obj_set_width(v, 84);
  lv_obj_set_height(v, lv_font_get_line_height(t.font_body) + 2);
  lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
  return v;
}

// 全幅 1 行 (キー + 値)。ルート名・次 CP・ペースなど長めの値用。
lv_obj_t* kv_line(lv_obj_t* parent, const char* key) {
  const ui::Theme& t = ui::theme();
  lv_obj_t* row = lv_obj_create(parent);
  lv_obj_remove_style_all(row);
  lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row, 6, 0);
  mk_label(row, t.font_body, t.text_dim, key);
  lv_obj_t* v = mk_label(row, t.font_body, t.text, "—");
  lv_obj_set_width(v, 250);
  lv_obj_set_height(v, lv_font_get_line_height(t.font_body) + 2);
  lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
  return v;
}

lv_obj_t* grid_row(lv_obj_t* parent) {
  lv_obj_t* r = lv_obj_create(parent);
  lv_obj_remove_style_all(r);
  lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(r, 8, 0);
  return r;
}

void set_text(lv_obj_t* label, const char* text) {
  if (label) lv_label_set_text(label, text);
}

// ---- 状態行 (docs/walk-event.md §4.4 の 4.) --------------------------------

bool is_stale(const WalkState& s) {
  const int64_t now = now_epoch();
  return s.updated_epoch > 0 && now > 0 && now - s.updated_epoch >= 60;
}

void refresh_status(const WalkState& s) {
  if (!s_status) return;
  const ui::Theme& t = ui::theme();
  const bool stale = is_stale(s);
  const char* stale_tag = stale ? "(古い情報) " : "";
  char buf[96];
  lv_color_t color = t.text_dim;
  switch (s.status) {
    case WalkStatus::Unknown:
      std::snprintf(buf, sizeof(buf), "スマホの状態を待っています");
      break;
    case WalkStatus::Idle:
      std::snprintf(buf, sizeof(buf), "%sスマホで「開始」を押してください",
                    stale_tag);
      break;
    case WalkStatus::NoRoute:
      std::snprintf(buf, sizeof(buf), "%sスマホでルートを取り込んでください",
                    stale_tag);
      break;
    case WalkStatus::NoFix:
      std::snprintf(buf, sizeof(buf), "%s測位を待っています", stale_tag);
      break;
    case WalkStatus::Finished:
      std::snprintf(buf, sizeof(buf), "到着しました。おつかれさまでした");
      color = t.ok;
      break;
    case WalkStatus::Walking:
    default:
      if (s.off_route_m >= kOffRouteWarnM) {
        // ルート外れは danger 色で目立たせる。
        std::snprintf(buf, sizeof(buf), "ルートから外れています (%lu m)",
                      static_cast<unsigned long>(s.off_route_m));
        color = t.danger;
      } else {
        char upd[12];
        fmt_hhmmss(upd, sizeof(upd), s.updated_epoch);
        if (s.accuracy_m > 0) {
          std::snprintf(buf, sizeof(buf), "%s更新 %s ・ 精度 %lu m", stale_tag,
                        upd, static_cast<unsigned long>(s.accuracy_m));
        } else {
          std::snprintf(buf, sizeof(buf), "%s更新 %s", stale_tag, upd);
        }
      }
      break;
  }
  lv_obj_set_style_text_color(s_status, color, 0);
  lv_label_set_text(s_status, buf);
}

// ---- 全体の再描画 ---------------------------------------------------------

void refresh() {
  const WalkState s = watch::features::walk_state();
  char buf[64];

  // 進捗 % (permille/10) とバー。
  uint32_t permille = s.permille > 1000 ? 1000 : s.permille;
  const uint32_t pct = permille / 10;
  std::snprintf(buf, sizeof(buf), "%lu%%", static_cast<unsigned long>(pct));
  set_text(s_num, buf);
  if (s_fill) lv_obj_set_width(s_fill, lv_pct(static_cast<int>(pct)));

  // 距離 (done / total)。total が無ければ done のみ。
  if (s.total_m > 0) {
    std::snprintf(buf, sizeof(buf), "%lu.%lu / %lu.%lu km",
                  static_cast<unsigned long>(s.done_m / 1000),
                  static_cast<unsigned long>((s.done_m % 1000) / 100),
                  static_cast<unsigned long>(s.total_m / 1000),
                  static_cast<unsigned long>((s.total_m % 1000) / 100));
  } else if (s.done_m > 0) {
    fmt_meters(buf, sizeof(buf), s.done_m);
  } else {
    std::snprintf(buf, sizeof(buf), "—");
  }
  set_text(s_dist, buf);

  fmt_hhmm(buf, sizeof(buf), s.eta_epoch);
  set_text(s_eta, buf);

  if (s.remain_m > 0) {
    fmt_meters(buf, sizeof(buf), s.remain_m);
  } else {
    std::snprintf(buf, sizeof(buf), "—");
  }
  set_text(s_remain, buf);

  fmt_pace(buf, sizeof(buf), s.pace_s_per_km);
  set_text(s_pace, buf);

  if (s.elapsed_s > 0) {
    fmt_elapsed(buf, sizeof(buf), s.elapsed_s);
  } else {
    std::snprintf(buf, sizeof(buf), "—");
  }
  set_text(s_elapsed, buf);

  fmt_hhmm(buf, sizeof(buf), s.started_epoch);
  set_text(s_start, buf);

  // ルート名 (無ければ "—")。
  set_text(s_route, s.route[0] ? s.route : "—");

  // 次のチェックポイント (無ければ行ごと隠す)。
  if (s_next_cp) {
    if (s.next_cp[0]) {
      if (s.next_cp_m > 0) {
        std::snprintf(buf, sizeof(buf), "%s %lu m", s.next_cp,
                      static_cast<unsigned long>(s.next_cp_m));
      } else {
        std::snprintf(buf, sizeof(buf), "%s", s.next_cp);
      }
      lv_obj_remove_flag(s_next_cp, LV_OBJ_FLAG_HIDDEN);
      set_text(s_next_cp, buf);
    } else {
      lv_obj_add_flag(s_next_cp, LV_OBJ_FLAG_HIDDEN);
    }
  }

  refresh_status(s);
}

// ---- 画面構築 -------------------------------------------------------------

lv_obj_t* build(lv_obj_t* scr) {
  const ui::Theme& t = ui::theme();
  lv_obj_set_style_bg_color(scr, t.bg, 0);
  ui::c::header(scr, "ウォーク", true);
  lv_obj_t* col = ui::c::content(scr);

  // 1) 進捗カード: 大きな % とバー、距離。
  lv_obj_t* prog = ui::c::card(col);
  lv_obj_set_style_pad_all(prog, t.space + 2, 0);
  lv_obj_set_style_pad_row(prog, t.space / 2, 0);
  lv_obj_t* top = lv_obj_create(prog);
  lv_obj_remove_style_all(top);
  lv_obj_remove_flag(top, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(top, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(top, t.space, 0);

  s_num = mk_label(top, &font_num_72, t.accent, "0%");
  // 右側: バー + 距離 (大きな数字の高さに合わせて中央寄せ)。
  lv_obj_t* right = lv_obj_create(top);
  lv_obj_remove_style_all(right);
  lv_obj_remove_flag(right, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_grow(right, 1);
  lv_obj_set_height(right, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(right, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(right, t.space / 2, 0);

  lv_obj_t* track = lv_obj_create(right);
  lv_obj_remove_style_all(track);
  lv_obj_set_size(track, LV_PCT(100), 18);
  lv_obj_set_style_bg_color(track, t.surface2, 0);
  lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(track, t.radius_sm, 0);
  s_fill = lv_obj_create(track);
  lv_obj_remove_style_all(s_fill);
  lv_obj_set_size(s_fill, lv_pct(0), lv_pct(100));
  lv_obj_set_style_bg_color(s_fill, t.primary, 0);
  lv_obj_set_style_bg_opa(s_fill, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(s_fill, t.radius_sm, 0);
  lv_obj_align(s_fill, LV_ALIGN_LEFT_MID, 0, 0);

  s_dist = mk_label(right, t.font_body, t.text, "—");
  lv_obj_set_width(s_dist, LV_PCT(100));
  lv_obj_set_height(s_dist, lv_font_get_line_height(t.font_body) + 2);
  lv_label_set_long_mode(s_dist, LV_LABEL_LONG_DOT);

  // 2) 情報カード: 到着予想・残り・経過・開始・ペース・ルート・次の CP。
  lv_obj_t* info = ui::c::card(col);
  lv_obj_set_style_pad_all(info, t.space + 2, 0);
  lv_obj_set_style_pad_row(info, t.space / 2, 0);

  lv_obj_t* r1 = grid_row(info);
  s_eta = kv_cell(r1, "到着");
  s_remain = kv_cell(r1, "残り");

  lv_obj_t* r2 = grid_row(info);
  s_elapsed = kv_cell(r2, "経過");
  s_start = kv_cell(r2, "開始");

  // ペースは "/km" まで出すため全幅、ルートと次 CP も全幅 1 行。
  s_pace = kv_line(info, "ペース");
  s_route = kv_line(info, "ルート");
  s_next_cp = kv_line(info, "次CP");
  lv_obj_add_flag(s_next_cp, LV_OBJ_FLAG_HIDDEN);  // CP が来たら refresh で出す

  // 3) 状態行 (更新時刻・ルート外れ警告・案内)。
  s_status = mk_label(col, t.font_body, t.text_dim, "");
  lv_obj_set_width(s_status, LV_PCT(100));
  lv_label_set_long_mode(s_status, LV_LABEL_LONG_WRAP);

  // 4) いま更新 (即時問い合わせ)。
  ui::c::button_primary(
      col, "いま更新",
      [](lv_event_t*) { ui::emit(watch::ActionType::WalkRefresh); }, nullptr);

  refresh();
  // 画面を開いた瞬間に最新値を要求する (docs/walk-event.md §3.1)。
  ui::emit(watch::ActionType::WalkRefresh);
  return scr;
}

void on_event(lv_obj_t*, const watch::Event& e) {
  if (e.type == watch::EventType::WalkStateChanged ||
      e.type == watch::EventType::SettingsChanged) {
    refresh();
  } else if (e.type == watch::EventType::ClockTick) {
    // 「(古い情報)」表示だけ毎秒更新する (他は変化時のみ)。
    refresh_status(watch::features::walk_state());
  }
}

}  // namespace

namespace ui {

extern const ScreenOps kWalkScreen = {watch::Route::App0, build, on_event};

// applist のサブ行 (app.cmake の APP_SUB が指す)。
const char* walk_applist_sub() {
  static char buf[40];
  const WalkState s = watch::features::walk_state();
  buf[0] = '\0';
  switch (s.status) {
    case WalkStatus::Unknown:
    case WalkStatus::Idle:
      std::snprintf(buf, sizeof(buf), "未開始");
      break;
    case WalkStatus::NoRoute:
      std::snprintf(buf, sizeof(buf), "ルート未設定");
      break;
    case WalkStatus::NoFix:
      std::snprintf(buf, sizeof(buf), "測位待ち");
      break;
    case WalkStatus::Finished: {
      char eta[12];
      fmt_hhmm(eta, sizeof(eta), s.eta_epoch);
      if (s.eta_epoch > 0) {
        std::snprintf(buf, sizeof(buf), "完走 ・ 到着 %s", eta);
      } else {
        std::snprintf(buf, sizeof(buf), "完走");
      }
      break;
    }
    case WalkStatus::Walking:
    default: {
      const uint32_t pct = (s.permille > 1000 ? 1000 : s.permille) / 10;
      char eta[12];
      fmt_hhmm(eta, sizeof(eta), s.eta_epoch);
      if (s.eta_epoch > 0) {
        std::snprintf(buf, sizeof(buf), "%lu%% ・ 到着 %s",
                      static_cast<unsigned long>(pct), eta);
      } else {
        std::snprintf(buf, sizeof(buf), "%lu%%",
                      static_cast<unsigned long>(pct));
      }
      break;
    }
  }
  return buf;
}

}  // namespace ui
