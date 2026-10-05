// lv_conf.h — sim (Linux) 用の最小 LVGL 設定。
// firmware 側は Kconfig (sdkconfig.defaults) で同等の値を与えている。
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16
#define LV_USE_OS 0  // LV_OS_NONE: lv_timer_handler を手動で回す

#define LV_USE_STDLIB_MALLOC    LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING    LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF   LV_STDLIB_CLIB

#define LV_DEF_REFR_PERIOD 33
#define LV_DPI_DEF 240

// 生成フォントは圧縮ビットマップなので必須 (sdkconfig.defaults も同じ)。
#define LV_USE_FONT_COMPRESSED 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_48 1
// 生成フォントを既定に (LV_FONT_CUSTOM_DECLARE は lv_font_t が見える場所で展開される)。
#define LV_FONT_CUSTOM_DECLARE LV_FONT_DECLARE(font_jp_20)
#define LV_FONT_DEFAULT &font_jp_20

#define LV_USE_LABEL 1
#define LV_USE_BUTTON 1
#define LV_USE_SLIDER 1
#define LV_USE_IMAGE 1
#define LV_USE_LINE 1
#define LV_USE_ARC 1
#define LV_USE_FLEX 1
#define LV_USE_GRID 1

#define LV_USE_LOG 0
#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1
#define LV_USE_ASSERT_STYLE 0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ 0
#define LV_USE_ASSERT_HANDLER 0

#define LV_USE_DEMO_WIDGETS 0
#define LV_USE_DEMO_KEYPAD_AND_ENCODER 0
#define LV_USE_DEMO_BENCHMARK 0
#define LV_USE_DEMO_STRESS 0
#define LV_USE_DEMO_MUSIC 0
#define LV_USE_SYSMON 0
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR 0

#endif  // LV_CONF_H
