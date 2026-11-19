// esp_display.cpp — firmware 用の表示起動。
// BSP v2 の bsp_display_lcd_init 相当をここで行う。
// 理由: BSP は esp_lcd_panel_handle_t を内部 static に持ち、
// esp_lcd_panel_disp_on_off (画面OFF) に必要なハンドルを外に出さないため、
// 同じ手順でこちらがハンドルを保持する (board.md / BSP v2.0.0 ソースより)。
//
// 手順は BSP ソース (esp32_s3_touch_amoled_2_06.c) と同じ:
//   lvgl_port_init → bsp_display_new → lvgl_port_add_disp
//   → rounder (CO5300 は x/y が偶数境界必須) → bsp_touch_new
//   → lvgl_port_add_touch → bsp_display_brightness_init
//
// フラッシュは port 標準ではなく、2 枚の PSRAM バンド描画バッファ +
// core1 専用 lcd_send タスクによる非同期パイプラインで行う:
//   - LVGL が PSRAM バンド i+1 を描く間に、lcd_send がバンド i を
//     内蔵 SRAM の 8.2KB DMA バウンスバッファ 2 枚へ 32bit コピー+バイトスワップ
//     しつつ QSPI 80MHz へ送る (1 バンドあたり CASET/RASET 1 回 + 2Ch/3Ch)。
//   - 完了 ISR (on_chunk_sent) が空きスロットを返し、明示的に portYIELD_FROM_ISR()
//     を呼ぶことでチャンク間の最大 1ms ティック待ちを無くす。
//   - flush_wait_cb (band_wait_cb) はスピンではなくセマフォ待機に置き換える。
#include "ui/port.hpp"
#include "ui/ui.hpp"
#include "components.hpp"

#include "esp_attr.h"
#include "esp_err.h"  // bsp/display.h が esp_err_t を自分では include しない
#include "bsp/display.h"
#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include "bsp/touch.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_types.h"
#include "esp_lvgl_port.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include <atomic>
#include <cmath>
#include <cstring>

static const char* TAG = "ui.display";

static esp_lcd_panel_io_handle_t s_io = nullptr;
static esp_lcd_panel_handle_t s_panel = nullptr;
static esp_lcd_touch_handle_t s_tp = nullptr;
static lv_display_t* s_disp = nullptr;
static lv_indev_t* s_indev = nullptr;

// perf_mark_screen で積まれた画面名を、次の RENDER_READY で消費する。
static const char* s_perf_screen = nullptr;
static uint32_t s_perf_render_start = 0;

static void perf_render_cb(lv_event_t* e) {
  if (lv_event_get_code(e) == LV_EVENT_RENDER_START) {
    s_perf_render_start = lv_tick_get();
  } else if (s_perf_screen) {
    ESP_LOGI("perf", "screen=%s render=%lums", s_perf_screen,
             (unsigned long)lv_tick_elaps(s_perf_render_start));
    s_perf_screen = nullptr;
  }
}

// BSP rounder_event_cb と同じ (esp32_s3_touch_amoled_2_06.c):
// CO5300 の invalid area は偶数座標でないとチラつくため丸める。
static void rounder_event_cb(lv_event_t* e) {
  lv_area_t* area = static_cast<lv_area_t*>(lv_event_get_param(e));
  uint16_t x1 = area->x1;
  uint16_t x2 = area->x2;
  uint16_t y1 = area->y1;
  uint16_t y2 = area->y2;

  area->x1 = (x1 >> 1) << 1;
  area->y1 = (y1 >> 1) << 1;
  area->x2 = ((x2 >> 1) << 1) + 1;
  area->y2 = ((y2 >> 1) << 1) + 1;
}

// ---- 非同期 2 バンド + バウンスバッファ・フラッシュ ----
// LVGL 描画バッファは 126 行 × 2 枚 (~202KB, PSRAM) に縮小し、
// 描画 (LVGL core1 prio4) と QSPI DMA 転送 (lcd_send core1 prio5) を重ねる。
// バウンスバッファ 2 枚 (8.2KB × 2, 内蔵 DMA SRAM) を ping-pong で使い、
// チャンク i の DMA 送信中にチャンク i+1 の 32bit コピー+バイトスワップを進める。
static constexpr int32_t kBounceRows = 10;
static constexpr size_t kBounceBytes = BSP_LCD_H_RES * kBounceRows * 2;  // 8200B
static constexpr int kPanelGapX = 0x16;  // BSP esp_lcd_panel_set_gap(0x16, 0)
static constexpr int kPanelGapY = 0;

struct BandJob {
  const uint8_t* pixels;  // nullptr は lcd_send_run 用のパネル制御呼び出し
  int32_t x1, y1, x2, y2;
};

