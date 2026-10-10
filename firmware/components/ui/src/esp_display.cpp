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
// (BSP は add_disp_rgb を使うが QSPI では flush_ready が早すぎる
//  ため非RGB経路の add_disp を使う — 下のコメント参照)
#include "ui/port.hpp"
#include "ui/ui.hpp"

#include "esp_err.h"  // bsp/display.h が esp_err_t を自分では include しない
#include "bsp/display.h"
#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include "bsp/touch.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_types.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"

static const char* TAG = "ui.display";

static esp_lcd_panel_io_handle_t s_io = nullptr;
static esp_lcd_panel_handle_t s_panel = nullptr;
static esp_lcd_touch_handle_t s_tp = nullptr;
static lv_display_t* s_disp = nullptr;
static lv_indev_t* s_indev = nullptr;

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

namespace ui {

lv_display_t* init_display() {
  // LVGL タスク (lvgl_port) — core1 相当、prio 4。
  lvgl_port_cfg_t pcfg = ESP_LVGL_PORT_INIT_CONFIG();
  ESP_ERROR_CHECK(lvgl_port_init(&pcfg));
  ESP_LOGI(TAG, "lvgl port ready");

  // QSPI パネル (BSP が pin/初期化コマンドを持つ)。
  // panel/io ハンドルはこちらが保持 → disp_on_off が使える。
  bsp_display_config_t dcfg = {
      .max_transfer_sz = BSP_LCD_H_RES * BSP_LCD_V_RES *
                         BSP_LCD_BITS_PER_PIXEL / 8,
  };
  ESP_ERROR_CHECK(bsp_display_new(&dcfg, &s_panel, &s_io));

  lvgl_port_display_cfg_t disp_cfg = {
      .io_handle = s_io,
      .panel_handle = s_panel,
      .control_handle = nullptr,
      .buffer_size = BSP_LCD_H_RES * LVGL_BUFFER_HEIGHT,
      .double_buffer = false,
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
              // QSPI の SPI DMA は PSRAM を読めない → 描画バッファは
              // 内蔵 SRAM に確保する。false だと MALLOC_CAP_DEFAULT で
              // PSRAM に着地し、bounce buffer 化も内部不足で失敗する
              // (実機で spi tx_color ESP_ERR_NO_MEM 連発を確認)。
              .buff_dma = true,
              .buff_spiram = false,
              .sw_rotate = true,
              .swap_bytes = true,
          },
  };
  // QSPI パネルなので非RGB経路の add_disp を使う。
  // add_disp_rgb (BSP がパラレルRGB用オプション流用で採用) は
  // flush 時に draw_bitmap をキューイングした直後 flush_ready を
  // 即返してしまい、DMA 転送中に LVGL がバッファを再描画して
  // 行単位で混ざる (実機で横縞・色化けを確認)。
  // 非RGB経路は on_color_trans_done 登録で実転送完了後にのみ
  // flush_ready になる。
  s_disp = lvgl_port_add_disp(&disp_cfg);
  ESP_RETURN_ON_FALSE(s_disp != nullptr, nullptr, TAG, "add_disp failed");

  // 偶数丸め (BSP と同じイベント)。
  lv_display_add_event_cb(s_disp, rounder_event_cb, LV_EVENT_INVALIDATE_AREA,
                          nullptr);

  // タッチ (CST816S 想定 — BSP が i2c 初期化済み)。
  ESP_ERROR_CHECK(bsp_touch_new(nullptr, &s_tp));
  const lvgl_port_touch_cfg_t tcfg = {
      .disp = s_disp,
      .handle = s_tp,
  };
  s_indev = lvgl_port_add_touch(&tcfg);
  ESP_RETURN_ON_FALSE(s_indev != nullptr, nullptr, TAG, "add_touch failed");

  // バックライト (DCS 0x51 書き込み。BSP の statics が埋まるので以後使える)。
  bsp_display_brightness_init();
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
  if (!s_panel) return;
  if (on) {
    esp_lcd_panel_io_tx_param(s_io, 0x11, nullptr, 0);  // SLPOUT
    esp_lcd_panel_disp_on_off(s_panel, true);           // DISPON
  } else {
    esp_lcd_panel_io_tx_param(s_io, 0x10, nullptr, 0);  // SLPIN
    esp_lcd_panel_disp_on_off(s_panel, false);          // DISPOFF
  }
}

void brightness_apply(int percent) {
  // bsp_display_brightness_set は BSP 内部の panel/io statics を使う。
  // bsp_display_new がそれらを埋めるのでここから呼んで良い。
  bsp_display_brightness_set(percent);
}

}  // namespace ui::port
