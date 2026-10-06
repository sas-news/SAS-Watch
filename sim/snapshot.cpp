// snapshot.cpp — ホストシミュレータ & スクリーンショット出力。
//   sim/build/snapshot --out docs/screenshots/
//   LVGL のメモリ上ディスプレイ 410x502 に ui コンポーネントを描画し、
//   各画面を PNG に書き出す。実機と同じ ui::create + EventBus + Action の
//   パイプラインを通す (タップも lv_indev 経由で本物のイベントとして打つ)。
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include "lvgl.h"
#include "png.hpp"
#include "sim_platform.hpp"
#include "ui/face_data.hpp"
#include "ui/port.hpp"
#include "ui/ui.hpp"
#include "watch/features/alarm.hpp"
#include "watch/features/media.hpp"
#include "watch/features/agent.hpp"
#include "watch/features/memo.hpp"
#include "watch/features/notify.hpp"
#include "watch/features/steps.hpp"
#include "watch/input_mapper.hpp"
#include "watch/power.hpp"
#include "watch/runtime.hpp"
#include "components.hpp"

namespace {

constexpr int kW = 410;
constexpr int kH = 502;

// LVGL draw buffer (partial render) → flush で全画面フレームへコピー。
uint16_t s_draw[kW * 60];
uint16_t s_fb[kW * kH];
lv_display_t* s_disp = nullptr;
lv_indev_t* s_indev = nullptr;

// core 周り (firmware の watch_app と同じ組み立て)。
watch::EventBus s_bus;
watch::Navigator s_nav(&s_bus);
watch::PowerPolicy s_power(&s_bus);
watch::InputMapper s_input;
watch::FeatureRegistry s_features(watch::builtin_features(),
                                  watch::builtin_features_count());
watch::Settings s_settings;
watch::Runtime s_rt(s_bus, s_nav, s_power, s_input, s_features, s_settings);
watch::FeatureContext* s_fctx = nullptr;

struct Ptr {
  int16_t x = 0, y = 0;
  bool pressed = false;
} s_ptr;

void flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
  // RGB565 little-endian。行ごとにコピー。
  const int w = area->x2 - area->x1 + 1;
  for (int y = area->y1; y <= area->y2; ++y) {
    std::memcpy(&s_fb[y * kW + area->x1],
                px_map + (y - area->y1) * w * 2, w * 2);
  }
  lv_display_flush_ready(disp);
}

void read_cb(lv_indev_t*, lv_indev_data_t* d) {
  d->point.x = s_ptr.x;
  d->point.y = s_ptr.y;
  d->state = s_ptr.pressed ? LV_INDEV_STATE_PRESSED
                           : LV_INDEV_STATE_RELEASED;
}

void sink(const watch::Action& a) { s_rt.queue().push(a); }

void pump(int ms) {
  const int steps = (ms + 29) / 30;
  for (int i = 0; i < steps; ++i) {
    sim::clock().advance_ms(30);
    // app タスク相当: キューを1件処理 + tick + power。
    s_rt.step(sim::clock().now_ms(), *s_fctx);
    lv_timer_handler();
  }
}

bool save(const char* dir, const char* name) {
  char path[512];
  std::snprintf(path, sizeof(path), "%s/%s.png", dir, name);
  const bool ok = write_png_rgb565(path, s_fb, kW, kH);
  std::printf("%s -> %s\n", name, ok ? "ok" : "FAILED");
  return ok;
}

void nav_to(watch::Route r) {
  ui::emit(watch::ActionType::Navigate, static_cast<uint32_t>(r));
  pump(400);  // アニメ 220ms + 余裕
}

void back_home() {
  ui::emit(watch::ActionType::Home);
  pump(400);
}

void tap(int x, int y) {
  s_ptr = {static_cast<int16_t>(x), static_cast<int16_t>(y), true};
  pump(60);
  s_ptr.pressed = false;
  pump(80);
}

