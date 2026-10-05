#include "ui_min/ui_min.hpp"

#include <time.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "bsp/esp-bsp.h"

// 最小 UI (docs/plan.md R 章 Phase 2 / F 章の黒背景方針)。
// フォントは英数字のみ (日本語フォントは plan.md I 章で後日)。

namespace ui_min {

static const char* TAG = "ui_min";

static constexpr uint32_t kToastShowMs = 1500;

static lv_display_t* s_disp = nullptr;
static lv_obj_t* s_time_label = nullptr;
static lv_obj_t* s_date_label = nullptr;
static lv_obj_t* s_batt_label = nullptr;
static lv_obj_t* s_toast_label = nullptr;
static lv_timer_t* s_timer = nullptr;

static int s_batt_percent = -1;
static bool s_batt_charging = false;
static int s_brightness = 80;   // 初期明るさ // TODO(hw): 屋外視認性を実機で確認
static bool s_screen_on = true;
static int64_t s_toast_hide_at_us = 0;
static void (*s_on_activity)(void) = nullptr;

static const char* const kWeekdays[] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat",
};

static void update_clock_locked()
{
    const time_t now = time(nullptr);
    struct tm tm_now;
    localtime_r(&now, &tm_now);

    char buf[48];
    snprintf(buf, sizeof(buf), "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
    lv_label_set_text(s_time_label, buf);

    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %s",
             tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
             kWeekdays[tm_now.tm_wday % 7]);
    lv_label_set_text(s_date_label, buf);
}

static void update_battery_locked()
{
    char buf[16];
    if (s_batt_percent < 0) {
        snprintf(buf, sizeof(buf), "--%%");
    } else {
        snprintf(buf, sizeof(buf), "%d%%%s", s_batt_percent,
                 s_batt_charging ? "+" : "");
    }
    lv_label_set_text(s_batt_label, buf);
}

static void tick(lv_timer_t*)
{
    update_clock_locked();

    if (s_toast_hide_at_us != 0 && esp_timer_get_time() > s_toast_hide_at_us) {
        lv_label_set_text(s_toast_label, "");
        s_toast_hide_at_us = 0;
    }
}

static void on_press(lv_event_t*)
{
    // タッチ = アクティビティ (画面復帰判定は app 側)
    if (s_on_activity) {
        s_on_activity();
    }
}

lv_display_t* init()
{
    s_disp = bsp_display_start();
    if (s_disp == nullptr) {
        ESP_LOGE(TAG, "bsp_display_start failed");
        return nullptr;
    }
    bsp_display_brightness_set(s_brightness);

    if (!bsp_display_lock(0)) {
        ESP_LOGE(TAG, "bsp_display_lock failed");
        return s_disp;
    }

    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    // 画面全体でタッチを拾ってアクティビティにする
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr, on_press, LV_EVENT_PRESSED, nullptr);

    // 時刻 HH:MM
    s_time_label = lv_label_create(scr);
    lv_obj_set_style_text_font(s_time_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_time_label, lv_color_white(), 0);
    lv_obj_align(s_time_label, LV_ALIGN_CENTER, 0, -60);

    // 日付
    s_date_label = lv_label_create(scr);
    lv_obj_set_style_text_font(s_date_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_date_label, lv_color_hex(0x9A9A9A), 0);
    lv_obj_align(s_date_label, LV_ALIGN_CENTER, 0, 0);

    // 電池% (右上)
    s_batt_label = lv_label_create(scr);
    lv_obj_set_style_text_font(s_batt_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_batt_label, lv_color_hex(0x9A9A9A), 0);
    lv_obj_align(s_batt_label, LV_ALIGN_TOP_RIGHT, -8, 8);

    // 画面下のトースト (ボタンイベント表示)
    s_toast_label = lv_label_create(scr);
    lv_obj_set_style_text_font(s_toast_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_toast_label, lv_color_hex(0xFFE27A), 0);
    lv_obj_align(s_toast_label, LV_ALIGN_BOTTOM_MID, 0, -12);

    update_clock_locked();
    update_battery_locked();

    s_timer = lv_timer_create(tick, 1000, nullptr);

    bsp_display_unlock();
    ESP_LOGI(TAG, "ui_min ready (%dx%d)", BSP_LCD_H_RES, BSP_LCD_V_RES);
    return s_disp;
}

void notify(const char* text)
{
    if (!bsp_display_lock(100)) {
        return;
    }
    if (s_toast_label != nullptr && text != nullptr) {
        lv_label_set_text(s_toast_label, text);
        s_toast_hide_at_us = esp_timer_get_time() + kToastShowMs * 1000;
    }
    bsp_display_unlock();
}

void set_battery(int percent, bool charging)
{
    s_batt_percent = percent;
    s_batt_charging = charging;
    if (!bsp_display_lock(100)) {
        return;
    }
    if (s_batt_label != nullptr) {
        update_battery_locked();
    }
    bsp_display_unlock();
}

void set_screen_on(bool on)
{
    if (on == s_screen_on) {
        return;
    }
    s_screen_on = on;
    if (on) {
        bsp_display_backlight_on();
        bsp_display_brightness_set(s_brightness);
    } else {
        bsp_display_backlight_off();  // = 明るさ0。DCS off は TODO(hw)
    }
}

bool is_screen_on()
{
    return s_screen_on;
}

void set_brightness(int percent)
{
    if (percent < 0 || percent > 100) {
        return;
    }
    s_brightness = percent;
    if (s_screen_on) {
        bsp_display_brightness_set(percent);
    }
}

int get_brightness()
{
    return s_brightness;
}

void set_on_activity(void (*cb)(void))
{
    s_on_activity = cb;
}

}  // namespace ui_min
