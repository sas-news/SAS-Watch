#include "watch/settings.hpp"

#include <cstddef>
#include <cstring>

namespace watch {

namespace {

#define WATCH_SSET_U32(name_, field_)                                  \
  {                                                                    \
    name_, nullptr, SettingType::U32, offsetof(Settings, field_),      \
        sizeof(uint32_t)                                               \
  }
#define WATCH_SSET_U32_S(name_, store_, field_)                        \
  {                                                                    \
    name_, store_, SettingType::U32, offsetof(Settings, field_),       \
        sizeof(uint32_t)                                               \
  }
#define WATCH_SSET_I32(name_, field_)                                  \
  {                                                                    \
    name_, nullptr, SettingType::I32, offsetof(Settings, field_),      \
        sizeof(int32_t)                                                \
  }
#define WATCH_SSET_I32_S(name_, store_, field_)                        \
  {                                                                    \
    name_, store_, SettingType::I32, offsetof(Settings, field_),       \
        sizeof(int32_t)                                                \
  }
#define WATCH_SSET_STR(name_, field_)                                  \
  {                                                                    \
    name_, nullptr, SettingType::Str, offsetof(Settings, field_),      \
        sizeof(((Settings*)nullptr)->field_)                           \
  }
#define WATCH_SSET_STR_S(name_, store_, field_)                        \
  {                                                                    \
    name_, store_, SettingType::Str, offsetof(Settings, field_),       \
        sizeof(((Settings*)nullptr)->field_)                           \
  }

// protocol-v1.md の settings keys + core 拡張 (deep_sleep_after_s,
// tz_offset_min, button.pwr.double, usb_keep_awake)。
// "set." + name が NVS の 15 文字上限を超えるキーだけ、store に短縮
// エイリアスを持つ (BLE のキー名は変えない)。これが無いと書き込みが
// ESP_ERR_NVS_KEY_TOO_LONG で静かに失敗する (tz_offset_min など)。
constexpr SettingKey kKeys[] = {
    WATCH_SSET_U32("brightness", brightness),
    WATCH_SSET_U32("dim_after_s", dim_after_s),
    WATCH_SSET_U32_S("screen_off_after_s", "scr_off_s", screen_off_after_s),
    WATCH_SSET_U32_S("deep_sleep_after_s", "deep_s", deep_sleep_after_s),
    WATCH_SSET_I32_S("tz_offset_min", "tz_off", tz_offset_min),
    WATCH_SSET_STR("theme", theme),
    WATCH_SSET_STR_S("button.boot.short", "bb.short", button_boot_short),
    WATCH_SSET_STR_S("button.boot.long", "bb.long", button_boot_long),
    WATCH_SSET_STR_S("button.boot.double", "bb.dbl", button_boot_double),
    WATCH_SSET_STR_S("button.pwr.short", "bp.short", button_pwr_short),
    WATCH_SSET_STR_S("button.pwr.long", "bp.long", button_pwr_long),
    WATCH_SSET_STR_S("button.pwr.double", "bp.dbl", button_pwr_double),
    WATCH_SSET_U32_S("audio.volume", "audio.vol", audio_volume),
    WATCH_SSET_U32("audio.click", audio_click),
    WATCH_SSET_U32_S("notify.vibrate", "notif.vib", notify_vibrate),
    WATCH_SSET_U32_S("raise_to_wake", "rtw", raise_to_wake),
    WATCH_SSET_U32("steps.goal", steps_goal),
    WATCH_SSET_STR("face", face),
    WATCH_SSET_STR("clock_font", clock_font),
    WATCH_SSET_STR("agent.q1", agent_q1),
    WATCH_SSET_STR("agent.q2", agent_q2),
    WATCH_SSET_STR("agent.q3", agent_q3),
    WATCH_SSET_U32_S("usb_keep_awake", "usb_awake", usb_keep_awake),
};

char* field_ptr(Settings& s, const SettingKey& k) {
  return reinterpret_cast<char*>(&s) + k.offset;
}
const char* field_ptr(const Settings& s, const SettingKey& k) {
  return reinterpret_cast<const char*>(&s) + k.offset;
}

bool persist_one(KeyValueStore* kv, const Settings& s, const SettingKey& k) {
  if (!kv) return true;
  char key[48];
  if (settings_kv_key(k, key, sizeof(key)) == 0) return false;
  const char* p = field_ptr(s, k);
  const size_t n = (k.type == SettingType::Str)
                       ? (std::strlen(p) + 1)
                       : (k.type == SettingType::I32 ? sizeof(int32_t)
                                                     : sizeof(uint32_t));
  return kv->set(key, p, n);
}

}  // namespace

const SettingKey* settings_keys() { return kKeys; }
size_t settings_key_count() { return sizeof(kKeys) / sizeof(kKeys[0]); }

const SettingKey* settings_find(const char* name) {
  if (!name) return nullptr;
  for (size_t i = 0; i < settings_key_count(); ++i) {
    if (std::strcmp(kKeys[i].name, name) == 0) return &kKeys[i];
  }
  return nullptr;
}

size_t settings_kv_key(const SettingKey& k, char* buf, size_t cap) {
  // ストレージ名は store エイリアス優先 (NVS の 15 文字制限対応)。
  const char* s = k.store ? k.store : k.name;
  const size_t need = 4 + std::strlen(s) + 1;
  if (need > cap) return 0;
  std::memcpy(buf, "set.", 4);
  std::strcpy(buf + 4, s);
  return need - 1;
}

bool settings_get_u32(const Settings& s, const SettingKey& k, uint32_t* out) {
  if (!out || k.type != SettingType::U32) return false;
  uint32_t v;
  std::memcpy(&v, field_ptr(s, k), sizeof(v));
  *out = v;
  return true;
}

bool settings_get_i32(const Settings& s, const SettingKey& k, int32_t* out) {
  if (!out || k.type != SettingType::I32) return false;
  int32_t v;
  std::memcpy(&v, field_ptr(s, k), sizeof(v));
  *out = v;
  return true;
}

const char* settings_get_str(const Settings& s, const SettingKey& k) {
  if (k.type != SettingType::Str) return nullptr;
  return field_ptr(s, k);
}

bool settings_set_u32(Settings& s, KeyValueStore* kv, const char* name,
                      uint32_t v) {
  const SettingKey* k = settings_find(name);
  if (!k || k->type != SettingType::U32) return false;
  std::memcpy(field_ptr(s, *k), &v, sizeof(v));
  return persist_one(kv, s, *k);
}

bool settings_set_i32(Settings& s, KeyValueStore* kv, const char* name,
                      int32_t v) {
  const SettingKey* k = settings_find(name);
  if (!k || k->type != SettingType::I32) return false;
  std::memcpy(field_ptr(s, *k), &v, sizeof(v));
  return persist_one(kv, s, *k);
}

bool settings_set_str(Settings& s, KeyValueStore* kv, const char* name,
                      const char* v) {
  const SettingKey* k = settings_find(name);
  if (!k || k->type != SettingType::Str || !v) return false;
  const size_t n = std::strlen(v);
  if (n >= k->max_len) return false;
  char* dst = field_ptr(s, *k);
  std::memcpy(dst, v, n + 1);
  return persist_one(kv, s, *k);
}

void settings_load(Settings& s, KeyValueStore& kv) {
  for (size_t i = 0; i < settings_key_count(); ++i) {
    const SettingKey& k = kKeys[i];
    char key[48];
    if (settings_kv_key(k, key, sizeof(key)) == 0) continue;
    const size_t have = kv.size(key);
    if (have == 0) continue;
    if (k.type == SettingType::Str) {
      if (have >= k.max_len) continue;  // 壊れた値は既定値のまま
      char* dst = field_ptr(s, k);
      size_t got = 0;
      if (kv.get(key, dst, k.max_len - 1, &got)) {
        dst[got < k.max_len ? got : k.max_len - 1] = '\0';
      }
    } else {
      const size_t want =
          k.type == SettingType::I32 ? sizeof(int32_t) : sizeof(uint32_t);
      if (have != want) continue;
      size_t got = 0;
      kv.get(key, field_ptr(s, k), want, &got);
    }
  }
}

void settings_save_all(const Settings& s, KeyValueStore& kv) {
  for (size_t i = 0; i < settings_key_count(); ++i) {
    persist_one(&kv, const_cast<Settings&>(s), kKeys[i]);
  }
}

}  // namespace watch
