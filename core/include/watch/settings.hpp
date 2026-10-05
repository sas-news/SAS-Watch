// settings.hpp — protocol-v1.md の settings keys を型付きで保持し、
// KeyValueStore に保存/復元する。
#pragma once

#include <cstddef>
#include <cstdint>

#include "watch/platform.hpp"

namespace watch {

enum class SettingType : uint8_t { U32, I32, Str };

// 全設定。protocol-v1.md の keys + deep_sleep_after_s / tz_offset_min /
// button.pwr.double (コア側の拡張。phone が知らなくても害はない)。
struct Settings {
  uint32_t brightness = 50;          // 0-100
  uint32_t dim_after_s = 8;          // Active→Dim (plan.md L章)
  uint32_t screen_off_after_s = 12;  // Active→ScreenOff (8+4)
  uint32_t deep_sleep_after_s = 1800;
  int32_t tz_offset_min = 0;         // time.set で記憶
  char theme[32] = "standard";
  // ボタン割り当て (Action 名)。既定は plan.md G章。
  char button_boot_short[24] = "primary";
  char button_boot_long[24] = "nav.dev";
  char button_boot_double[24] = "memo.record";
  char button_pwr_short[24] = "back";
  char button_pwr_long[24] = "power_menu";
  char button_pwr_double[24] = "none";
};

struct SettingKey {
  const char* name;    // "brightness" など (protocol-v1.md 準拠)
  SettingType type;
  uint16_t offset;     // Settings 内のオフセット
  uint16_t max_len;    // Str のバッファ長 (NUL 含む)
};

const SettingKey* settings_keys();
size_t settings_key_count();
const SettingKey* settings_find(const char* name);

bool settings_get_u32(const Settings& s, const SettingKey& k, uint32_t* out);
bool settings_get_i32(const Settings& s, const SettingKey& k, int32_t* out);
const char* settings_get_str(const Settings& s, const SettingKey& k);

// 1キーだけ KV に書く (kv=nullptr で永続化なし)。範囲外・長すぎは false。
bool settings_set_u32(Settings& s, KeyValueStore* kv, const char* name, uint32_t v);
bool settings_set_i32(Settings& s, KeyValueStore* kv, const char* name, int32_t v);
bool settings_set_str(Settings& s, KeyValueStore* kv, const char* name,
                      const char* v);

// KV から全キーを復元。無いキーは既定値のまま。
void settings_load(Settings& s, KeyValueStore& kv);
// 全キーを KV に書く。
void settings_save_all(const Settings& s, KeyValueStore& kv);

}  // namespace watch
