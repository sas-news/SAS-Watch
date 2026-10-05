// REQ ディスパッチ。method 名 → ハンドラ。
// {"m":<method>,"p":<params>} → {"ok":true,"r":<result>} or
// {"ok":false,"e":<code>,"msg":<string>} (protocol-v1.md)。
#include "watch/protocol/dispatch.hpp"

#include <cstdint>
#include <cstring>

#include "watch/event.hpp"
#include "watch/protocol/frame.hpp"

namespace watch {
namespace proto {

namespace {

const char* err_code(DispatchError e) {
  switch (e) {
    case DispatchError::BadRequest:
      return "bad_request";
    case DispatchError::UnknownMethod:
      return "unknown_method";
    case DispatchError::UnsupportedProto:
      return "unsupported_proto";
    case DispatchError::Busy:
      return "busy";
    case DispatchError::NotFound:
      return "not_found";
    default:
      return "internal";
  }
}

void write_error(cbor::Writer* out, DispatchError e, const char* msg) {
  out->map(3)
      .text("ok")
      .bool_v(false)
      .text("e")
      .text(err_code(e))
      .text("msg")
      .text(msg ? msg : "");
}

// params の指定キーを int で取る。無ければ false。
bool param_int(const cbor::Value& params, const char* key, int64_t* out) {
  cbor::Value v;
  if (!cbor::map_find(params, key, &v)) return false;
  return cbor::as_int(v, out);
}

bool param_text(const cbor::Value& params, const char* key, const char** s,
                size_t* n) {
  cbor::Value v;
  if (!cbor::map_find(params, key, &v)) return false;
  return cbor::as_text(v, s, n);
}

// ---------- handlers (params → result map の中身を書く) ----------

DispatchError h_hello(const cbor::Value& params, Services& svc,
                      cbor::Writer* r) {
  int64_t proto = 0;
  if (!param_int(params, "proto", &proto)) return DispatchError::BadRequest;
  if (proto != kProtoVer) return DispatchError::UnsupportedProto;
  const char* fw = svc.fw_version ? svc.fw_version(svc.ctx) : "0.1.0";
  r->map(3)
      .text("proto")
      .uint_v(kProtoVer)
      .text("fw")
      .text(fw)
      .text("caps")
      .array(11)
      .text("timer")
      .text("stopwatch")
      .text("counter")
      .text("memo")
      .text("theme")
      .text("audio")
      .text("alarm")
      .text("notify")
      .text("media")
      .text("wifi")
      .text("ota");
  return DispatchError::Ok;
}

DispatchError h_time_set(const cbor::Value& params, Services& svc,
                         cbor::Writer* r) {
  if (!svc.clock) return DispatchError::Internal;
  int64_t epoch = 0;
  int64_t tz = 0;
  if (!param_int(params, "epoch", &epoch)) return DispatchError::BadRequest;
  if (!svc.clock->set_epoch_s(epoch)) return DispatchError::Internal;
  if (param_int(params, "tz_offset_min", &tz) && svc.settings) {
    settings_set_i32(*svc.settings, svc.kv, "tz_offset_min",
                     static_cast<int32_t>(tz));
  }
  r->map(0);
  return DispatchError::Ok;
}

DispatchError h_device_info(const cbor::Value&, Services& svc,
                            cbor::Writer* r) {
  const int battery = svc.battery_percent ? svc.battery_percent(svc.ctx) : -1;
  const bool charging = svc.is_charging ? svc.is_charging(svc.ctx) : false;
  const char* fw = svc.fw_version ? svc.fw_version(svc.ctx) : "0.1.0";
  const int64_t heap = svc.free_heap ? svc.free_heap(svc.ctx) : 0;
  const int64_t psram = svc.free_psram ? svc.free_psram(svc.ctx) : 0;
  r->map(5)
      .text("battery")
      .int_v(battery)
      .text("charging")
      .bool_v(charging)
      .text("fw")
      .text(fw)
      .text("free_heap")
      .int_v(heap)
      .text("free_psram")
      .int_v(psram);
  return DispatchError::Ok;
}

// settings の1キーを result map に書く。
void write_setting_value(const Settings& s, const SettingKey& k,
                         cbor::Writer* w) {
  w->text(k.name);
  switch (k.type) {
    case SettingType::U32: {
      uint32_t v = 0;
      settings_get_u32(s, k, &v);
      w->uint_v(v);
      break;
    }
    case SettingType::I32: {
      int32_t v = 0;
      settings_get_i32(s, k, &v);
      w->int_v(v);
      break;
    }
    case SettingType::Str:
      w->text(settings_get_str(s, k));
      break;
  }
}

DispatchError h_settings_get(const cbor::Value& params, Services& svc,
                             cbor::Writer* r) {
  if (!svc.settings) return DispatchError::Internal;
  cbor::Value keys_v;
  const bool has_keys = cbor::map_find(params, "keys", &keys_v);
  size_t want = 0;
  if (has_keys) {
    // 指定キーのうち有効なものを数える。
    size_t n = 0;
    if (cbor::type(keys_v) != cbor::Type::Array) {
      return DispatchError::BadRequest;
    }
    cbor::container_count(keys_v, &n);
    for (size_t i = 0; i < n; ++i) {
      cbor::Value kv;
      if (!cbor::array_at(keys_v, i, &kv)) return DispatchError::BadRequest;
      const char* s = nullptr;
      size_t sl = 0;
      if (!cbor::as_text(kv, &s, &sl)) return DispatchError::BadRequest;
      for (size_t k = 0; k < settings_key_count(); ++k) {
        const SettingKey& sk = settings_keys()[k];
        if (std::strlen(sk.name) == sl &&
            std::memcmp(sk.name, s, sl) == 0) {
          ++want;
          break;
        }
      }
    }
    r->map(want);
    for (size_t i = 0; i < n; ++i) {
      cbor::Value kv;
      if (!cbor::array_at(keys_v, i, &kv)) return DispatchError::BadRequest;
      const char* s = nullptr;
      size_t sl = 0;
      cbor::as_text(kv, &s, &sl);
      for (size_t k = 0; k < settings_key_count(); ++k) {
        const SettingKey& sk = settings_keys()[k];
        if (std::strlen(sk.name) == sl &&
            std::memcmp(sk.name, s, sl) == 0) {
          write_setting_value(*svc.settings, sk, r);
          break;
        }
      }
    }
  } else {
    r->map(settings_key_count());
    for (size_t k = 0; k < settings_key_count(); ++k) {
      write_setting_value(*svc.settings, settings_keys()[k], r);
    }
  }
  return DispatchError::Ok;
}

struct SetCtx {
  Services* svc;
  int applied;
  int failed;
};

bool set_one(const cbor::Value& k, const cbor::Value& v, void* ctx_) {
  SetCtx* c = static_cast<SetCtx*>(ctx_);
  const char* ks = nullptr;
  size_t kn = 0;
  if (!cbor::as_text(k, &ks, &kn)) {
    ++c->failed;
    return true;
  }
  char name[40];
  if (kn >= sizeof(name)) {
    ++c->failed;
    return true;
  }
  std::memcpy(name, ks, kn);
  name[kn] = '\0';
  const SettingKey* sk = settings_find(name);
  if (!sk) {
    ++c->failed;
    return true;  // 知らないキーは飛ばす
  }
  bool ok = false;
  switch (sk->type) {
    case SettingType::U32: {
      int64_t iv;
      if (cbor::as_int(v, &iv) && iv >= 0 && iv <= UINT32_MAX) {
        ok = settings_set_u32(*c->svc->settings, c->svc->kv, name,
                              static_cast<uint32_t>(iv));
      }
      break;
    }
    case SettingType::I32: {
      int64_t iv;
      if (cbor::as_int(v, &iv) && iv >= INT32_MIN && iv <= INT32_MAX) {
        ok = settings_set_i32(*c->svc->settings, c->svc->kv, name,
                              static_cast<int32_t>(iv));
      }
      break;
    }
    case SettingType::Str: {
      const char* s = nullptr;
      size_t n = 0;
      if (cbor::as_text(v, &s, &n)) {
        char buf[64];
        if (n < sizeof(buf)) {
          std::memcpy(buf, s, n);
          buf[n] = '\0';
          ok = settings_set_str(*c->svc->settings, c->svc->kv, name, buf);
        }
      }
      break;
    }
  }
  if (ok) {
    ++c->applied;
    if (c->svc->setting_changed) {
      c->svc->setting_changed(name, c->svc->ctx);
    }
  } else {
    ++c->failed;
  }
  return true;
}

DispatchError h_settings_set(const cbor::Value& params, Services& svc,
                             cbor::Writer* r) {
  if (!svc.settings) return DispatchError::Internal;
  if (cbor::type(params) != cbor::Type::Map) {
    return DispatchError::BadRequest;
  }
  SetCtx c{&svc, 0, 0};
  if (!cbor::map_foreach(params, set_one, &c)) {
    return DispatchError::BadRequest;
  }
  apply_settings_side_effects(*svc.settings, svc);
  r->map(0);
  return DispatchError::Ok;
}

DispatchError h_timer_start(const cbor::Value& params, Services& svc,
                            cbor::Writer* r) {
  if (!svc.timer_start) return DispatchError::Internal;
  int64_t sec = 0;
  if (!param_int(params, "seconds", &sec) || sec <= 0 || sec > UINT32_MAX) {
    return DispatchError::BadRequest;
  }
  if (!svc.timer_start(static_cast<uint32_t>(sec), svc.ctx)) {
    return DispatchError::Internal;
  }
  r->map(0);
  return DispatchError::Ok;
}

DispatchError h_timer_stop(const cbor::Value&, Services& svc,
                           cbor::Writer* r) {
  if (!svc.timer_stop) return DispatchError::Internal;
  if (!svc.timer_stop(svc.ctx)) return DispatchError::Internal;
  r->map(0);
  return DispatchError::Ok;
}

DispatchError h_memo_create(const cbor::Value& params, Services& svc,
                            cbor::Writer* r) {
  if (!svc.memo_create) return DispatchError::Internal;
  const char* text = nullptr;
  size_t n = 0;
  if (!param_text(params, "text", &text, &n) || n == 0) {
    return DispatchError::BadRequest;
  }
  const int32_t id = svc.memo_create(text, n, svc.ctx);
  if (id < 0) return DispatchError::BadRequest;
  r->map(1).text("id").int_v(id);
  return DispatchError::Ok;
}

DispatchError h_memo_list(const cbor::Value& params, Services& svc,
                          cbor::Writer* r) {
  if (!svc.memo_count || !svc.memo_entry) return DispatchError::Internal;
  int64_t i = 0, n = 16;
  param_int(params, "i", &i);
  param_int(params, "n", &n);
  if (i < 0 || n <= 0 || n > 16 || i > INT32_MAX) {
    return DispatchError::BadRequest;
  }
  const int32_t total = svc.memo_count(svc.ctx);
  const int32_t remain = total - static_cast<int32_t>(i);
  const size_t cnt = remain <= 0 ? 0
                                 : (remain < n ? static_cast<size_t>(remain)
                                               : static_cast<size_t>(n));
  r->map(2).text("total").int_v(total).text("memos").array(cnt);
  for (size_t k = 0; k < cnt; ++k) {
    if (!svc.memo_entry(static_cast<uint32_t>(i) + k, *r, svc.ctx)) {
      return DispatchError::Internal;
    }
  }
  return DispatchError::Ok;
}

DispatchError h_memo_get(const cbor::Value& params, Services& svc,
                         cbor::Writer* r) {
  if (!svc.memo_get) return DispatchError::Internal;
  int64_t id = -1;
  if (!param_int(params, "id", &id) || id < 0 || id > UINT32_MAX) {
    return DispatchError::BadRequest;
  }
  if (!svc.memo_get(static_cast<uint32_t>(id), *r, svc.ctx)) {
    return DispatchError::NotFound;
  }
  return DispatchError::Ok;
}

DispatchError h_memo_delete(const cbor::Value& params, Services& svc,
                            cbor::Writer* r) {
  if (!svc.memo_delete) return DispatchError::Internal;
  int64_t id = -1;
  if (!param_int(params, "id", &id) || id < 0 || id > UINT32_MAX) {
    return DispatchError::BadRequest;
  }
  const int32_t res = svc.memo_delete(static_cast<uint32_t>(id), svc.ctx);
  if (res == 0) return DispatchError::NotFound;
  if (res < 0) return DispatchError::Internal;
  r->map(0);
  return DispatchError::Ok;
}

DispatchError h_memo_audio_get(const cbor::Value& params, Services& svc,
                               cbor::Writer* r) {
  if (!svc.memo_audio_info || !svc.memo_audio_send) {
    return DispatchError::Internal;
  }
  int64_t id = -1;
  if (!param_int(params, "id", &id) || id < 0 || id > UINT32_MAX) {
    return DispatchError::BadRequest;
  }
  uint32_t size = 0;
  uint8_t sha[32] = {};
  if (!svc.memo_audio_info(static_cast<uint32_t>(id), &size, sha, svc.ctx)) {
    return DispatchError::NotFound;
  }
  r->map(3)
      .text("id").uint_v(static_cast<uint32_t>(id))
      .text("size").uint_v(size)
      .text("sha256").bytes(sha, sizeof(sha));
  if (!svc.memo_audio_send(static_cast<uint32_t>(id), svc.ctx)) {
    return DispatchError::Busy;
  }
  return DispatchError::Ok;
}

// text パラメタを固定バッファにコピーして NUL 終端させる。
size_t copy_text(char* dst, size_t cap, const char* s, size_t n) {
  if (n >= cap) n = cap - 1;
  std::memcpy(dst, s, n);
  dst[n] = '\0';
  return n;
}

DispatchError h_wifi_set(const cbor::Value& params, Services& svc,
                         cbor::Writer* r) {
  if (!svc.wifi_set) return DispatchError::Internal;
  const char *ssid = nullptr, *pass = nullptr;
  size_t sn = 0, pn = 0;
  if (!param_text(params, "ssid", &ssid, &sn) || sn == 0 || sn > 32) {
    return DispatchError::BadRequest;
  }
  if (!param_text(params, "pass", &pass, &pn)) {
    return DispatchError::BadRequest;
  }
  // WPA は 8-63 文字。空 (0) はオープン接続として許可する。
  if (pn > 0 && (pn < 8 || pn > 63)) return DispatchError::BadRequest;
  char s[33], p[64];
  copy_text(s, sizeof(s), ssid, sn);
  copy_text(p, sizeof(p), pass, pn);
  if (!svc.wifi_set(s, p, svc.ctx)) return DispatchError::Internal;
  r->map(0);
  return DispatchError::Ok;
}

DispatchError h_wifi_status(const cbor::Value&, Services& svc,
                            cbor::Writer* r) {
  char ssid[33] = {};
  const bool configured =
      svc.wifi_info && svc.wifi_info(ssid, sizeof(ssid), svc.ctx);
  r->map(2).text("configured").bool_v(configured).text("ssid").text(ssid);
  return DispatchError::Ok;
}

DispatchError h_ota_start(const cbor::Value& params, Services& svc,
                          cbor::Writer* r) {
  if (!svc.ota_start) return DispatchError::Internal;
  const char *url = nullptr, *version = nullptr;
  size_t un = 0, vn = 0;
  if (!param_text(params, "url", &url, &un) || un < 8 || un > 255) {
    return DispatchError::BadRequest;
  }
  cbor::Value sv;
  const uint8_t* sha = nullptr;
  size_t shn = 0;
  if (!cbor::map_find(params, "sha256", &sv) ||
      !cbor::as_bytes(sv, &sha, &shn) || shn != 32) {
    return DispatchError::BadRequest;
  }
  if (!param_text(params, "version", &version, &vn) || vn == 0 || vn > 23) {
    return DispatchError::BadRequest;
  }
  // http(s):// 以外は受け付けない。
  if (!((un >= 8 && std::memcmp(url, "https://", 8) == 0) ||
        (un >= 7 && std::memcmp(url, "http://", 7) == 0))) {
    return DispatchError::BadRequest;
  }
  char url_s[256], ver_s[24];
  uint8_t sha_s[32];
  copy_text(url_s, sizeof(url_s), url, un);
  copy_text(ver_s, sizeof(ver_s), version, vn);
  std::memcpy(sha_s, sha, sizeof(sha_s));
  const int rc = svc.ota_start(url_s, sha_s, ver_s, svc.ctx);
  if (rc > 0) return DispatchError::Busy;
  if (rc < 0) return DispatchError::Internal;
  r->map(0);
  return DispatchError::Ok;
}

DispatchError h_ota_status(const cbor::Value&, Services& svc,
                           cbor::Writer* r) {
  if (!svc.ota_status) return DispatchError::Internal;
  // 結果は {active,stage,pct,msg,version} の map。
  if (!svc.ota_status(*r, svc.ctx)) return DispatchError::Internal;
  return DispatchError::Ok;
}

DispatchError h_notify_post(const cbor::Value& params, Services& svc,
                            cbor::Writer* r) {
  const char *app = nullptr, *title = nullptr, *body = nullptr;
  size_t an = 0, tn = 0, bn = 0;
  if (!param_text(params, "app", &app, &an) ||
      !param_text(params, "title", &title, &tn) ||
      !param_text(params, "body", &body, &bn)) {
    return DispatchError::BadRequest;
  }
  if (svc.notify_posted) {
    // NUL 終端が必要なので固定バッファにコピー。
    char app_s[48], title_s[96], body_s[256];
    auto copy = [](char* dst, size_t cap, const char* s, size_t n) {
      if (n >= cap) n = cap - 1;
      std::memcpy(dst, s, n);
      dst[n] = '\0';
    };
    copy(app_s, sizeof(app_s), app, an);
    copy(title_s, sizeof(title_s), title, tn);
    copy(body_s, sizeof(body_s), body, bn);
    svc.notify_posted(app_s, title_s, body_s, svc.ctx);
  }
  if (svc.bus) svc.bus->publish({EventType::NotificationPosted, 0});
  r->map(0);
  return DispatchError::Ok;
}

// ---- alarm.* ----
// params: alarm.set {id?(0=新規),hour:0-23,min:0-59,dow?(0=毎日),on?}
//         alarm.list {} → {alarms:[{id,hour,min,dow,on}]}
//         alarm.delete {id} → {}
DispatchError h_alarm_list(const cbor::Value&, Services& svc,
                           cbor::Writer* r) {
  if (!svc.alarm_count || !svc.alarm_entry) return DispatchError::Internal;
  const int32_t total = svc.alarm_count(svc.ctx);
  if (total < 0) return DispatchError::Internal;
  r->map(1).text("alarms").array(static_cast<size_t>(total));
  for (int32_t i = 0; i < total; ++i) {
    if (!svc.alarm_entry(static_cast<uint32_t>(i), *r, svc.ctx)) {
      return DispatchError::Internal;
    }
  }
  return DispatchError::Ok;
}

DispatchError h_alarm_set(const cbor::Value& params, Services& svc,
                          cbor::Writer* r) {
  if (!svc.alarm_set) return DispatchError::Internal;
  int64_t id = 0, hour = 0, min = 0, dow = 0;
  bool on = true;
  param_int(params, "id", &id);  // 省略時 0 = 新規
  if (!param_int(params, "hour", &hour) || !param_int(params, "min", &min)) {
    return DispatchError::BadRequest;
  }
  param_int(params, "dow", &dow);
  cbor::Value pv;
  if (cbor::map_find(params, "on", &pv)) {
    if (!cbor::as_bool(pv, &on)) return DispatchError::BadRequest;
  }
  if (id < 0 || id > UINT32_MAX || hour < 0 || hour > 23 || min < 0 ||
      min > 59 || dow < 0 || dow > 0x7F) {
    return DispatchError::BadRequest;
  }
  const int32_t res =
      svc.alarm_set(static_cast<uint32_t>(id), static_cast<uint8_t>(hour),
                    static_cast<uint8_t>(min), static_cast<uint8_t>(dow), on,
                    svc.ctx);
  if (res == -1) return DispatchError::BadRequest;
  if (res == -2) return DispatchError::Busy;
  if (res == -3) return DispatchError::NotFound;
  if (res < 0) return DispatchError::Internal;
  r->map(1).text("id").int_v(res);
  return DispatchError::Ok;
}

DispatchError h_alarm_delete(const cbor::Value& params, Services& svc,
                             cbor::Writer* r) {
  if (!svc.alarm_delete) return DispatchError::Internal;
  int64_t id = -1;
  if (!param_int(params, "id", &id) || id < 0 || id > UINT32_MAX) {
    return DispatchError::BadRequest;
  }
  const int32_t res = svc.alarm_delete(static_cast<uint32_t>(id), svc.ctx);
  if (res == 0) return DispatchError::NotFound;
  if (res < 0) return DispatchError::Internal;
  r->map(0);
  return DispatchError::Ok;
}

DispatchError h_media_state(const cbor::Value& params, Services& svc,
                            cbor::Writer* r) {
  const char *title = nullptr, *artist = nullptr;
  size_t tn = 0, an = 0;
  bool playing = false;
  bool have_playing = false;
  if (!param_text(params, "title", &title, &tn) ||
      !param_text(params, "artist", &artist, &an)) {
    return DispatchError::BadRequest;
  }
  cbor::Value pv;
  if (cbor::map_find(params, "playing", &pv)) {
    if (!cbor::as_bool(pv, &playing)) return DispatchError::BadRequest;
    have_playing = true;
  }
  (void)have_playing;
  if (svc.media_state) {
    char title_s[96], artist_s[96];
    auto copy = [](char* dst, size_t cap, const char* s, size_t n) {
      if (n >= cap) n = cap - 1;
      std::memcpy(dst, s, n);
      dst[n] = '\0';
    };
    copy(title_s, sizeof(title_s), title, tn);
    copy(artist_s, sizeof(artist_s), artist, an);
    svc.media_state(title_s, artist_s, playing, svc.ctx);
  }
  r->map(0);
  return DispatchError::Ok;
}

struct Handler {
  const char* name;
  DispatchError (*fn)(const cbor::Value&, Services&, cbor::Writer*);
};

constexpr Handler kHandlers[] = {
    {"hello", h_hello},         {"time.set", h_time_set},
    {"device.info", h_device_info}, {"settings.get", h_settings_get},
    {"settings.set", h_settings_set}, {"timer.start", h_timer_start},
    {"timer.stop", h_timer_stop},     {"memo.create", h_memo_create},
    {"memo.list", h_memo_list},       {"memo.get", h_memo_get},
    {"memo.delete", h_memo_delete},   {"memo.audio.get", h_memo_audio_get},
    {"notify.post", h_notify_post},   {"media.state", h_media_state},
    {"alarm.list", h_alarm_list},    {"alarm.set", h_alarm_set},
    {"alarm.delete", h_alarm_delete},
    {"wifi.set", h_wifi_set},         {"wifi.status", h_wifi_status},
    {"ota.start", h_ota_start},       {"ota.status", h_ota_status},
};

}  // namespace

void apply_settings_side_effects(const Settings& s, Services& svc) {
  if (svc.power) {
    svc.power->configure(s.dim_after_s, s.screen_off_after_s,
                         s.deep_sleep_after_s);
  }
  if (svc.input) {
    svc.input->map(PhysicalButton::Boot, PressType::Short, s.button_boot_short);
    svc.input->map(PhysicalButton::Boot, PressType::Long, s.button_boot_long);
    svc.input->map(PhysicalButton::Boot, PressType::Double,
                   s.button_boot_double);
    svc.input->map(PhysicalButton::Pwr, PressType::Short, s.button_pwr_short);
    svc.input->map(PhysicalButton::Pwr, PressType::Long, s.button_pwr_long);
    svc.input->map(PhysicalButton::Pwr, PressType::Double, s.button_pwr_double);
  }
}

DispatchError dispatch_req(const uint8_t* req, size_t req_len, Services& svc,
                           cbor::Writer* out) {
  if (!req || !out || req_len == 0) {
    if (out) write_error(out, DispatchError::BadRequest, "empty request");
    return DispatchError::BadRequest;
  }
  cbor::Value top{req, req + req_len};
  if (cbor::type(top) != cbor::Type::Map) {
    write_error(out, DispatchError::BadRequest, "not a map");
    return DispatchError::BadRequest;
  }
  cbor::Value m_v;
  const char* method = nullptr;
  size_t method_len = 0;
  if (!cbor::map_find(top, "m", &m_v) ||
      !cbor::as_text(m_v, &method, &method_len)) {
    write_error(out, DispatchError::BadRequest, "missing method");
    return DispatchError::BadRequest;
  }
  cbor::Value params;
  if (!cbor::map_find(top, "p", &params)) {
    // p 無しは空 map として扱う: 空のバッファ上の map(0)。
    static const uint8_t kEmptyMap[] = {0xA0};
    params.p = kEmptyMap;
    params.end = kEmptyMap + 1;
  }
  for (size_t i = 0; i < sizeof(kHandlers) / sizeof(kHandlers[0]); ++i) {
    if (std::strlen(kHandlers[i].name) == method_len &&
        std::memcmp(kHandlers[i].name, method, method_len) == 0) {
      // result は {"ok":true,"r":...}。handler が r の中身 (map) を
      // 一時バッファに書いてから、できた CBOR をそのまま埋め込む。
      uint8_t rbuf[512];
      cbor::Writer rw(rbuf, sizeof(rbuf));
      const DispatchError e = kHandlers[i].fn(params, svc, &rw);
      if (e != DispatchError::Ok) {
        write_error(out, e, kHandlers[i].name);
        return e;
      }
      if (!rw.ok()) {
        write_error(out, DispatchError::Internal, "result too large");
        return DispatchError::Internal;
      }
      out->map(2).text("ok").bool_v(true).text("r").raw(rw.data(), rw.size());
      return DispatchError::Ok;
    }
  }
  write_error(out, DispatchError::UnknownMethod, "unknown method");
  return DispatchError::UnknownMethod;
}

bool encode_evt(cbor::Writer* w, const char* event_name,
                void (*write_d)(cbor::Writer&, void*), void* ctx) {
  if (!w || !event_name) return false;
  w->map(2).text("e").text(event_name).text("d");
  if (write_d) {
    write_d(*w, ctx);
  } else {
    w->map(0);
  }
  return w->ok();
}

}  // namespace proto
}  // namespace watch