static uint8_t* s_bounce[2] = {};
static QueueHandle_t s_bands = nullptr;
static SemaphoreHandle_t s_chunk_free = nullptr;
static SemaphoreHandle_t s_band_done = nullptr;
static SemaphoreHandle_t s_call_lock = nullptr;
static SemaphoreHandle_t s_call_done = nullptr;
static void (*s_call_fn)(void*) = nullptr;
static void* s_call_arg = nullptr;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile int s_chunks_out = 0;
static volatile bool s_band_busy = false;

// フリーズ切り分け用: flush 入出カウンタと段階。
// stage: 0=idle 1=window(2A/2B) 2=copy_swap 3=wire_idle 4=tx_color 5=call。
static volatile uint32_t s_flush_in = 0, s_flush_out = 0,
                         s_flush_stage = 0;
static constexpr TickType_t kSlotTimeout = pdMS_TO_TICKS(1000);

static inline int qspi_cmd(uint8_t c) {
  return static_cast<int>((0x02u << 24) | (static_cast<uint32_t>(c) << 8));
}

static inline int qspi_pixels(uint8_t c) {
  return static_cast<int>((0x32u << 24) | (static_cast<uint32_t>(c) << 8));
}

// 1 パスで PSRAM → 内蔵 DMA バッファのコピー + RGB565 バイトスワップを行う
// (LV_COLOR_FORMAT_RGB565 を維持し、テーマ/PNG/キャッシュ/sim への影響を防ぐ)。
static void copy_swap_rgb565(uint8_t* dst, const uint8_t* src, size_t bytes) {
  size_t i = 0;
  if ((reinterpret_cast<uintptr_t>(dst) & 3u) == 0 &&
      (reinterpret_cast<uintptr_t>(src) & 3u) == 0) {
    const size_t words = bytes >> 2;
    auto* d32 = reinterpret_cast<uint32_t*>(dst);
    const auto* s32 = reinterpret_cast<const uint32_t*>(src);
    for (size_t w = 0; w < words; ++w) {
      const uint32_t v = s32[w];
      d32[w] = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
    }
    i = words << 2;
  }
  for (; i + 1 < bytes; i += 2) {
    dst[i] = src[i + 1];
    dst[i + 1] = src[i];
  }
}

// esp_lcd_panel_io 色転送完了 ISR。
// ESP-IDF の esp_lcd_panel_io_spi は戻り値を無視するため、高優先度タスク起床時は
// 明示的に portYIELD_FROM_ISR() を呼んでチャンク間の最大 1ms 待ちを無くす。
static bool IRAM_ATTR on_chunk_sent(esp_lcd_panel_io_handle_t,
                                    esp_lcd_panel_io_event_data_t*, void*) {
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(s_chunk_free, &woken);
  bool last = false;
  portENTER_CRITICAL_ISR(&s_lock);
  if (s_chunks_out > 0 && --s_chunks_out == 0) {
    last = true;
    s_band_busy = false;
    if (s_disp) lv_display_flush_ready(s_disp);
    s_flush_out++;
  }
  portEXIT_CRITICAL_ISR(&s_lock);
  if (last) {
    xSemaphoreGiveFromISR(s_band_done, &woken);
  }
  if (woken == pdTRUE) {
    portYIELD_FROM_ISR();
  }
  return woken == pdTRUE;
}

