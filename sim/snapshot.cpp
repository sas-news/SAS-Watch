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
#include "ui/port.hpp"
#include "ui/ui.hpp"
#include "watch/features/alarm.hpp"
#include "watch/features/media.hpp"
#include "watch/features/memo.hpp"
#include "watch/features/notify.hpp"
#include "watch/input_mapper.hpp"
#include "watch/power.hpp"
#include "watch/runtime.hpp"

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

  ui::emit(watch::ActionType::TimerStart, 90);
  pump(300);
  ok &= save(out, "05_timer_running");

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
  // 先頭行をタップして詳細 (削除ボタンが見える)。
  // 録音カードが上にあるので行の中心は y≈200。
  tap(205, 200);
  pump(200);
  ok &= save(out, "10_memo_detail");
  tap(205, 290);  // 一覧に戻る (削除の下)
  pump(200);

  back_home();
  nav_to(watch::Route::Settings);
  ok &= save(out, "11_settings");

  nav_to(watch::Route::PowerMenu);
  ok &= save(out, "12_powermenu");

  // パスキー確認モーダル。
  back_home();
  ui::request_passkey(483920);
  pump(300);
  ok &= save(out, "13_passkey");
  // 「はい」ボタン (中央 +120 → y≈371) をタップして閉じる。
  // 閉じ損なうと layer_top の全画面モーダルが残って以降の shot を覆う。
  tap(205, 371);
  pump(200);

  // タイマー終了フルスクリーン通知。
  back_home();
  s_bus.publish({watch::EventType::TimerFinished, 0});
  pump(300);
  ok &= save(out, "14_timer_alert");
  tap(205, 290);  // 止める
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
  tap(205, 290);  // 止める
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
  tap(205, 190);
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
  tap(205, 281);  // 止める (中央 +30 のボタン中心)
  pump(300);

  // ---- 通知一覧 ----
  nav_to(watch::Route::Notifications);
  ok &= save(out, "42_notifications");
  // 先頭行をタップして詳細 (アプリ名・タイトル・本文の全体表示)。
  tap(205, 130);
  pump(200);
  ok &= save(out, "43_notification_detail");
  tap(205, 290);  // 一覧に戻る
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
  std::printf("done -> %s (%s)\n", out, ok ? "ok" : "some failed");
  return ok ? 0 : 1;
}
