// 最小限の CBOR。definite-length のみ。壊れた入力は全部 false で返す。
#include "watch/protocol/cbor.hpp"

#include <cstring>

namespace watch {
namespace cbor {

// ---------- Writer ----------

void Writer::put(uint8_t b) {
  if (len_ < cap_) {
    buf_[len_++] = b;
  } else {
    ok_ = false;
  }
}

void Writer::put_n(const uint8_t* p, size_t n) {
  if (len_ + n <= cap_) {
    std::memcpy(buf_ + len_, p, n);
    len_ += n;
  } else {
    ok_ = false;
  }
}

void Writer::head(uint8_t major, uint64_t v) {
  const uint8_t m = static_cast<uint8_t>(major << 5);
  if (v < 24) {
    put(m | static_cast<uint8_t>(v));
  } else if (v <= 0xFF) {
    put(m | 24);
    put(static_cast<uint8_t>(v));
  } else if (v <= 0xFFFF) {
    put(m | 25);
    put(static_cast<uint8_t>(v >> 8));
    put(static_cast<uint8_t>(v));
  } else if (v <= 0xFFFFFFFFULL) {
    put(m | 26);
    for (int i = 3; i >= 0; --i) put(static_cast<uint8_t>(v >> (i * 8)));
  } else {
    put(m | 27);
    for (int i = 7; i >= 0; --i) put(static_cast<uint8_t>(v >> (i * 8)));
  }
}

Writer& Writer::uint_v(uint64_t v) {
  head(0, v);
  return *this;
}

Writer& Writer::int_v(int64_t v) {
  if (v >= 0) {
    head(0, static_cast<uint64_t>(v));
  } else {
    // -1-n を unsigned で表す (INT64_MIN でもオーバーフローしない)。
    head(1, static_cast<uint64_t>(-(v + 1)));
  }
  return *this;
}

Writer& Writer::bool_v(bool v) {
  put(v ? 0xF5 : 0xF4);
  return *this;
}

Writer& Writer::null_v() {
  put(0xF6);
  return *this;
}

Writer& Writer::text(const char* s) { return text(s, s ? std::strlen(s) : 0); }

Writer& Writer::text(const char* s, size_t n) {
  if (!s && n > 0) {
    ok_ = false;
    return *this;
  }
  head(3, n);
  put_n(reinterpret_cast<const uint8_t*>(s), n);
  return *this;
}

Writer& Writer::bytes(const uint8_t* p, size_t n) {
  if (!p && n > 0) {
    ok_ = false;
    return *this;
  }
  head(2, n);
  put_n(p, n);
  return *this;
}

Writer& Writer::map(size_t pairs) {
  head(5, pairs);
  return *this;
}

Writer& Writer::array(size_t count) {
  head(4, count);
  return *this;
}

Writer& Writer::raw(const uint8_t* p, size_t n) {
  if (!p && n > 0) {
    ok_ = false;
    return *this;
  }
  put_n(p, n);
  return *this;
}

// ---------- Reader ----------

namespace {

// head を読む。v の位置を arg バイト分進める。
bool read_head(Value& v, uint8_t* major, uint64_t* arg) {
  if (!v.p || v.p >= v.end) return false;
  const uint8_t ib = *v.p;
  *major = static_cast<uint8_t>(ib >> 5);
  const uint8_t ai = ib & 0x1F;
  const uint8_t* p = v.p + 1;
  uint64_t a = 0;
  if (ai < 24) {
    a = ai;
  } else if (ai == 24) {
    if (v.end - p < 1) return false;
    a = p[0];
    p += 1;
  } else if (ai == 25) {
    if (v.end - p < 2) return false;
    a = (static_cast<uint64_t>(p[0]) << 8) | p[1];
    p += 2;
  } else if (ai == 26) {
    if (v.end - p < 4) return false;
    for (int i = 0; i < 4; ++i) a = (a << 8) | p[i];
    p += 4;
  } else if (ai == 27) {
    if (v.end - p < 8) return false;
    for (int i = 0; i < 8; ++i) a = (a << 8) | p[i];
    p += 8;
  } else {
    return false;  // 28-30 予約、31 indefinite-length → 扱わない
  }
  *arg = a;
  v.p = p;
  return true;
}

}  // namespace

Type type(const Value& v) {
  if (!v.p || v.p >= v.end) return Type::Invalid;
  const uint8_t major = static_cast<uint8_t>((*v.p) >> 5);
  const uint8_t ai = (*v.p) & 0x1F;
  switch (major) {
    case 0:
      return Type::Uint;
    case 1:
      return Type::Nint;
    case 2:
      return Type::Bytes;
    case 3:
      return Type::Text;
    case 4:
      return Type::Array;
    case 5:
      return Type::Map;
    case 7:
      if (ai == 20 || ai == 21) return Type::Bool;
      if (ai == 22 || ai == 23) return Type::Null;
      return Type::Invalid;
    default:
      return Type::Invalid;
  }
}

bool as_uint(const Value& v, uint64_t* out) {
  Value t = v;
  uint8_t major;
  uint64_t arg;
  if (!read_head(t, &major, &arg) || major != 0) return false;
  if (out) *out = arg;
  return true;
}

bool as_int(const Value& v, int64_t* out) {
  Value t = v;
  uint8_t major;
  uint64_t arg;
  if (!read_head(t, &major, &arg)) return false;
  int64_t r;
  if (major == 0) {
    if (arg > static_cast<uint64_t>(INT64_MAX)) return false;
    r = static_cast<int64_t>(arg);
  } else if (major == 1) {
    if (arg > static_cast<uint64_t>(INT64_MAX)) return false;
    r = -1 - static_cast<int64_t>(arg);
  } else {
    return false;
  }
  if (out) *out = r;
  return true;
}

bool as_bool(const Value& v, bool* out) {
  if (!v.p || v.p >= v.end || (*v.p) >> 5 != 7) return false;
  const uint8_t ai = (*v.p) & 0x1F;
  if (ai != 20 && ai != 21) return false;
  if (out) *out = (ai == 21);
  return true;
}

bool as_text(const Value& v, const char** s, size_t* n) {
  Value t = v;
  uint8_t major;
  uint64_t arg;
  if (!read_head(t, &major, &arg) || major != 3) return false;
  if (arg > static_cast<uint64_t>(t.end - t.p)) return false;
  if (s) *s = reinterpret_cast<const char*>(t.p);
  if (n) *n = static_cast<size_t>(arg);
  return true;
}

bool as_bytes(const Value& v, const uint8_t** p, size_t* n) {
  Value t = v;
  uint8_t major;
  uint64_t arg;
  if (!read_head(t, &major, &arg) || major != 2) return false;
  if (arg > static_cast<uint64_t>(t.end - t.p)) return false;
  if (p) *p = t.p;
  if (n) *n = static_cast<size_t>(arg);
  return true;
}

bool container_count(const Value& v, size_t* out) {
  Value t = v;
  uint8_t major;
  uint64_t arg;
  if (!read_head(t, &major, &arg)) return false;
  if (major != 4 && major != 5) return false;
  if (major == 5) {
    if (arg > (UINT64_MAX / 2)) return false;
    arg *= 2;
  }
  if (out) *out = static_cast<size_t>(arg);
  return true;
}

bool skip(const Value& v, Value* next) {
  Value t = v;
  uint8_t major;
  uint64_t arg;
  if (!read_head(t, &major, &arg)) return false;
  size_t items = 0;
  switch (major) {
    case 0:
    case 1:
      break;  // 整数: head の後にデータは無い
    case 2:
    case 3:
      if (arg > static_cast<uint64_t>(t.end - t.p)) return false;
      t.p += arg;
      break;
    case 4:
      items = static_cast<size_t>(arg);
      break;
    case 5:
      if (arg > (UINT64_MAX / 2)) return false;
      items = static_cast<size_t>(arg) * 2;
      break;
    case 7:
      break;  // simple: ai 24 未満のみ (bool/null)。ai==24 は 1byte 追加
    default:
      return false;
  }
  // major 7 で ai==24 (simple value の拡張) は head 後に1byte。
  // read_head は ai==24 を 1byte arg として読んでいるので既に消費済み。
  for (size_t i = 0; i < items; ++i) {
    if (!skip(t, &t)) return false;
  }
  if (next) *next = t;
  return true;
}

bool map_find(const Value& map, const char* key, Value* out) {
  Value t = map;
  uint8_t major;
  uint64_t pairs;
  if (!read_head(t, &major, &pairs) || major != 5 || !key) return false;
  const size_t klen = std::strlen(key);
  for (uint64_t i = 0; i < pairs; ++i) {
    Value kv = t;
    const char* ks = nullptr;
    size_t kn = 0;
    if (!as_text(kv, &ks, &kn) || !skip(kv, &t)) return false;
    Value vv = t;
    if (!skip(vv, &t)) return false;
    if (kn == klen && std::memcmp(ks, key, kn) == 0) {
      if (out) *out = vv;
      return true;
    }
  }
  return false;
}

bool map_foreach(const Value& map, bool (*fn)(const Value& k, const Value& v,
                                              void* ctx),
                 void* ctx) {
  if (!fn) return false;
  Value t = map;
  uint8_t major;
  uint64_t pairs;
  if (!read_head(t, &major, &pairs) || major != 5) return false;
  for (uint64_t i = 0; i < pairs; ++i) {
    Value kv = t;
    if (!skip(kv, &t)) return false;
    Value vv = t;
    if (!skip(vv, &t)) return false;
    if (!fn(kv, vv, ctx)) return false;
  }
  return true;
}

bool array_at(const Value& arr, size_t i, Value* out) {
  Value t = arr;
  uint8_t major;
  uint64_t count;
  if (!read_head(t, &major, &count) || major != 4) return false;
  if (i >= count) return false;
  for (size_t k = 0; k <= i; ++k) {
    Value elem = t;
    if (!skip(elem, &t)) return false;
    if (k == i) {
      if (out) *out = elem;
      return true;
    }
  }
  return false;
}

}  // namespace cbor
}  // namespace watch