// 送信中チャンクが 0 件 (held=0) または最大 1 件未満 (held=1) まで待つ。
// spi_bus_lock の割り込み引き渡し競合を避けるため、次トランザクション開始前に
// 前の転送が線路上から捌けていることを保証する (muse_lcd_bands.c 方式)。
static bool wire_idle(int held) {
  for (int i = held; i < 2; ++i) {
    if (xSemaphoreTake(s_chunk_free, kSlotTimeout) != pdTRUE) {
      ESP_LOGE(TAG, "wire_idle(%d) timeout int_free=%u", held,
               heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
      for (int j = held; j < i; ++j) xSemaphoreGive(s_chunk_free);
      return false;
    }
  }
  for (int i = held; i < 2; ++i) {
    xSemaphoreGive(s_chunk_free);
  }
  return true;
}

static void band_failed() {
  portENTER_CRITICAL(&s_lock);
  s_chunks_out = 0;
  if (s_disp) lv_display_flush_ready(s_disp);
  s_band_busy = false;
  s_flush_out++;
  portEXIT_CRITICAL(&s_lock);
  xSemaphoreGive(s_band_done);
}

static void lcd_send_task(void*) {
  int k = 0;
  BandJob job{};
  for (;;) {
    xQueueReceive(s_bands, &job, portMAX_DELAY);
    if (!job.pixels) {
      s_flush_stage = 5;
      wire_idle(0);
      if (s_call_fn) s_call_fn(s_call_arg);
      s_flush_stage = 0;
      xSemaphoreGive(s_call_done);
      continue;
    }

    const int32_t w = job.x2 - job.x1 + 1;
    const int32_t h = job.y2 - job.y1 + 1;
    const size_t row_bytes = static_cast<size_t>(w) * 2;
    int32_t rows = static_cast<int32_t>(kBounceBytes / row_bytes) & ~1;
    if (rows < 2) rows = 2;
    if (rows > h) rows = h;
    const int chunks = static_cast<int>((h + rows - 1) / rows);

    s_flush_stage = 3;
    if (!wire_idle(0)) {
      band_failed();
      s_flush_stage = 0;
      continue;
    }

    // 1 バンドにつき 1 回だけ CASET (0x2A) / RASET (0x2B) を送る。
    s_flush_stage = 1;
    const int xs = job.x1 + kPanelGapX;
    const int xe = job.x2 + kPanelGapX;
    const int ys = job.y1 + kPanelGapY;
    const int ye = job.y2 + kPanelGapY;
    const uint8_t ca[4] = {
        static_cast<uint8_t>((xs >> 8) & 0xFF), static_cast<uint8_t>(xs & 0xFF),
        static_cast<uint8_t>((xe >> 8) & 0xFF), static_cast<uint8_t>(xe & 0xFF),
    };
    const uint8_t ra[4] = {
        static_cast<uint8_t>((ys >> 8) & 0xFF), static_cast<uint8_t>(ys & 0xFF),
        static_cast<uint8_t>((ye >> 8) & 0xFF), static_cast<uint8_t>(ye & 0xFF),
    };
    if (esp_lcd_panel_io_tx_param(s_io, qspi_cmd(0x2A), ca, 4) != ESP_OK ||
        esp_lcd_panel_io_tx_param(s_io, qspi_cmd(0x2B), ra, 4) != ESP_OK) {
      ESP_LOGE(TAG, "CASET/RASET failed y=%ld..%ld", (long)job.y1,
               (long)job.y2);
      band_failed();
      s_flush_stage = 0;
      continue;
    }

    portENTER_CRITICAL(&s_lock);
    s_chunks_out = chunks;
    portEXIT_CRITICAL(&s_lock);

    bool ok = true;
    for (int32_t y = 0; y < h; y += rows) {
      const int32_t n = (h - y < rows) ? (h - y) : rows;
      if (xSemaphoreTake(s_chunk_free, kSlotTimeout) != pdTRUE) {
        ESP_LOGE(TAG, "bounce slot stall y=%ld/%ld int_free=%u", (long)y,
                 (long)h, heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        ok = false;
        break;
      }
      s_flush_stage = 2;
      copy_swap_rgb565(s_bounce[k], job.pixels + static_cast<size_t>(y) * row_bytes,
                       static_cast<size_t>(n) * row_bytes);
      s_flush_stage = 3;
      if (!wire_idle(1)) {
        xSemaphoreGive(s_chunk_free);
        ok = false;
        break;
      }
      s_flush_stage = 4;
      const uint8_t cmd = (y == 0) ? 0x2C : 0x3C;
      const esp_err_t err = esp_lcd_panel_io_tx_color(
          s_io, qspi_pixels(cmd), s_bounce[k], static_cast<size_t>(n) * row_bytes);
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "tx_color failed: %s", esp_err_to_name(err));
        xSemaphoreGive(s_chunk_free);
        ok = false;
        break;
      }
      k ^= 1;
    }
    if (!ok) {
      band_failed();
    }
    s_flush_stage = 0;
  }
}

static void lcd_send_run(void (*fn)(void*), void* arg) {
  if (!s_bands) {
    fn(arg);
    return;
  }
  xSemaphoreTake(s_call_lock, portMAX_DELAY);
  s_call_fn = fn;
  s_call_arg = arg;
  const BandJob call = {nullptr, 0, 0, 0, 0};
  xQueueSend(s_bands, &call, portMAX_DELAY);
  xSemaphoreTake(s_call_done, portMAX_DELAY);
  xSemaphoreGive(s_call_lock);
}

static void band_flush_cb(lv_display_t*, const lv_area_t* area,
                          uint8_t* px_map) {
  s_flush_in++;
  xSemaphoreTake(s_band_done, 0);  // LVGL が待たなかった完了分を掃除
  s_band_busy = true;
  const BandJob job = {px_map, area->x1, area->y1, area->x2, area->y2};
  if (xQueueSend(s_bands, &job, pdMS_TO_TICKS(1000)) != pdTRUE) {
    ESP_LOGE(TAG, "band queue full");
    band_failed();
  }
}

