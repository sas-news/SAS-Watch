// theme/manifest.cpp — manifest.cbor → ThemeManifest。
// 未知キーは読み飛ばす (将来互換)。既知キーで型が違えば kBadValue。
#include "watch/theme/manifest.hpp"

#include <cstring>

#include "watch/protocol/cbor.hpp"

namespace watch {
namespace {

struct ColorKey {
  const char* key;
};
constexpr ColorKey kColorKeys[19] = {
    {"bg"},      {"surface"}, {"surface2"}, {"primary"}, {"on_primary"},
    {"text"},    {"text_dim"}, {"accent"},   {"danger"},  {"ok"},
    {"accent2"}, {"accent3"},  {"accent4"},  {"accent5"},
    {"bubble_bg"}, {"bubble_text"},
    {"line"},    {"primary2"}, {"edge"},
};

constexpr const char* kMetricKeys[4] = {"radius_sm", "radius_lg", "space",
                                        "anim_ms"};
constexpr const char* kFontKeys[4] = {"font_body", "font_title", "font_digits",
                                      "font_digits_sm"};
constexpr const char* kImageKeys[kThemeImageSlots] = {
    "home_bg", "stand", "timer_done", "face_chara"};

constexpr const char* kBubbleKeys[5] = {"morning", "noon", "evening",
                                        "night", "steps"};

}  // namespace

// 画面キー表 (manifest.hpp の宣言と同じ順)。
const char* const kThemeScreenNames[kThemeScreenCount] = {
    "home",   "quick",   "more",          "timer", "stopwatch", "counter",
    "steps",  "memo",    "settings",      "ota",   "powermenu", "alarm",
    "notifications", "media", "agent",    "alert",
};

// face_layout の要素名表 (ThemeFaceElem と同じ順)。
const char* const kThemeFaceElemNames[kThemeFaceElemCount] = {
    "time", "date", "steps", "battery", "notify", "bubble", "chara",
};

// skin のパーツ名表 (ThemeSkinPartId と同じ順)。
const char* const kThemeSkinPartNames[kThemeSkinPartCount] = {
    "card",          "list_group",   "row",           "divider",
    "button_primary", "button_secondary", "button_danger", "back_pill",
    "header_bar",
    "switch_track",  "switch_knob",
    "slider_track",  "slider_fill",  "slider_knob",
    "icon_tile",     "toast",        "bubble",        "caption_line",
};

const char* const kThemeSkinStateNames[kThemeSkinStateCount] = {
    "pressed", "checked", "disabled",
};

// 画面名 → index (不明なら -1)。"*" は kThemeScreenWildcard。
int theme_screen_index(const char* name) {
  if (!name) return -1;
  if (name[0] == '*' && name[1] == '\0') return kThemeScreenWildcard;
  for (int i = 0; i < kThemeScreenCount; ++i) {
    if (std::strcmp(name, kThemeScreenNames[i]) == 0) return i;
  }
  return -1;
}

namespace {

// cbor text → theme_screen_index
int screen_index(const cbor::Value& k) {
  const char* p = nullptr;
  size_t n = 0;
  if (!cbor::as_text(k, &p, &n) || n >= 24) return -1;
  char buf[24];
  std::memcpy(buf, p, n);
  buf[n] = '\0';
  return theme_screen_index(buf);
}

bool key_eq(const cbor::Value& k, const char* s) {
  const char* p = nullptr;
  size_t n = 0;
  if (!cbor::as_text(k, &p, &n)) return false;
  return n == std::strlen(s) && std::memcmp(p, s, n) == 0;
}

// "0xRRGGBB" (8文字固定) → 24bit 値。
bool parse_color(const cbor::Value& v, uint32_t* out) {
  const char* p = nullptr;
  size_t n = 0;
  if (!cbor::as_text(v, &p, &n) || n != 8 || p[0] != '0' || p[1] != 'x') {
    return false;
  }
  uint32_t c = 0;
  for (int i = 2; i < 8; ++i) {
    const char ch = p[i];
    uint32_t d;
    if (ch >= '0' && ch <= '9') {
      d = static_cast<uint32_t>(ch - '0');
    } else if (ch >= 'a' && ch <= 'f') {
      d = static_cast<uint32_t>(ch - 'a' + 10);
    } else if (ch >= 'A' && ch <= 'F') {
      d = static_cast<uint32_t>(ch - 'A' + 10);
    } else {
      return false;
    }
    c = (c << 4) | d;
  }
  *out = c;
  return true;
}

// 画像ファイル名: [a-z0-9._-] + 末尾 ".bin" or ".png" (theme-format.md)。
bool file_name_ok(const char* s, size_t n) {
  if (n == 0 || n >= kThemeImageNameMax) return false;
  const bool ext = (n >= 4 && (std::memcmp(s + n - 4, ".bin", 4) == 0 ||
                               std::memcmp(s + n - 4, ".png", 4) == 0));
  if (!ext) return false;
  for (size_t i = 0; i < n; ++i) {
    const char c = s[i];
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '.' || c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
}

struct Ctx {
  ThemeManifest* out;
  ThemeManifestError err = ThemeManifestError::kOk;
  int scratch_i = 0;                 // 入れ子 map で使う index 受け渡し
  ThemeScreenSkin scratch_skin{};    // "*" 用の一時値
};

bool tokens_cb(const cbor::Value& k, const cbor::Value& v, void* c_) {
  Ctx* c = static_cast<Ctx*>(c_);
  ThemeManifest* o = c->out;
  for (int i = 0; i < 19; ++i) {
    if (key_eq(k, kColorKeys[i].key)) {
      if (!parse_color(v, &o->color[i])) {
        c->err = ThemeManifestError::kBadValue;
        return false;
      }
      o->color_set |= static_cast<uint32_t>(1u << i);
      return true;
    }
  }
  for (int i = 0; i < 4; ++i) {
    if (key_eq(k, kMetricKeys[i])) {
      uint64_t u = 0;
      if (!cbor::as_uint(v, &u)) {
        c->err = ThemeManifestError::kBadValue;
        return false;
      }
      if (i < 3 && u > 255) {
        c->err = ThemeManifestError::kBadValue;
        return false;
      }
      if (i == 3 && u > 2000) {
        c->err = ThemeManifestError::kBadValue;
        return false;
      }
      switch (i) {
        case 0: o->radius_sm = static_cast<uint8_t>(u); break;
        case 1: o->radius_lg = static_cast<uint8_t>(u); break;
        case 2: o->space = static_cast<uint8_t>(u); break;
        case 3: o->anim_ms = static_cast<uint16_t>(u); break;
      }
      o->metric_set |= static_cast<uint8_t>(1u << i);
      return true;
    }
    if (key_eq(k, kFontKeys[i])) {
      uint64_t u = 0;
      if (!cbor::as_uint(v, &u) || u == 0 || u > 255) {
        c->err = ThemeManifestError::kBadValue;
        return false;
      }
      o->font_px[i] = static_cast<uint8_t>(u);
      o->font_set |= static_cast<uint8_t>(1u << i);
      return true;
    }
  }
  return true;  // 未知キーは無視
}

bool images_cb(const cbor::Value& k, const cbor::Value& v, void* c_) {
  Ctx* c = static_cast<Ctx*>(c_);
  ThemeManifest* o = c->out;
  for (int i = 0; i < kThemeImageSlots; ++i) {
    if (!key_eq(k, kImageKeys[i])) continue;
    const char* p = nullptr;
    size_t n = 0;
    if (!cbor::as_text(v, &p, &n) || !file_name_ok(p, n)) {
      c->err = ThemeManifestError::kBadImageName;
      return false;
    }
    std::memcpy(o->image[i], p, n);
    o->image[i][n] = '\0';
    o->image_set |= static_cast<uint8_t>(1u << i);
    return true;
  }
  return true;  // 未知キーは無視
}

bool bubble_cb(const cbor::Value& k, const cbor::Value& v, void* c_) {
  Ctx* c = static_cast<Ctx*>(c_);
  ThemeManifest* o = c->out;
  for (int i = 0; i < 5; ++i) {
    if (!key_eq(k, kBubbleKeys[i])) continue;
    const char* p = nullptr;
    size_t n = 0;
    if (!cbor::as_text(v, &p, &n) || n >= sizeof(o->bubble[0])) {
      c->err = ThemeManifestError::kBadValue;
      return false;
    }
    std::memcpy(o->bubble[i], p, n);
    o->bubble[i][n] = '\0';
    o->bubble_set |= static_cast<uint8_t>(1u << i);
    return true;
  }
  return true;  // 未知キーは無視
}

// ---- v2 ----

// フォントファイルは .bin のみ (PNG 不可)。
bool font_name_ok(const char* s, size_t n) {
  return n >= 4 && std::memcmp(s + n - 4, ".bin", 4) == 0 &&
         file_name_ok(s, n);
}

// アプリ id: [a-z0-9_-]{1,15} (アプリ一覧行の識別子)。
bool app_id_ok(const char* s, size_t n) {
  if (n == 0 || n >= kThemeAppIdMax) return false;
  for (size_t i = 0; i < n; ++i) {
    const char c = s[i];
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
}

// uint を取り、範囲外なら kBadValue。
bool take_uint(Ctx* c, const cbor::Value& v, uint64_t max, uint64_t* out) {
  uint64_t u = 0;
  if (!cbor::as_uint(v, &u) || u > max) {
    c->err = ThemeManifestError::kBadValue;
    return false;
  }
  *out = u;
  return true;
}

// screens.<screen> = {"bg": "file", "scrim": 0-255}
bool screens_map_cb(const cbor::Value& k, const cbor::Value& v, void* c_);

bool screen_skin_cb(const cbor::Value& k, const cbor::Value& v, void* c_) {
  Ctx* c = static_cast<Ctx*>(c_);
  ThemeManifest* o = c->out;
  const int idx = c->scratch_i;
  ThemeScreenSkin* s =
      (idx == kThemeScreenWildcard) ? &c->scratch_skin : &o->screens[idx];
  if (key_eq(k, "bg")) {
    const char* p = nullptr;
    size_t n = 0;
    if (!cbor::as_text(v, &p, &n) || !file_name_ok(p, n)) {
      c->err = ThemeManifestError::kBadImageName;
      return false;
    }
    std::memcpy(s->bg, p, n);
    s->bg[n] = '\0';
    return true;
  }
  if (key_eq(k, "scrim")) {
    uint64_t u = 0;
    if (!take_uint(c, v, 255, &u)) return false;
    s->scrim = static_cast<uint8_t>(u);
    return true;
  }
  return true;
}

bool screens_map_cb(const cbor::Value& k, const cbor::Value& v, void* c_) {
  Ctx* c = static_cast<Ctx*>(c_);
  ThemeManifest* o = c->out;
  const int idx = screen_index(k);
  if (idx < 0) return true;  // 未知画面名は無視
  if (cbor::type(v) != cbor::Type::Map) {
    c->err = ThemeManifestError::kBadValue;
    return false;
  }
  c->scratch_i = idx;
  c->scratch_skin = ThemeScreenSkin{};
  if (!cbor::map_foreach(v, screen_skin_cb, c)) return false;
  // 非 "*" は screen_skin_cb が o->screens[idx] に直接書いている。
  if (idx == kThemeScreenWildcard) o->wildcard_skin = c->scratch_skin;
  o->screens_set |= (1u << idx);
  return true;
}

// style の各フィールド。
bool style_cb(const cbor::Value& k, const cbor::Value& v, void* c_) {
  Ctx* c = static_cast<Ctx*>(c_);
  ThemeStyleSkin& s = c->out->style;
  uint64_t u = 0;
  if (key_eq(k, "card_radius")) {
    if (!take_uint(c, v, 255, &u)) return false;
    s.card_radius = static_cast<uint8_t>(u);
    s.set |= 1u << kStyleCardRadius;
  } else if (key_eq(k, "card_opa")) {
    if (!take_uint(c, v, 255, &u)) return false;
    s.card_opa = static_cast<uint8_t>(u);
    s.set |= 1u << kStyleCardOpa;
  } else if (key_eq(k, "border_w")) {
    if (!take_uint(c, v, 16, &u)) return false;
    s.border_w = static_cast<uint8_t>(u);
    s.set |= 1u << kStyleBorderW;
  } else if (key_eq(k, "btn_radius")) {
    if (!take_uint(c, v, 255, &u)) return false;
    s.btn_radius = static_cast<uint8_t>(u);
    s.set |= 1u << kStyleBtnRadius;
  } else if (key_eq(k, "glow_w")) {
    if (!take_uint(c, v, 64, &u)) return false;
    s.glow_w = static_cast<uint8_t>(u);
    s.set |= 1u << kStyleGlowW;
  } else if (key_eq(k, "header")) {
    const char* p = nullptr;
    size_t n = 0;
    if (!cbor::as_text(v, &p, &n)) {
      c->err = ThemeManifestError::kBadValue;
      return false;
    }
    if (n == 4 && std::memcmp(p, "flat", 4) == 0) {
      s.header = 1;
    } else if (!(n == 7 && std::memcmp(p, "default", 7) == 0)) {
      c->err = ThemeManifestError::kBadValue;
      return false;
    }
    s.set |= 1u << kStyleHeader;
  } else if (key_eq(k, "border")) {
    if (!parse_color(v, &s.border)) {
      c->err = ThemeManifestError::kBadValue;
      return false;
    }
    s.set |= 1u << kStyleBorder;
  } else if (key_eq(k, "glow_color")) {
    if (!parse_color(v, &s.glow)) {
      c->err = ThemeManifestError::kBadValue;
      return false;
    }
    s.set |= 1u << kStyleGlow;
  }
  return true;
}

// icons.<app> = "file.png|bin"
bool icons_cb(const cbor::Value& k, const cbor::Value& v, void* c_) {
  Ctx* c = static_cast<Ctx*>(c_);
  ThemeManifest* o = c->out;
  const char* p = nullptr;
  size_t n = 0;
  if (o->icon_count >= kThemeMaxIcons) return true;  // 溢れは無視
  const char* ap = nullptr;
  size_t an = 0;
  if (!cbor::as_text(k, &ap, &an) || !app_id_ok(ap, an)) return true;
  if (!cbor::as_text(v, &p, &n) || !file_name_ok(p, n)) {
    c->err = ThemeManifestError::kBadImageName;
    return false;
  }
  ThemeIcon& ic = o->icons[o->icon_count++];
  std::memcpy(ic.app, ap, an);
  ic.app[an] = '\0';
  std::memcpy(ic.file, p, n);
  ic.file[n] = '\0';
  return true;
}

// fonts.<slot> = "file.bin"
bool fonts_cb(const cbor::Value& k, const cbor::Value& v, void* c_) {
  Ctx* c = static_cast<Ctx*>(c_);
  ThemeManifest* o = c->out;
  for (int i = 0; i < 4; ++i) {
    if (!key_eq(k, kThemeFontKeys[i])) continue;
    const char* p = nullptr;
    size_t n = 0;
    if (!cbor::as_text(v, &p, &n) || !font_name_ok(p, n)) {
      c->err = ThemeManifestError::kBadImageName;
      return false;
    }
    std::memcpy(o->font_file[i], p, n);
    o->font_file[i][n] = '\0';
    o->font_file_set |= static_cast<uint8_t>(1u << i);
    return true;
  }
  return true;
}

// mascot.expr.<name> = "file"
bool mascot_expr_cb(const cbor::Value& k, const cbor::Value& v, void* c_) {
  Ctx* c = static_cast<Ctx*>(c_);
  ThemeManifest* o = c->out;
  if (o->mascot.expr_count >= kThemeMascotExprMax) return true;
  const char* np = nullptr;
  size_t nn = 0;
  const char* fp = nullptr;
  size_t fn = 0;
  if (!cbor::as_text(k, &np, &nn) || nn == 0 || nn >= 16 ||
      !cbor::as_text(v, &fp, &fn) || !file_name_ok(fp, fn)) {
    c->err = ThemeManifestError::kBadImageName;
    return false;
  }
  const int i = o->mascot.expr_count++;
  std::memcpy(o->mascot.expr_name[i], np, nn);
  o->mascot.expr_name[i][nn] = '\0';
  std::memcpy(o->mascot.expr_file[i], fp, fn);
  o->mascot.expr_file[i][fn] = '\0';
  return true;
}

// mascot.lines = ["..."] / mascot.screens = ["home", ...]
bool mascot_cb(const cbor::Value& k, const cbor::Value& v, void* c_) {
  Ctx* c = static_cast<Ctx*>(c_);
  ThemeManifest* o = c->out;
  if (key_eq(k, "expr")) {
    if (cbor::type(v) != cbor::Type::Map) {
      c->err = ThemeManifestError::kBadValue;
      return false;
    }
    return cbor::map_foreach(v, mascot_expr_cb, c);
  }
  if (key_eq(k, "lines")) {
    if (cbor::type(v) != cbor::Type::Array) {
      c->err = ThemeManifestError::kBadValue;
      return false;
    }
    size_t cnt = 0;
    if (!cbor::container_count(v, &cnt)) return false;
    for (size_t i = 0; i < cnt && o->mascot.line_count < kThemeMascotLineMax;
         ++i) {
      cbor::Value e;
      if (!cbor::array_at(v, i, &e)) {
        c->err = ThemeManifestError::kBadCbor;
        return false;
      }
      const char* p = nullptr;
      size_t n = 0;
      if (!cbor::as_text(e, &p, &n) || n >= kThemeMascotLineMaxLen) {
        c->err = ThemeManifestError::kBadValue;
        return false;
      }
      const int j = o->mascot.line_count++;
      std::memcpy(o->mascot.line[j], p, n);
      o->mascot.line[j][n] = '\0';
    }
    return true;
  }
  if (key_eq(k, "screens")) {
    if (cbor::type(v) != cbor::Type::Array) {
      c->err = ThemeManifestError::kBadValue;
      return false;
    }
    size_t cnt = 0;
    if (!cbor::container_count(v, &cnt)) return false;
    for (size_t i = 0; i < cnt; ++i) {
      cbor::Value e;
      if (!cbor::array_at(v, i, &e)) {
        c->err = ThemeManifestError::kBadCbor;
        return false;
      }
      const int idx = screen_index(e);
      if (idx >= 0) o->mascot.screens |= (1u << idx);
    }
    return true;
  }
  if (key_eq(k, "x") || key_eq(k, "y")) {
    int64_t d = 0;
    if (!cbor::as_int(v, &d) || d < -4096 || d > 4096) {
      c->err = ThemeManifestError::kBadValue;
      return false;
    }
    if (key_eq(k, "x")) o->mascot.x = static_cast<int16_t>(d);
    else o->mascot.y = static_cast<int16_t>(d);
    return true;
  }
  return true;
}

// face_layout.<elem> = {x,y,w,h,font,color,img}
bool face_elem_cb(const cbor::Value& k, const cbor::Value& v, void* c_) {
  Ctx* c = static_cast<Ctx*>(c_);
  ThemeFaceElem& e = c->out->face_elem[c->scratch_i];
  int64_t d = 0;
  if (key_eq(k, "x") || key_eq(k, "y") || key_eq(k, "w") || key_eq(k, "h") ||
      key_eq(k, "font")) {
    const int bit = key_eq(k, "x")   ? 0
                    : key_eq(k, "y") ? 1
                    : key_eq(k, "w") ? 2
                    : key_eq(k, "h") ? 3
                                     : 4;
    const int64_t lim = (bit <= 3) ? 4096 : 400;
    if (!cbor::as_int(v, &d) || d < -lim || d > lim) {
      c->err = ThemeManifestError::kBadValue;
      return false;
    }
    switch (bit) {
      case 0: e.x = static_cast<int16_t>(d); break;
      case 1: e.y = static_cast<int16_t>(d); break;
      case 2: e.w = static_cast<int16_t>(d); break;
      case 3: e.h = static_cast<int16_t>(d); break;
      case 4: e.font = static_cast<int16_t>(d); break;
    }
    e.set |= static_cast<uint8_t>(1u << bit);
    return true;
  }
  if (key_eq(k, "color")) {
    if (!parse_color(v, &e.color)) {
      c->err = ThemeManifestError::kBadValue;
      return false;
    }
    e.set |= 1u << 5;
    return true;
  }
  if (key_eq(k, "img")) {
    const char* p = nullptr;
    size_t n = 0;
    if (!cbor::as_text(v, &p, &n) || !file_name_ok(p, n)) {
      c->err = ThemeManifestError::kBadImageName;
      return false;
    }
    std::memcpy(e.img, p, n);
    e.img[n] = '\0';
    e.set |= 1u << 6;
    return true;
  }
  return true;
}

bool face_layout_cb(const cbor::Value& k, const cbor::Value& v, void* c_) {
  Ctx* c = static_cast<Ctx*>(c_);
  ThemeManifest* o = c->out;
  for (int i = 0; i < kThemeFaceElemCount; ++i) {
    if (!key_eq(k, kThemeFaceElemNames[i])) continue;
    if (cbor::type(v) != cbor::Type::Map) {
      c->err = ThemeManifestError::kBadValue;
      return false;
    }
    c->scratch_i = i;
    if (!cbor::map_foreach(v, face_elem_cb, c)) return false;
    if (o->face_elem[i].set != 0) o->face_layout_set |= (1u << i);
    return true;
  }
  return true;  // 未知 elem は無視
}

// ---- v3: skin ----

// skin.<part>.states.<state> = "file.png|bin"
bool skin_states_cb(const cbor::Value& k, const cbor::Value& v, void* c_) {
  Ctx* c = static_cast<Ctx*>(c_);
  ThemeSkinPart& p = c->out->skin_part[c->scratch_i];
  for (int i = 0; i < kThemeSkinStateCount; ++i) {
    if (!key_eq(k, kThemeSkinStateNames[i])) continue;
    const char* fp = nullptr;
    size_t fn = 0;
    if (!cbor::as_text(v, &fp, &fn) || !file_name_ok(fp, fn)) {
      c->err = ThemeManifestError::kBadImageName;
      return false;
    }
    std::memcpy(p.state_img[i], fp, fn);
    p.state_img[i][fn] = '\0';
    p.state_set |= static_cast<uint8_t>(1u << i);
    return true;
  }
  return true;  // 未知 state は無視
}

// slice/pad = [l,t,r,b] 4要素の uint 配列 (各 ≤128px)。
bool take_quad(Ctx* c, const cbor::Value& v, uint8_t out[4]) {
  if (cbor::type(v) != cbor::Type::Array) {
    c->err = ThemeManifestError::kBadValue;
    return false;
  }
  size_t cnt = 0;
  if (!cbor::container_count(v, &cnt) || cnt != 4) {
    c->err = ThemeManifestError::kBadValue;
    return false;
  }
  for (size_t i = 0; i < 4; ++i) {
    cbor::Value e;
    uint64_t u = 0;
    if (!cbor::array_at(v, i, &e) || !cbor::as_uint(e, &u) || u > 128) {
      c->err = ThemeManifestError::kBadValue;
      return false;
    }
    out[i] = static_cast<uint8_t>(u);
  }
  return true;
}

// skin.<part> = {img, slice, pad, states, text, text_pressed}
bool skin_part_cb(const cbor::Value& k, const cbor::Value& v, void* c_) {
  Ctx* c = static_cast<Ctx*>(c_);
  ThemeSkinPart& p = c->out->skin_part[c->scratch_i];
  if (key_eq(k, "img")) {
    const char* fp = nullptr;
    size_t fn = 0;
    if (!cbor::as_text(v, &fp, &fn) || !file_name_ok(fp, fn)) {
      c->err = ThemeManifestError::kBadImageName;
      return false;
    }
    std::memcpy(p.img, fp, fn);
    p.img[fn] = '\0';
    p.set |= 1u;
    return true;
  }
  if (key_eq(k, "slice")) {
    if (!take_quad(c, v, p.slice)) return false;
    p.set |= 1u << 1;
    return true;
  }
  if (key_eq(k, "pad")) {
    if (!take_quad(c, v, p.pad)) return false;
    p.set |= 1u << 2;
    return true;
  }
  if (key_eq(k, "states")) {
    if (cbor::type(v) != cbor::Type::Map) {
      c->err = ThemeManifestError::kBadValue;
      return false;
    }
    return cbor::map_foreach(v, skin_states_cb, c);
  }
  if (key_eq(k, "text") || key_eq(k, "text_pressed")) {
    const bool pressed = key_eq(k, "text_pressed");
    if (!parse_color(v, pressed ? &p.text_pressed : &p.text)) {
      c->err = ThemeManifestError::kBadValue;
      return false;
    }
    p.set |= 1u << (pressed ? 4 : 3);
    return true;
  }
  return true;  // 未知キーは無視
}

bool skin_cb(const cbor::Value& k, const cbor::Value& v, void* c_) {
  Ctx* c = static_cast<Ctx*>(c_);
  ThemeManifest* o = c->out;
  for (int i = 0; i < kThemeSkinPartCount; ++i) {
    if (!key_eq(k, kThemeSkinPartNames[i])) continue;
    if (cbor::type(v) != cbor::Type::Map) {
      c->err = ThemeManifestError::kBadValue;
      return false;
    }
    c->scratch_i = i;
    if (!cbor::map_foreach(v, skin_part_cb, c)) return false;
    if (o->skin_part[i].img[0]) o->skin_set |= (1u << i);
    return true;
  }
  return true;  // 未知 part は無視
}

}  // namespace

bool theme_id_ok(const char* id) {
  if (!id) return false;
  const size_t n = std::strlen(id);
  if (n == 0 || n >= 32) return false;
  for (size_t i = 0; i < n; ++i) {
    const char c = id[i];
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
    if (!ok) return false;
  }
  return true;
}

ThemeManifestError theme_manifest_parse(const uint8_t* buf, size_t n,
                                        ThemeManifest* out) {
  if (!buf || !out) return ThemeManifestError::kBadCbor;
  const cbor::Value root{buf, buf + n};
  if (cbor::type(root) != cbor::Type::Map) {
    return ThemeManifestError::kBadCbor;
  }

  ThemeManifest o;
  Ctx c{&o};

  cbor::Value v;
  if (!cbor::map_find(root, "id", &v)) return ThemeManifestError::kMissingId;
  {
    const char* p = nullptr;
    size_t len = 0;
    if (!cbor::as_text(v, &p, &len) || len >= sizeof(o.id)) {
      return ThemeManifestError::kBadId;
    }
    std::memcpy(o.id, p, len);
    o.id[len] = '\0';
    if (!theme_id_ok(o.id)) return ThemeManifestError::kBadId;
  }

  {
    uint64_t api = 0;
    if (!cbor::map_find(root, "api", &v) || !cbor::as_uint(v, &api) ||
        api != 1) {
      return ThemeManifestError::kBadApi;
    }
    o.api = static_cast<uint32_t>(api);
  }

  if (cbor::map_find(root, "name", &v)) {
    const char* p = nullptr;
    size_t len = 0;
    if (!cbor::as_text(v, &p, &len) || len >= sizeof(o.name)) {
      return ThemeManifestError::kBadValue;
    }
    std::memcpy(o.name, p, len);
    o.name[len] = '\0';
  } else {
    std::strncpy(o.name, o.id, sizeof(o.name) - 1);
  }

  if (cbor::map_find(root, "version", &v)) {
    uint64_t u = 0;
    if (!cbor::as_uint(v, &u) || u > 0xFFFF) {
      return ThemeManifestError::kBadValue;
    }
    o.version = static_cast<uint32_t>(u);
  }

  if (cbor::map_find(root, "tokens", &v)) {
    if (cbor::type(v) != cbor::Type::Map) return ThemeManifestError::kBadValue;
    if (!cbor::map_foreach(v, tokens_cb, &c)) return c.err;
  }

  if (cbor::map_find(root, "images", &v)) {
    if (cbor::type(v) != cbor::Type::Map) return ThemeManifestError::kBadValue;
    if (!cbor::map_foreach(v, images_cb, &c)) return c.err;
  }

  if (cbor::map_find(root, "bubble", &v)) {
    if (cbor::type(v) != cbor::Type::Map) return ThemeManifestError::kBadValue;
    if (!cbor::map_foreach(v, bubble_cb, &c)) return c.err;
  }

  // ---- v2 拡張キー (全て任意) ----

  if (cbor::map_find(root, "screens", &v)) {
    if (cbor::type(v) != cbor::Type::Map) return ThemeManifestError::kBadValue;
    if (!cbor::map_foreach(v, screens_map_cb, &c)) return c.err;
  }

  if (cbor::map_find(root, "style", &v)) {
    if (cbor::type(v) != cbor::Type::Map) return ThemeManifestError::kBadValue;
    if (!cbor::map_foreach(v, style_cb, &c)) return c.err;
  }

  if (cbor::map_find(root, "icons", &v)) {
    if (cbor::type(v) != cbor::Type::Map) return ThemeManifestError::kBadValue;
    if (!cbor::map_foreach(v, icons_cb, &c)) return c.err;
  }

  if (cbor::map_find(root, "fonts", &v)) {
    if (cbor::type(v) != cbor::Type::Map) return ThemeManifestError::kBadValue;
    if (!cbor::map_foreach(v, fonts_cb, &c)) return c.err;
  }

  if (cbor::map_find(root, "mascot", &v)) {
    if (cbor::type(v) != cbor::Type::Map) return ThemeManifestError::kBadValue;
    o.mascot.used = 1;
    if (!cbor::map_foreach(v, mascot_cb, &c)) return c.err;
  }

  if (cbor::map_find(root, "face_layout", &v)) {
    if (cbor::type(v) != cbor::Type::Map) return ThemeManifestError::kBadValue;
    if (!cbor::map_foreach(v, face_layout_cb, &c)) return c.err;
  }

  if (cbor::map_find(root, "skin", &v)) {
    if (cbor::type(v) != cbor::Type::Map) return ThemeManifestError::kBadValue;
    if (!cbor::map_foreach(v, skin_cb, &c)) return c.err;
  }

  *out = o;
  return ThemeManifestError::kOk;
}

}  // namespace watch