// 歩行っぽい ImuSample を n サンプル流す (50ms 間隔想定)。
// 大きさが 1400/600mg を 5 サンプルごとに振動 → 約 1 歩/10 サンプル。
void feed_walk(int n) {
  for (int i = 0; i < n; ++i) {
    watch::Action a{};
    a.type = watch::ActionType::ImuSample;
    a.source = watch::ActionSource::System;  // 計測は電源を蹴らない
    const int16_t z =
        static_cast<int16_t>(-1000 + ((i % 10 < 5) ? -400 : 400));
    a.arg0 = 0;
    a.arg1 = static_cast<uint16_t>(z);
    s_rt.queue().push(a);
    pump(50);
  }
}

// ---- parts ギャラリー ----------------------------------------------
// 全スキンパーツを 1 画面に並べるデバッグ画面。LV_STATE_* を強制付与して
// state 画像差替え (pressed/checked/disabled) を目視確認する。

lv_obj_t* gbox(lv_obj_t* p, int32_t w, int32_t h) {
  lv_obj_t* o = lv_obj_create(p);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(o, 0, 0);
  lv_obj_set_style_pad_all(o, 0, 0);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  return o;
}

lv_obj_t* grow3(lv_obj_t* p, int32_t h) {
  lv_obj_t* r = gbox(p, 366, h);
  lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(r, 6, 0);
  return r;
}

void glabel(lv_obj_t* p, const char* s) {
  lv_obj_t* l = lv_label_create(p);
  lv_label_set_text(l, s);
  lv_obj_set_style_text_font(l, ui::theme().font_body, 0);
  lv_obj_center(l);
}

lv_obj_t* gpart(lv_obj_t* p, watch::ThemeSkinPartId part, int32_t w,
                int32_t h, lv_state_t st, const char* text) {
  lv_obj_t* o = gbox(p, w, h);
  ui::c::skin_obj(o, part);
  if (st) lv_obj_add_state(o, st);
  if (text) glabel(o, text);
  return o;
}