// LVGL の既定スピン待ちをセマフォ待機に差し替える (core1 上の優先度継承による
// 飢餓・デッドロックを防ぐ)。
static void band_wait_cb(lv_display_t*) {
  int tries = 0;
  while (s_band_busy) {
    if (xSemaphoreTake(s_band_done, pdMS_TO_TICKS(100)) != pdTRUE) {
      if (++tries >= 15) {
        ESP_LOGE(TAG, "band wait timeout (flush %lu/%lu st%lu chunks=%d)",
                 (unsigned long)s_flush_in, (unsigned long)s_flush_out,
                 (unsigned long)s_flush_stage, s_chunks_out);
        band_failed();
        break;
      }
    }
  }
}

namespace ui {

void perf_mark_screen(const char* name) { s_perf_screen = name; }

// ---- 描画ベンチ (perf_bench) ----
// 縦にスクロールできる最初の可視オブジェクト (content() の列など)。
static lv_obj_t* bench_find_scrollable(lv_obj_t* o) {
  const uint32_t n = lv_obj_get_child_count(o);
  for (uint32_t i = 0; i < n; i++) {
    lv_obj_t* c = lv_obj_get_child(o, i);
    if (lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN)) continue;
    if (lv_obj_has_flag(c, LV_OBJ_FLAG_SCROLLABLE) &&
        lv_obj_get_scroll_top(c) + lv_obj_get_scroll_bottom(c) > 64) {
      return c;
    }
    if (lv_obj_t* r = bench_find_scrollable(c)) return r;
  }
  return nullptr;
}

static uint32_t bench_refr_us() {
  const int64_t t0 = esp_timer_get_time();
  lv_refr_now(s_disp);
  band_wait_cb(s_disp);
  return static_cast<uint32_t>(esp_timer_get_time() - t0);
}

static SemaphoreHandle_t s_bench_done = nullptr;

