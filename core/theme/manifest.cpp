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
constexpr ColorKey kColorKeys[10] = {
    {"bg"},      {"surface"}, {"surface2"}, {"primary"}, {"on_primary"},
    {"text"},    {"text_dim"}, {"accent"},   {"danger"},  {"ok"},
};

constexpr const char* kMetricKeys[4] = {"radius_sm", "radius_lg", "space",
                                        "anim_ms"};
constexpr const char* kFontKeys[4] = {"font_body", "font_title", "font_digits",
                                      "font_digits_sm"};
constexpr const char* kImageKeys[kThemeImageSlots] = {"home_bg", "stand",
                                                      "timer_done"};

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

bool file_name_ok(const char* s, size_t n) {
  if (n == 0 || n >= kThemeImageNameMax) return false;
  if (n < 4 || std::memcmp(s + n - 4, ".bin", 4) != 0) return false;
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
};

bool tokens_cb(const cbor::Value& k, const cbor::Value& v, void* c_) {
  Ctx* c = static_cast<Ctx*>(c_);
  ThemeManifest* o = c->out;
  for (int i = 0; i < 10; ++i) {
    if (key_eq(k, kColorKeys[i].key)) {
      if (!parse_color(v, &o->color[i])) {
        c->err = ThemeManifestError::kBadValue;
        return false;
      }
      o->color_set |= static_cast<uint16_t>(1u << i);
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

  *out = o;
  return ThemeManifestError::kOk;
}

}  // namespace watch