void parts_gallery(const char* out_dir, const char* theme_id, int num,
                   bool* ok) {
  lv_obj_t* prev = lv_screen_active();
  lv_obj_t* g = gbox(nullptr, kW, kH);
  lv_obj_set_style_bg_color(g, ui::theme().bg, 0);
  lv_obj_set_style_bg_opa(g, LV_OPA_COVER, 0);
  ui::c::header(g, "パーツ", true);  // header_bar + back_pill

  lv_obj_t* col = gbox(g, 366, kH - 78);
  lv_obj_set_pos(col, 22, 78);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(col, 3, 0);

  ui::c::caption(col, "ボタン / スイッチ / スライダー");

  // ボタン 3 種 x normal/pressed/disabled。
  using MkBtn = lv_obj_t* (*)(lv_obj_t*, const char*, lv_event_cb_t, void*);
  const struct {
    MkBtn mk;
    const char* tx;
  } btns[3] = {{ui::c::button_primary, "主"},
               {ui::c::button, "副"},
               {ui::c::button_danger, "危"}};
  const lv_state_t sts[3] = {LV_STATE_DEFAULT, LV_STATE_PRESSED,
                             LV_STATE_DISABLED};
  for (const auto& b : btns) {
    lv_obj_t* r = grow3(col, 30);
    for (int i = 0; i < 3; ++i) {
      lv_obj_t* o = b.mk(r, b.tx, nullptr, nullptr);
      lv_obj_set_size(o, 118, 30);
      if (sts[i]) lv_obj_add_state(o, sts[i]);
    }
  }

  // スイッチ off/on/disabled + icon_tile + toast。
  lv_obj_t* ra = grow3(col, 36);
  lv_obj_t* sw1 = ui::c::mk_switch(ra, false);
  lv_obj_t* sw2 = ui::c::mk_switch(ra, true);
  lv_obj_t* sw3 = ui::c::mk_switch(ra, true);
  lv_obj_add_state(sw3, LV_STATE_DISABLED);
  (void)sw1;
  (void)sw2;
  gpart(ra, watch::kSkinIconTile, 36, 36, LV_STATE_DEFAULT, "タ");
  gpart(ra, watch::kSkinToast, 142, 36, LV_STATE_DEFAULT, "通知");

  // back_pill x 3 state。
  lv_obj_t* rb = grow3(col, 30);
  gpart(rb, watch::kSkinBackPill, 96, 30, LV_STATE_DEFAULT, "戻る");
  gpart(rb, watch::kSkinBackPill, 96, 30, LV_STATE_PRESSED, "戻る");
  gpart(rb, watch::kSkinBackPill, 96, 30, LV_STATE_DISABLED, "戻る");

  // スライダー (track/fill/knob)。
  ui::c::slider_row(col, "明るさ", "%", 0, 100, 60, nullptr, nullptr);

  // グループ: list_group + row + divider + icon_tile。
  lv_obj_t* grp = ui::c::group(col);
  lv_obj_t* ri = ui::c::row_icon(grp, "時", lv_color_hex(0x445566), "アプリ",
                               "sub", true, nullptr, nullptr);
  lv_obj_set_style_min_height(ri, 44, 0);
  lv_obj_t* rp = ui::c::row(grp, "押下状態", nullptr, "v", true, nullptr,
                            nullptr);
  lv_obj_set_style_min_height(rp, 34, 0);
  lv_obj_add_state(rp, LV_STATE_PRESSED);
  lv_obj_t* rd = ui::c::row(grp, "無効状態", nullptr, nullptr, false, nullptr,
                            nullptr);
  lv_obj_set_style_min_height(rd, 34, 0);
  lv_obj_add_state(rd, LV_STATE_DISABLED);

  // card + bubble。
  lv_obj_t* rc = grow3(col, 44);
  lv_obj_t* cd = ui::c::card(rc);
  lv_obj_set_size(cd, 172, 44);
  glabel(cd, "カード");
  gpart(rc, watch::kSkinBubble, 188, 44, LV_STATE_DEFAULT, "ふきだし");

  lv_screen_load(g);
  pump(400);
  char nm[64];
  std::snprintf(nm, sizeof(nm), "%d_theme_%s_parts", num, theme_id);
  *ok &= save(out_dir, nm);
  lv_screen_load(prev);
  lv_obj_delete(g);
  pump(200);
}

}  // namespace