// LVGL タスク上で走る (lv_async_call)。描画は呼び出し側スタックで
// 行われるので、4KB の sdbg タスクから直接 lv_refr_now しない。
static void bench_run(void*) {
  lv_obj_t* scr = lv_screen_active();
  constexpr int kFull = 6;
  uint32_t full_sum = 0, full_max = 0;
  c::skin_stats_reset();
  lv_refr_now(s_disp);  // 溜まっている無効領域を先に吐く
  band_wait_cb(s_disp);
  for (int i = 0; i < kFull; i++) {
    lv_obj_invalidate(scr);
    const uint32_t us = bench_refr_us();
    full_sum += us;
    full_max = LV_MAX(full_max, us);
  }
  constexpr int kSteps = 16;
  constexpr int kDy = 12;
  uint32_t sc_sum = 0, sc_max = 0;
  int sc_n = 0;
  if (lv_obj_t* col = bench_find_scrollable(scr)) {
    const int32_t y0 = lv_obj_get_scroll_y(col);
    for (int dir = 0; dir < 2; dir++) {
      for (int i = 0; i < kSteps; i++) {
        lv_obj_scroll_by(col, 0, dir == 0 ? -kDy : kDy, LV_ANIM_OFF);
        const uint32_t us = bench_refr_us();
        sc_sum += us;
        sc_max = LV_MAX(sc_max, us);
        sc_n++;
      }
    }
    lv_obj_scroll_to_y(col, y0, LV_ANIM_OFF);
    lv_refr_now(s_disp);
  }
  ESP_LOGI("bench",
           "screen=%s full avg=%lu max=%lu ms scroll(n=%d) avg=%lu max=%lu ms "
           "int_free=%u int_largest=%u psram_free=%u psram_largest=%u",
           current_screen_name(), (unsigned long)(full_sum / kFull / 1000),
           (unsigned long)(full_max / 1000), sc_n,
           (unsigned long)(sc_n ? sc_sum / sc_n / 1000 : 0),
           (unsigned long)(sc_max / 1000),
           heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
           heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
  // スキン描画の呼数内訳 (blit=キャッシュ等倍, miss=9-slice 伸縮,
  // rej_size=大きすぎてキャッシュ不能, rej_room=予算不足で構築断念)。
  // LVGL9 は描画タスク遅延なので us は実描画を含まず、呼数と画素数で見る。
  uint64_t blit_us = 0, miss_us = 0;
  uint32_t blit_n = 0, miss_n = 0, blit_px = 0, miss_px = 0;
  uint32_t rej_size = 0, rej_room = 0;
  uint32_t fill_n = 0, band_n = 0, imgreg_n = 0, bail[10] = {0};
  c::skin_stats(&blit_us, &blit_n, &miss_us, &miss_n, &blit_px, &miss_px,
                &rej_size, &rej_room, &fill_n, &band_n, &imgreg_n, bail);
  ESP_LOGI("bench",
           "skinstats: screen=%s blit(n=%lu px=%lu) miss(n=%lu px=%lu) "
           "rej(size=%lu room=%lu %lux%lu<-%lux%lu) fill=%lu band=%lu "
           "imgreg=%lu "
           "bail(fmt=%lu scan=%lu alpha=%lu nonuni=%lu runs=%lu bands=%lu)",
           current_screen_name(), (unsigned long)blit_n,
           (unsigned long)blit_px, (unsigned long)miss_n,
           (unsigned long)miss_px, (unsigned long)rej_size,
           (unsigned long)rej_room, (unsigned long)bail[6],
           (unsigned long)bail[7], (unsigned long)bail[8],
           (unsigned long)bail[9], (unsigned long)fill_n,
           (unsigned long)band_n, (unsigned long)imgreg_n,
           (unsigned long)bail[0], (unsigned long)bail[1],
           (unsigned long)bail[2], (unsigned long)bail[3],
           (unsigned long)bail[4], (unsigned long)bail[5]);
  xSemaphoreGive(s_bench_done);
}

void perf_bench() {
  if (!s_disp) return;
  if (!s_bench_done) s_bench_done = xSemaphoreCreateBinary();
  if (!s_bench_done || !lvgl_port_lock(1000)) {
    ESP_LOGW(TAG, "bench: lvgl lock busy (flush %lu/%lu st%lu)",
             (unsigned long)s_flush_in, (unsigned long)s_flush_out,
             (unsigned long)s_flush_stage);
    return;
  }
  lv_async_call(bench_run, nullptr);
  lvgl_port_unlock();
  if (xSemaphoreTake(s_bench_done, pdMS_TO_TICKS(15000)) != pdTRUE) {
    ESP_LOGW(TAG, "bench timeout (flush %lu/%lu st%lu)",
             (unsigned long)s_flush_in, (unsigned long)s_flush_out,
             (unsigned long)s_flush_stage);
  }
}

// indev の PRESSED で Wake Action を emit する (消灯中のタップ起こし)。
// widget に当たらない場所のタップでも発火するので全面で起きる。
// 押下中は LV_EVENT_PRESSED が read 毎 (~66回/秒) に立つため、Wake emit は
// 間引く。さもないと action queue (40件) が Wake で溢れ、Back/PrimaryAction
// 等の本物の入力が drop される (実機ログで確認)。
static uint32_t s_last_wake_emit;
// パネル消灯状態 (display_power(false))。LVGL 側で知る手段が無いので
// ui::port::display_power で更新する。消灯中のタップは「起こすだけ」に
// して、その押下の RELEASE 以降のイベントを復帰後の UI に流さない。
static std::atomic<bool> s_panel_off{false};
static void on_touch_press(lv_event_t* e) {
  if (s_panel_off.load(std::memory_order_relaxed)) {
    // 消灯中は間引きに関係なく必ず Wake を出す。
    s_last_wake_emit = lv_tick_get();
    emit(watch::ActionType::Wake);
    // この押下の残りを捨てる。LVGL 9.5 の lv_indev.c:
    // wait_until_release=1 で indev_proc_press が冒頭 return (press 以降の
    // scroll/gesture 蓄積も起きない) し、release 時には act_obj への
    // PRESS_LOST だけを送って act_obj=NULL にするため RELEASED/CLICKED は
    // 一切発行されない。これが無いと、起こしタップと同じ指の RELEASE が
    // 復帰後の画面上のボタンを誤タップする。
    lv_indev_t* indev = static_cast<lv_indev_t*>(lv_event_get_target(e));
    lv_indev_wait_release(indev ? indev : s_indev);
    return;
  }
  if (s_last_wake_emit != 0 && lv_tick_elaps(s_last_wake_emit) < 400) return;
  s_last_wake_emit = lv_tick_get();
  emit(watch::ActionType::Wake);
}

#if CONFIG_WATCH_DIAG_CORNERS
// 角曲率計測オーバーレイ: 四隅の角点を中心に半径8px刻みの同心弧を
// 描く。物理的に切れる位置で欠けるので、欠け始めるリング位置から
// 実効の角 R / safe-area が実機写真で読み取れる。
// 色順(内→外): 赤0 橙8 黄16 緑24 水32 青40 紫48
// さらに全面フレーム: 辺ごと別色 (上=白 右=赤 下=緑 左=青) の3px枠
// +32px刻みの内向き目盛 (12px)。パネル窓(gap)のズレ方向と量が読める。
void draw_corner_probes() {
  constexpr int SZ = 64;
  constexpr int STEPS = 7;
  const lv_color_t cols[STEPS] = {
      LV_COLOR_MAKE(255, 64, 64),   LV_COLOR_MAKE(255, 160, 32),
      LV_COLOR_MAKE(255, 232, 48),  LV_COLOR_MAKE(96, 255, 96),
      LV_COLOR_MAKE(32, 224, 255),  LV_COLOR_MAKE(96, 128, 255),
      LV_COLOR_MAKE(224, 96, 255),
  };
  // 全面フレーム (描画領域の物理的位置が一目で分かる目盛付き枠)
  {
    const int W = BSP_LCD_H_RES, H = BSP_LCD_V_RES;
    uint8_t* fbuf = static_cast<uint8_t*>(
        heap_caps_malloc(W * H * 4, MALLOC_CAP_SPIRAM));
    if (fbuf) {
      memset(fbuf, 0, (size_t)W * H * 4);
      lv_obj_t* fc = lv_canvas_create(lv_layer_top());
      lv_canvas_set_buffer(fc, fbuf, W, H, LV_COLOR_FORMAT_ARGB8888);
      // edge: 0=上(白) 1=右(赤) 2=下(緑) 3=左(青)
      const lv_color_t ecols[4] = {
          LV_COLOR_MAKE(255, 255, 255), LV_COLOR_MAKE(255, 32, 32),
          LV_COLOR_MAKE(64, 255, 64),   LV_COLOR_MAKE(64, 64, 255),
      };
      for (int e = 0; e < 4; e++) {
        const bool horz = (e == 0 || e == 2);
        const int len = horz ? W : H;
        for (int i = 0; i < len; i++) {
          for (int t = 0; t < 3; t++) {  // 枠の太さ 3px
            int x, y;
            if (e == 0)      { x = i; y = t; }
            else if (e == 1) { x = W - 1 - t; y = i; }
            else if (e == 2) { x = i; y = H - 1 - t; }
            else             { x = t; y = i; }
            lv_canvas_set_px(fc, x, y, ecols[e], LV_OPA_COVER);
          }
          // 目盛: 32px 刻みで 12px 内側へ (辺の色と同色)
          if (i % 32 == 0) {
            for (int t = 3; t < 12; t++) {
              int x, y;
              if (e == 0)      { x = i; y = t; }
              else if (e == 1) { x = W - 1 - t; y = i; }
              else if (e == 2) { x = i; y = H - 1 - t; }
              else             { x = t; y = i; }
              lv_canvas_set_px(fc, x, y, ecols[e], LV_OPA_COVER);
            }
          }
        }
      }
      lv_obj_set_pos(fc, 0, 0);
      lv_obj_remove_flag(fc, LV_OBJ_FLAG_CLICKABLE);
    }
  }
  // corner: 0=TL 1=TR 2=BL 3=BR
  for (int c = 0; c < 4; c++) {
    uint8_t* buf = static_cast<uint8_t*>(
        heap_caps_malloc(SZ * SZ * 4, MALLOC_CAP_SPIRAM));
    if (!buf) return;
    memset(buf, 0, SZ * SZ * 4);
    lv_obj_t* cv = lv_canvas_create(lv_layer_top());
    lv_canvas_set_buffer(cv, buf, SZ, SZ, LV_COLOR_FORMAT_ARGB8888);
    // キャンバス内の円心 (画面角点がローカル原点に写るよう配置)
    const float cx = (c == 1 || c == 3) ? SZ : 0.0f;
    const float cy = (c >= 2) ? SZ : 0.0f;
    for (int y = 0; y < SZ; y++) {
      for (int x = 0; x < SZ; x++) {
        const float d = std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy));
        for (int r = 0; r < STEPS; r++) {
          if (d >= r * 8 - 1.5f && d <= r * 8 + 1.5f) {
            lv_canvas_set_px(cv, x, y, cols[r], LV_OPA_COVER);
            break;
          }
        }
      }
    }
    lv_obj_set_pos(cv, (c == 1 || c == 3) ? BSP_LCD_H_RES - SZ : 0,
                   (c >= 2) ? BSP_LCD_V_RES - SZ : 0);
    // クリック対象にしない (入力を透過させる)。
    lv_obj_remove_flag(cv, LV_OBJ_FLAG_CLICKABLE);
  }
}
#endif

lv_display_t* init_display() {
  // LVGL タスク (lvgl_port) — core1 相当、prio 4。
  lvgl_port_cfg_t pcfg = ESP_LVGL_PORT_INIT_CONFIG();
  // SPI ISR と同じ core1 に固定する (vendor BSP の buscfg.isr_cpu_id)。
  // 別コアだと spi_bus_lock のマルチコア競合 (IDFGH-17166) で
  // flush 中に spi_device_acquire_bus が永久待ちになる。BLE は core0。
  pcfg.task_affinity = 1;
  // theme_apply (littlefs/esp_flash_read 使用のため内蔵 DRAM 必須) や
  // 装飾付き文字盤の描画余裕として既定 7168B から 9216B に拡張。
  pcfg.task_stack = 9216;
  ESP_ERROR_CHECK(lvgl_port_init(&pcfg));
  ESP_LOGI(TAG, "lvgl port ready");

  // QSPI パネル (BSP が pin/初期化コマンドを持つ)。
  // panel/io ハンドルはこちらが保持 → disp_on_off が使える。
  // チャンク最大サイズに絞って SPI DMA descriptor の内蔵 RAM を節約する。
  bsp_display_config_t dcfg = {
      .max_transfer_sz = kBounceBytes,
  };
  ESP_ERROR_CHECK(bsp_display_new(&dcfg, &s_panel, &s_io));

  // バンドキュー + バウンスバッファ 2 枚 (16B 整列) + 転送完了通知プリミティブ。
  s_bands = xQueueCreate(4, sizeof(BandJob));
  s_chunk_free = xSemaphoreCreateCounting(2, 2);
  s_band_done = xSemaphoreCreateBinary();
  s_call_lock = xSemaphoreCreateMutex();
  s_call_done = xSemaphoreCreateBinary();
  ESP_RETURN_ON_FALSE(s_bands && s_chunk_free && s_band_done && s_call_lock &&
                          s_call_done,
                      nullptr, TAG, "band sync init failed");
  for (int i = 0; i < 2; i++) {
    s_bounce[i] = static_cast<uint8_t*>(heap_caps_aligned_alloc(
        16, kBounceBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    ESP_RETURN_ON_FALSE(s_bounce[i] != nullptr, nullptr, TAG,
                        "bounce buf alloc failed");
  }
  ESP_RETURN_ON_FALSE(
      xTaskCreatePinnedToCore(lcd_send_task, "lcd_send", 3584, nullptr,
                              pcfg.task_priority + 1, nullptr, 1) == pdPASS,
      nullptr, TAG, "lcd_send task failed");

  lvgl_port_display_cfg_t disp_cfg = {
      .io_handle = s_io,
      .panel_handle = s_panel,
      .control_handle = nullptr,
      // 描画バッファは 126 行 × 2 枚を PSRAM に確保 (描画と QSPI DMA を重ね、
      // かつ旧全面 1 枚比で PSRAM を ~205KB 節約。確保失敗時は 84/64 行へ縮小)。
      .buffer_size = BSP_LCD_H_RES * 126,
      .double_buffer = true,
      .trans_size = 0,
      .hres = BSP_LCD_H_RES,
      .vres = BSP_LCD_V_RES,
      .monochrome = false,
      .rotation =
          {
              .swap_xy = false,
              .mirror_x = false,
              .mirror_y = false,
          },
      .rounder_cb = nullptr,
      .color_format = LV_COLOR_FORMAT_RGB565,
      .flags =
          {
              .buff_dma = false,
              .buff_spiram = true,
              .sw_rotate = false,
              // バイトスワップはバウンスコピー時に自分でやるので
              // port 側は OFF (ここを true にすると二重スワップ)。
              .swap_bytes = false,
          },
  };
  // QSPI パネルなので非RGB経路の add_disp を使う (add_disp_rgb は
  // パラレルRGB前提で flush_ready の扱いが違う)。
  // add_disp から flush cb / 転送完了 ISR の差し替えまで lvgl_port_lock
  // (再帰 mutex) を保持する。間に LVGL タスクの再描画が挟まると
  // port 既定の flush cb が PSRAM バッファを直接 DMA に渡して
  // queue_trans が失敗し、flush_ready も来ず LVGL が空転する
  // (実機で taskLVGL wdt を確認)。
  static constexpr uint32_t kTryRows[] = {126, 84, 64};
  if (lvgl_port_lock(-1)) {
    for (uint32_t rows : kTryRows) {
      disp_cfg.buffer_size = BSP_LCD_H_RES * rows;
      s_disp = lvgl_port_add_disp(&disp_cfg);
      if (s_disp) {
        ESP_LOGI(TAG, "draw buf %lux%lu x2 PSRAM",
                 (unsigned long)BSP_LCD_H_RES, (unsigned long)rows);
        break;
      }
      ESP_LOGW(TAG, "add_disp %lu rows x2 failed; retry", (unsigned long)rows);
    }
    if (s_disp) {
      // port が張った flush cb / 転送完了 ISR / 待機 cb を差し替える。
      lv_display_set_flush_cb(s_disp, band_flush_cb);
      lv_display_set_flush_wait_cb(s_disp, band_wait_cb);
      const esp_lcd_panel_io_callbacks_t cbs = {
          .on_color_trans_done = on_chunk_sent,
      };
      esp_lcd_panel_io_register_event_callbacks(s_io, &cbs, nullptr);
    }
    lvgl_port_unlock();
  }
  ESP_RETURN_ON_FALSE(s_disp != nullptr, nullptr, TAG, "add_disp failed");

  // T24: assets の日本語フォント (binfont) をここで読んでおく。
  // theme_apply のアリーナ確保より先に PSRAM を抑えるのと、
  // LVGL ヒープへの複製が終わってから画面構築に入るため。
  if (lvgl_port_lock(-1)) {
    ui::port::builtin_font_load(20);
    ui::port::builtin_font_load(26);
    ui::port::builtin_font_load(32);
    lvgl_port_unlock();
  }

    // 偶数丸め (BSP と同じイベント)。
  lv_display_add_event_cb(s_disp, rounder_event_cb, LV_EVENT_INVALIDATE_AREA,
                          nullptr);

  // 画面遷移ごとの初回 render 計測 (perf_mark_screen が名前を積む)。
  lv_display_add_event_cb(s_disp, perf_render_cb, LV_EVENT_RENDER_START,
                          nullptr);
  lv_display_add_event_cb(s_disp, perf_render_cb, LV_EVENT_RENDER_READY,
                          nullptr);

  // タッチ (CST816S 想定 — BSP が i2c 初期化済み)。
  ESP_ERROR_CHECK(bsp_touch_new(nullptr, &s_tp));
  const lvgl_port_touch_cfg_t tcfg = {
      .disp = s_disp,
      .handle = s_tp,
  };
  s_indev = lvgl_port_add_touch(&tcfg);
  ESP_RETURN_ON_FALSE(s_indev != nullptr, nullptr, TAG, "add_touch failed");

  // TP INT(GPIO38) が配線されていると lvgl_port は indev を
  // LV_INDEV_MODE_EVENT にする (read_timer 停止・割り込み時のみ読取)。
  // このモードだと低速ドラッグ時の読取が INT の発火間隔まかせになり
  // スクロールが 1fps 紙芝居化する (実機で確認) ので TIMER モード
  // (33ms ポーリング) に戻す。INT→portタスク即時 read の経路は残る。
  // 消灯中のタップ起こし: widget に当たらない場所でも indev の
  // LV_EVENT_PRESSED は立つので、そこから Wake Action を emit する。
  // (CLICKED 経由だけだと行タップでしか起きず「たまにしか起きない」)
  if (lvgl_port_lock(-1)) {
    lv_indev_set_mode(s_indev, LV_INDEV_MODE_TIMER);
    lv_indev_add_event_cb(s_indev, on_touch_press, LV_EVENT_PRESSED, nullptr);
#if CONFIG_WATCH_DIAG_CORNERS
    draw_corner_probes();
#endif
    lvgl_port_unlock();
  }

  // バックライト (DCS 0x51 書き込み。BSP の statics が埋まるので以後使える)。
  lcd_send_run([](void*) { bsp_display_brightness_init(); }, nullptr);
  return s_disp;
}

}  // namespace ui

namespace ui::port {

// パネル表示の ON/OFF — ScreenOff 適用層から呼ぶ。
// waveshare の sh8601 ドライバは disp_sleep (SLPIN/SLPOUT, 0x10/0x11) を
// 実装していないので io_tx_param で自前送信する。
// T-Watch の手順と同じ考え方で ScreenOff は DISPOFF + SLPIN とし、
// ALDO2 の切断は長時間 OFF/deep sleep 側 (board::pmic::panel_power) が担う。
void display_power(bool on) {
  // on_touch_press が「起こしタップか」を判定するためのフラグ。
  // display_power(false) → true の間にある PRESSED だけが対象になる。
  s_panel_off.store(!on, std::memory_order_relaxed);
  if (!s_panel) return;
  lcd_send_run(
      [](void* arg) {
        const bool on_v = *static_cast<const bool*>(arg);
        if (on_v) {
          esp_lcd_panel_io_tx_param(s_io, 0x11, nullptr, 0);  // SLPOUT
          esp_lcd_panel_disp_on_off(s_panel, true);           // DISPON
        } else {
          esp_lcd_panel_io_tx_param(s_io, 0x10, nullptr, 0);  // SLPIN
          esp_lcd_panel_disp_on_off(s_panel, false);          // DISPOFF
        }
      },
      &on);
}

void brightness_apply(int percent) {
  // bsp_display_brightness_set は BSP 内部の panel/io statics を使う。
  // bsp_display_new がそれらを埋めるのでここから呼んで良い。
  lcd_send_run(
      [](void* arg) {
        bsp_display_brightness_set(*static_cast<const int*>(arg));
      },
      &percent);
}

}  // namespace ui::port