int main(int argc, char** argv) {
  const char* out = "docs/screenshots";
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
      out = argv[++i];
    }
  }
  std::filesystem::create_directories(out);

  watch::log_set(&sim::log());

  // 表示 410x502 RGB565。
  lv_init();
  lv_tick_set_cb([]() -> uint32_t {
    return static_cast<uint32_t>(sim::clock().now_ms());
  });
  s_disp = lv_display_create(kW, kH);
  lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
  lv_display_set_flush_cb(s_disp, flush_cb);
  lv_display_set_buffers(s_disp, s_draw, nullptr, sizeof(s_draw),
                         LV_DISPLAY_RENDER_MODE_PARTIAL);
  s_indev = lv_indev_create();
  lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(s_indev, read_cb);
  lv_indev_set_display(s_indev, s_disp);

  // core 起動 (2025-10-05 09:41 UTC, JST 表示で 18:41)。
  sim::clock().set_base_epoch(1759657260);
  s_settings.tz_offset_min = 540;  // JST
  static watch::FeatureContext fctx{s_bus, sim::kv(), sim::clock(),
                                    &s_nav, &s_power};
  fctx.audio = &sim::audio();
  fctx.settings = &s_settings;
  s_fctx = &fctx;
  s_rt.init(fctx);
  s_features.restore_all(fctx);

  // メモを2件仕込んでおく (一覧が見えるように)。
  const char* m1 = "買い物: 牛乳、卵、食パン";
  const char* m2 = "TODO: 実機が届いたら輝度・ジェスチャーの確認をする";
  watch::features::memo_create(m1, std::strlen(m1), fctx);
  watch::features::memo_create(m2, std::strlen(m2), fctx);

  // アラーム 2件 (7:00 平日 / 9:30 毎日) と通知・曲情報。
  watch::features::alarm_set(0, 7, 0, 0x3E, true, fctx);
  watch::features::alarm_set(0, 9, 30, 0, false, fctx);
  watch::features::notify_add("LINE", "母", "夕飯何にする？");
  watch::features::notify_add("Gmail", "GitHub", "[SAS-Watch] PR #9 merged");
  watch::features::media_set("夜に駆ける", "YOASOBI", true, fctx);

  // 文字盤の補助データ (ui/face_data.hpp)。歩数・通知数・次のアラーム
  // は実 Feature に結線 (alarm_next_fire_epoch → 当日 min-of-day)。
  static const ui::face_data::Hooks kFaceData = {
      []() -> int32_t { return static_cast<int32_t>(watch::features::steps_today()); },
      []() -> int32_t { return static_cast<int32_t>(s_settings.steps_goal); },
      []() -> int32_t {
        return static_cast<int32_t>(watch::features::notify_count());
      },
      []() -> int32_t {
        const int64_t e = watch::features::alarm_next_fire_epoch();
        if (e <= 0) return -1;
        const int64_t local = e + s_settings.tz_offset_min * 60;
        return static_cast<int32_t>(((local % 86400) + 86400) % 86400 / 60);
      },
  };
  ui::face_data::set_hooks(&kFaceData);

  // Action 出口 → core キュー。
  ui::set_action_sink(&sink);
  ui::create({&s_bus, &s_nav, &s_settings, &fctx});
  pump(400);

  bool ok = true;
  ok &= save(out, "01_home");

  nav_to(watch::Route::Quick);
  ok &= save(out, "02_quick");

  back_home();
  nav_to(watch::Route::More);
  ok &= save(out, "03_applist");

  nav_to(watch::Route::Timer);
  ok &= save(out, "04_timer_setup");

  // 3分タイマーを開始して 90 秒経過させる (残り 1:30 → リングが半周、
  // 未充填トラックも写る状態で撮る)。無入力だと 12 秒で画面OFFになり、
  // 消灯中の Action は「起こすだけ」で捨てられる (TimerReset が効かず
  // タイマーが動き続ける) ため、間だけ PowerPolicy の消灯を延ばす。
  s_power.configure(8, 300, 1800);
  ui::emit(watch::ActionType::TimerStart, 180);
  pump(90500);
  ok &= save(out, "05_timer_running");
  s_power.configure(8, 12, 1800);

  ui::emit(watch::ActionType::TimerReset);
  back_home();
  nav_to(watch::Route::Stopwatch);
  ok &= save(out, "06_stopwatch");

  ui::emit(watch::ActionType::StopwatchToggle);
  pump(400);
  ui::emit(watch::ActionType::StopwatchLap);
  pump(400);
  ok &= save(out, "07_stopwatch_running");

  ui::emit(watch::ActionType::StopwatchReset);
  back_home();
  nav_to(watch::Route::Counter);
  ui::emit(watch::ActionType::CounterAdd, 1);
  ui::emit(watch::ActionType::CounterAdd, 1);
  ui::emit(watch::ActionType::CounterAdd, 1);
  pump(200);
  ok &= save(out, "08_counter");

  back_home();
  nav_to(watch::Route::Memo);
  ok &= save(out, "09_memo_list");
  // 先頭行をタップして詳細 (録音カード ~140px の下から行が始まる)。
  tap(205, 210);
  pump(200);
  ok &= save(out, "10_memo_detail");
  // 「一覧に戻る」ボタン (詳細カード末尾) をタップ。
  tap(205, 249);
  pump(200);

  back_home();
  nav_to(watch::Route::Settings);
  ok &= save(out, "11_settings");

  // 「文字盤」「数字フォント」は行タップで選択ビューに開く (掘り下げ)。
  // 実測: 文字盤行センター y≈338、数字フォント y≈399、選択ビューの戻る行 y≈106。
  tap(205, 338);  // 文字盤
  pump(200);
  ok &= save(out, "30_settings_faces");
  tap(205, 106);  // ‹ 戻る
  pump(200);
  tap(205, 399);  // 数字フォント
  pump(200);
  ok &= save(out, "31_settings_fonts");
  tap(205, 106);  // ‹ 戻る
  pump(200);

  nav_to(watch::Route::PowerMenu);
  ok &= save(out, "12_powermenu");

  // パスキー確認モーダル。
  back_home();
  ui::request_passkey(483920);
  pump(300);
  ok &= save(out, "13_passkey");
  // 「はい」ボタン (下端 -86 のピル → 中心 y≈387) をタップして閉じる。
  // 閉じ損なうと layer_top の全画面モーダルが残って以降の shot を覆う。
  tap(205, 387);
  pump(200);

  // タイマー終了フルスクリーン通知。
  back_home();
  s_bus.publish({watch::EventType::TimerFinished, 0});
  pump(300);
  ok &= save(out, "14_timer_alert");
  tap(205, 387);  // 止める
  pump(200);

  // ---- テーマ切替 (docs/theme-format.md) ----
  // 内蔵の明るめテーマ。
  back_home();
  ui::emit_text(watch::ActionType::SetTheme, "light");
  pump(500);
  ok &= save(out, "15_theme_light_home");
  nav_to(watch::Route::Settings);
  ok &= save(out, "16_theme_light_settings");

  // サンプルキャラテーマ (sim/themes/mame。tools/build_themes.py で生成)。
  back_home();
  ui::emit_text(watch::ActionType::SetTheme, "mame");
  pump(500);
  ok &= save(out, "17_theme_mame_home");
  s_bus.publish({watch::EventType::TimerFinished, 0});
  pump(300);
  ok &= save(out, "18_theme_mame_timer");
  tap(205, 387);  // 止める
  pump(200);

  // 元に戻しておく。
  ui::emit_text(watch::ActionType::SetTheme, "standard");
  pump(400);

  // 音声メモ: 録音中の表示 → 確定 → 音声メモ詳細 (再生ボタン)。
  back_home();
  nav_to(watch::Route::Memo);
  ui::emit(watch::ActionType::MemoRecordStart);
  pump(2500);
  ok &= save(out, "19_memo_recording");
  ui::emit(watch::ActionType::MemoRecordStop);
  pump(400);
  // 先頭行 (新しい音声メモ) を開く。
  tap(205, 210);
  pump(300);
  ok &= save(out, "20_memo_voice_detail");

  // ファーム更新画面: 待機 → 進捗中。
  back_home();
  nav_to(watch::Route::Ota);
  ok &= save(out, "21_ota_idle");
  sim::set_ota_debug(2, 42, "", "0.2.0");
  s_bus.publish({watch::EventType::OtaProgress, 42});
  pump(300);
  ok &= save(out, "22_ota_progress");

  // ---- アラーム ----
  back_home();
  nav_to(watch::Route::Alarm);
  ok &= save(out, "40_alarm");

  // 鳴動アラート: 今この分のアラームを登録すると次の tick で鳴る。
  watch::features::alarm_set(0, 18, 41, 0, true, fctx);
  pump(400);
  ok &= save(out, "41_alarm_alert");
  tap(205, 387);  // 止める (下端 -86 のピル中心)
  pump(300);

  // ---- 通知一覧 ----
  nav_to(watch::Route::Notifications);
  ok &= save(out, "42_notifications");
  // 先頭行をタップして詳細 (アプリ名・タイトル・本文の全体表示)。
  tap(205, 130);
  pump(200);
  ok &= save(out, "43_notification_detail");
  tap(205, 290);  // 一覧に戻る (detail カード末尾のボタン)
  pump(200);

  // 通知ポップアップ: notify.post 相当 (store 追加 + NotificationPosted)。
  watch::features::notify_add("LINE", "兄", "今週末帰るよ");
  s_bus.publish({watch::EventType::NotificationPosted, 0});
  pump(300);
  ok &= save(out, "44_notify_popup");
  pump(4500);  // トーストが消えるまで待つ

  // ---- 音楽操作 ----
  back_home();
  nav_to(watch::Route::Media);
  ok &= save(out, "45_media");

  // 歩数: 歩行っぽい加速度を流してから歩数画面を開く。
  // (ImuSample は電源を蹴らないので、途中で PowerState は ScreenOff へ
  //  進んでいる。先に Wake で起こしてから遷移する。)
  back_home();
  feed_walk(24000);  // ~2400 歩 (目標 8000 の ~30%)
  ui::emit(watch::ActionType::Wake);
  pump(200);
  nav_to(watch::Route::Steps);
  ok &= save(out, "23_steps");

  // ---- 文字盤 (settings.face) ----
  // standard テーマで4面 (01_home = bold と同じ見えになるが、
  // 文字盤名を揃えて残すため明示的に撮る)。
  back_home();
  ui::emit_text(watch::ActionType::SetTheme, "standard");
  pump(400);
  const char* kFaceShots[][2] = {
      {"bold", "24_face_bold"},
      {"analog", "25_face_analog"},
      {"hud", "26_face_hud"},
      {"minimal", "27_face_minimal"},
  };
  for (const auto& fs : kFaceShots) {
    ui::emit_text(watch::ActionType::SetFace, fs[0]);
    pump(300);
    ok &= save(out, fs[1]);
  }
  // chara_* は立ち絵 (face_chara) のある mame テーマで。
  ui::emit_text(watch::ActionType::SetTheme, "mame");
  pump(500);
  ui::emit_text(watch::ActionType::SetFace, "chara_side");
  pump(300);
  ok &= save(out, "28_face_chara_side");
  ui::emit_text(watch::ActionType::SetFace, "chara_bubble");
  pump(300);
  ok &= save(out, "29_face_chara_bubble");
  // 元に戻す。
  ui::emit_text(watch::ActionType::SetFace, "bold");
  ui::emit_text(watch::ActionType::SetTheme, "standard");
  pump(300);


  // ---- AI (Agent) ----
  // sim には BLE が無いので、ble_connected を立てておき、送信完了/返答は
  // feature の API を直接叩く (実機では ble_glue が agent_pending を拾う)。
  s_power.set_ble_connected(true);
  back_home();
  nav_to(watch::Route::Agent);
  ok &= save(out, "50_agent");

  ui::emit(watch::ActionType::AgentRecordToggle);
  pump(2500);
  ok &= save(out, "51_agent_recording");
  ui::emit(watch::ActionType::AgentRecordToggle);  // 停止→送信中
  pump(300);

  // 定型質問で「考え中→返答」まで再現する。
  watch::features::agent_reset_state();
  ui::emit(watch::ActionType::AgentAsk, 0);  // agent.q1
  pump(300);
  watch::features::AgentPending ap;
  if (watch::features::agent_pending(&ap)) {
    watch::features::agent_send_started(ap.id);
    watch::features::agent_sent(ap.id, true, *s_fctx);
  }
  pump(100);
  ok &= save(out, "52_agent_thinking");
  const char* reply =
      "今日は 15 時にミーティング、19 時にジムの予定があります。";
  watch::features::agent_on_reply(ap.id, reply, std::strlen(reply),
                                  *s_fctx);
  pump(300);
  ok &= save(out, "53_agent_reply");

  // ---- テーマ v2 サンプル「cosmos」 ----
  // sim/themes/cosmos.zip を zip 直読み (展開済み dir は読まない)。
  // screens/style/icons/fonts/mascot/face_layout の v2 全キーを行使。
  back_home();
  ui::emit_text(watch::ActionType::SetFace, "theme");  // face_layout 駆動
  ui::emit_text(watch::ActionType::SetTheme, "cosmos");
  pump(800);
  ok &= save(out, "60_theme_cosmos_home");   // home + theme 文字盤

  nav_to(watch::Route::More);
  pump(200);
  ok &= save(out, "61_theme_cosmos_applist");  // icons + mascot

  nav_to(watch::Route::Timer);
  ok &= save(out, "62_theme_cosmos_timer");    // 画面別 bg + mascot

  back_home();
  nav_to(watch::Route::Memo);
  pump(200);
  ok &= save(out, "63_theme_cosmos_memo");

  back_home();
  nav_to(watch::Route::Settings);
  ok &= save(out, "64_theme_cosmos_settings");  // scrim 高め + style

  // タイマー終了アラート (screens.alert bg + images.timer_done + mascot)。
  back_home();
  s_bus.publish({watch::EventType::TimerFinished, 0});
  pump(300);
  ok &= save(out, "65_theme_cosmos_alert");
  tap(205, 387);  // 止める
  pump(300);

  // cosmos の face_chara は images スロット経由で既存文字盤にも効く。
  // chara 系レイアウトを cosmos 背景で撮る。
  back_home();
  ui::emit_text(watch::ActionType::SetFace, "chara_side");
  pump(300);
  ok &= save(out, "66_theme_cosmos_face_chara_side");
  ui::emit_text(watch::ActionType::SetFace, "chara_bubble");
  pump(300);
  ok &= save(out, "67_theme_cosmos_face_chara_bubble");

  // quick 設定画面も撮る (mascot 対象外画面の見え確認)。
  ui::emit_text(watch::ActionType::SetFace, "theme");
  nav_to(watch::Route::Quick);
  pump(200);
  ok &= save(out, "68_theme_cosmos_quick");

  // ---- テーマ v3 サンプル「cyber」「cute」 ----
  // sim/themes/<id>.zip (tools/build_themes.py --kit が kit.html から生成)。
  // skin の 9-slice 画像スキンが全コンポーネントに効く。
  for (const char* theme_id : {"cyber", "cute"}) {
    const bool cy = theme_id[0] == 'c' && theme_id[1] == 'y';
    int n = cy ? 70 : 80;
    char nm[64];
    auto shot = [&](const char* tail) {
      std::snprintf(nm, sizeof(nm), "%d_theme_%s_%s", n++, theme_id, tail);
      ok &= save(out, nm);
    };

    back_home();
    ui::emit_text(watch::ActionType::SetFace, "bold");  // face_layout は無い
    ui::emit_text(watch::ActionType::SetTheme, theme_id);
    pump(800);
    shot("home");  // 画面 bg + スキン

    nav_to(watch::Route::More);
    pump(200);
    shot("applist");  // list_group / row / icon_tile

    back_home();
    nav_to(watch::Route::Settings);
    pump(200);
    shot("settings");  // switch on/off + slider + caption_line

    back_home();
    nav_to(watch::Route::Timer);
    pump(200);
    shot("timer");  // button_primary/secondary

    back_home();
    nav_to(watch::Route::Quick);
    pump(200);
    shot("quick");

    back_home();
    nav_to(watch::Route::Memo);
    pump(200);
    shot("memo");

    // タイマー終了アラート (button_primary/danger + bubble skin)。
    back_home();
    s_bus.publish({watch::EventType::TimerFinished, 0});
    pump(300);
    shot("alert");
    tap(205, 387);  // 止める
    pump(300);

    // 全パーツ x 全 state のギャラリー (77 / 87)。
    parts_gallery(out, theme_id, n++, &ok);

    std::printf("%s skin bytes: %u\n", theme_id,
                static_cast<unsigned>(ui::theme_skin_bytes()));
  }

  // 戻して終了。
  ui::emit_text(watch::ActionType::SetFace, "bold");
  ui::emit_text(watch::ActionType::SetTheme, "standard");
  pump(400);

  std::printf("done -> %s (%s)\n", out, ok ? "ok" : "some failed");
  return ok ? 0 : 1;
}
