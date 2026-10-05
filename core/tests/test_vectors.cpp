// test_vectors.cpp — docs/protocol-vectors/*.json を両側共通の期待値として
// encode/decode 結果の一致を検査する。JSON の object はキー順を保持する
// (正規形 = 定義順)。{"$bytes":"<hex>"} は byte string。
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "watch/protocol/cbor.hpp"
#include "watch/protocol/frame.hpp"

using namespace watch;

#ifndef WATCH_VECTORS_DIR
#define WATCH_VECTORS_DIR "../../docs/protocol-vectors"
#endif

namespace {

// ---------------- 最小 JSON (object は挿入順を保持) ----------------

struct J {
  enum class T { Null, Bool, Int, Str, Bytes, Arr, Obj } t = T::Null;
  bool b = false;
  int64_t i = 0;
  std::string s;                               // Str / Bytes (hex decoded)
  std::vector<J> arr;
  std::vector<std::pair<std::string, J>> obj;  // ordered
};

bool hex_decode(const std::string& in, std::vector<uint8_t>* out) {
  out->clear();
  int hi = -1;
  for (char c : in) {
    int v;
    if (c >= '0' && c <= '9') {
      v = c - '0';
    } else if (c >= 'a' && c <= 'f') {
      v = c - 'a' + 10;
    } else if (c >= 'A' && c <= 'F') {
      v = c - 'A' + 10;
    } else if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
      continue;
    } else {
      return false;
    }
    if (hi < 0) {
      hi = v;
    } else {
      out->push_back(static_cast<uint8_t>((hi << 4) | v));
      hi = -1;
    }
  }
  return hi < 0;
}

std::string hex_encode(const uint8_t* p, size_t n) {
  static const char* kHex = "0123456789abcdef";
  std::string s;
  s.reserve(n * 2);
  for (size_t i = 0; i < n; ++i) {
    s += kHex[p[i] >> 4];
    s += kHex[p[i] & 0xF];
  }
  return s;
}

struct JsonParser {
  const char* p;
  const char* end;
  bool fail = false;

  explicit JsonParser(const std::string& s)
      : p(s.data()), end(s.data() + s.size()) {}

  void ws() {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
      ++p;
    }
  }

  bool literal(const char* w) {
    const size_t n = std::strlen(w);
    if (static_cast<size_t>(end - p) < n || std::memcmp(p, w, n) != 0) {
      fail = true;
      return false;
    }
    p += n;
    return true;
  }

  static void utf8(std::string* out, uint32_t cp) {
    if (cp < 0x80) {
      out->push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  bool str(std::string* out) {
    ws();
    if (p >= end || *p != '"') {
      fail = true;
      return false;
    }
    ++p;
    out->clear();
    while (p < end && *p != '"') {
      if (*p == '\\') {
        ++p;
        if (p >= end) break;
        switch (*p) {
          case '"': out->push_back('"'); break;
          case '\\': out->push_back('\\'); break;
          case '/': out->push_back('/'); break;
          case 'b': out->push_back('\b'); break;
          case 'f': out->push_back('\f'); break;
          case 'n': out->push_back('\n'); break;
          case 'r': out->push_back('\r'); break;
          case 't': out->push_back('\t'); break;
          case 'u': {
            if (end - p < 5) {
              fail = true;
              return false;
            }
            uint32_t cp = 0;
            for (int k = 0; k < 4; ++k) {
              ++p;
              const char c = *p;
              cp <<= 4;
              cp += (c >= '0' && c <= '9')   ? uint32_t(c - '0')
                    : (c >= 'a' && c <= 'f') ? uint32_t(c - 'a' + 10)
                    : (c >= 'A' && c <= 'F') ? uint32_t(c - 'A' + 10)
                                             : 0;
            }
            utf8(out, cp);
            break;
          }
          default:
            fail = true;
            return false;
        }
        ++p;
      } else {
        out->push_back(*p++);
      }
    }
    if (p >= end) {
      fail = true;
      return false;
    }
    ++p;  // closing "
    return true;
  }

  bool value(J* j) {
    ws();
    if (p >= end) {
      fail = true;
      return false;
    }
    switch (*p) {
      case 'n':
        j->t = J::T::Null;
        return literal("null");
      case 't':
        j->t = J::T::Bool;
        j->b = true;
        return literal("true");
      case 'f':
        j->t = J::T::Bool;
        j->b = false;
        return literal("false");
      case '"':
        j->t = J::T::Str;
        return str(&j->s);
      case '[': {
        ++p;
        j->t = J::T::Arr;
        ws();
        if (p < end && *p == ']') {
          ++p;
          return true;
        }
        while (true) {
          J e;
          if (!value(&e)) return false;
          j->arr.push_back(std::move(e));
          ws();
          if (p < end && *p == ',') {
            ++p;
            continue;
          }
          if (p < end && *p == ']') {
            ++p;
            return true;
          }
          fail = true;
          return false;
        }
      }
      case '{': {
        ++p;
        j->t = J::T::Obj;
        ws();
        if (p < end && *p == '}') {
          ++p;
        } else {
          while (true) {
            std::string key;
            if (!str(&key)) return false;
            ws();
            if (p >= end || *p != ':') {
              fail = true;
              return false;
            }
            ++p;
            J v;
            if (!value(&v)) return false;
            j->obj.emplace_back(std::move(key), std::move(v));
            ws();
            if (p < end && *p == ',') {
              ++p;
              continue;
            }
            if (p < end && *p == '}') {
              ++p;
              break;
            }
            fail = true;
            return false;
          }
        }
        // {"$bytes": "<hex>"} は byte string。
        if (j->obj.size() == 1 && j->obj[0].first == "$bytes" &&
            j->obj[0].second.t == J::T::Str) {
          std::vector<uint8_t> b;
          if (!hex_decode(j->obj[0].second.s, &b)) {
            fail = true;
            return false;
          }
          j->t = J::T::Bytes;
          j->s.assign(reinterpret_cast<const char*>(b.data()), b.size());
          j->obj.clear();
        }
        return true;
      }
      default: {
        // 整数のみ許容 (指数・小数点は使わない)。
        bool neg = false;
        if (*p == '-') {
          neg = true;
          ++p;
        }
        if (p >= end || *p < '0' || *p > '9') {
          fail = true;
          return false;
        }
        int64_t i = 0;
        while (p < end && *p >= '0' && *p <= '9') {
          i = i * 10 + (*p - '0');
          ++p;
        }
        if (p < end && (*p == '.' || *p == 'e' || *p == 'E')) {
          fail = true;
          return false;
        }
        j->t = J::T::Int;
        j->i = neg ? -i : i;
        return true;
      }
    }
  }
};

bool parse_file(const char* name, J* root) {
  const std::string path = std::string(WATCH_VECTORS_DIR) + "/" + name;
  std::ifstream f(path);
  if (!f) {
    ADD_FAILURE() << "cannot open " << path;
    return false;
  }
  std::stringstream ss;
  ss << f.rdbuf();
  const std::string text = ss.str();
  JsonParser p(text);
  if (!p.value(root) || p.fail) {
    ADD_FAILURE() << "parse failed: " << path;
    return false;
  }
  return true;
}

const J* jfind(const J& obj, const char* key) {
  for (const auto& kv : obj.obj) {
    if (kv.first == key) return &kv.second;
  }
  return nullptr;
}

// ---------------- JSON → CBOR ----------------

void j_to_cbor(const J& j, cbor::Writer* w) {
  switch (j.t) {
    case J::T::Null: w->null_v(); break;
    case J::T::Bool: w->bool_v(j.b); break;
    case J::T::Int: w->int_v(j.i); break;
    case J::T::Str: w->text(j.s.data(), j.s.size()); break;
    case J::T::Bytes:
      w->bytes(reinterpret_cast<const uint8_t*>(j.s.data()), j.s.size());
      break;
    case J::T::Arr:
      w->array(j.arr.size());
      for (const J& e : j.arr) j_to_cbor(e, w);
      break;
    case J::T::Obj:
      w->map(j.obj.size());
      for (const auto& kv : j.obj) {
        w->text(kv.first.data(), kv.first.size());
        j_to_cbor(kv.second, w);
      }
      break;
  }
}

std::vector<uint8_t> j_to_cbor(const J& j) {
  std::vector<uint8_t> buf(8192);
  cbor::Writer w(buf.data(), buf.size());
  j_to_cbor(j, &w);
  EXPECT_TRUE(w.ok());
  buf.resize(w.size());
  return buf;
}

// ---------------- CBOR → JSON (decode 比較用) ----------------

bool cbor_to_j(const cbor::Value& v, J* j) {
  switch (cbor::type(v)) {
    case cbor::Type::Uint:
    case cbor::Type::Nint: {
      int64_t i;
      if (!cbor::as_int(v, &i)) return false;
      j->t = J::T::Int;
      j->i = i;
      return true;
    }
    case cbor::Type::Bytes: {
      const uint8_t* p;
      size_t n;
      if (!cbor::as_bytes(v, &p, &n)) return false;
      j->t = J::T::Bytes;
      j->s.assign(reinterpret_cast<const char*>(p), n);
      return true;
    }
    case cbor::Type::Text: {
      const char* s;
      size_t n;
      if (!cbor::as_text(v, &s, &n)) return false;
      j->t = J::T::Str;
      j->s.assign(s, n);
      return true;
    }
    case cbor::Type::Array: {
      size_t n;
      if (!cbor::container_count(v, &n)) return false;
      j->t = J::T::Arr;
      j->arr.resize(n);
      for (size_t i = 0; i < n; ++i) {
        cbor::Value e;
        if (!cbor::array_at(v, i, &e) || !cbor_to_j(e, &j->arr[i])) {
          return false;
        }
      }
      return true;
    }
    case cbor::Type::Map: {
      j->t = J::T::Obj;
      struct Ctx {
        J* j;
        bool ok;
      } ctx{j, true};
      const bool done = cbor::map_foreach(
          v,
          [](const cbor::Value& k, const cbor::Value& val, void* c) {
            Ctx* cc = static_cast<Ctx*>(c);
            const char* s;
            size_t n;
            if (!cbor::as_text(k, &s, &n)) {
              cc->ok = false;
              return false;
            }
            J child;
            if (!cbor_to_j(val, &child)) {
              cc->ok = false;
              return false;
            }
            cc->j->obj.emplace_back(std::string(s, n), std::move(child));
            return true;
          },
          &ctx);
      return done && ctx.ok;
    }
    case cbor::Type::Bool: {
      bool b;
      if (!cbor::as_bool(v, &b)) return false;
      j->t = J::T::Bool;
      j->b = b;
      return true;
    }
    case cbor::Type::Null:
      j->t = J::T::Null;
      return true;
    default:
      return false;
  }
}

// 値の一致 (map はキー順込みで比較: 正規形は定義順なので厳密に)。
bool j_eq(const J& a, const J& b) {
  if (a.t != b.t) return false;
  switch (a.t) {
    case J::T::Null:
      return true;
    case J::T::Bool:
      return a.b == b.b;
    case J::T::Int:
      return a.i == b.i;
    case J::T::Str:
    case J::T::Bytes:
      return a.s == b.s;
    case J::T::Arr:
      if (a.arr.size() != b.arr.size()) return false;
      for (size_t i = 0; i < a.arr.size(); ++i) {
        if (!j_eq(a.arr[i], b.arr[i])) return false;
      }
      return true;
    case J::T::Obj:
      if (a.obj.size() != b.obj.size()) return false;
      for (size_t i = 0; i < a.obj.size(); ++i) {
        if (a.obj[i].first != b.obj[i].first) return false;
        if (!j_eq(a.obj[i].second, b.obj[i].second)) return false;
      }
      return true;
  }
  return false;
}

// ---------------- frame cases ----------------

proto::FrameType type_of(const std::string& name) {
  if (name == "REQ") return proto::FrameType::Req;
  if (name == "RES") return proto::FrameType::Res;
  if (name == "EVT") return proto::FrameType::Evt;
  if (name == "BULK_START") return proto::FrameType::BulkStart;
  if (name == "BULK_CHUNK") return proto::FrameType::BulkChunk;
  if (name == "BULK_ACK") return proto::FrameType::BulkAck;
  if (name == "BULK_END") return proto::FrameType::BulkEnd;
  ADD_FAILURE() << "unknown type " << name;
  return proto::FrameType::Req;
}

proto::FrameError error_of(const std::string& name) {
  if (name == "too_short") return proto::FrameError::TooShort;
  if (name == "bad_version") return proto::FrameError::BadVersion;
  if (name == "bad_length") return proto::FrameError::BadLength;
  if (name == "bad_crc") return proto::FrameError::BadCrc;
  ADD_FAILURE() << "unknown error " << name;
  return proto::FrameError::Ok;
}

bool emit_collect(const uint8_t* f, size_t n, void* ctx) {
  static_cast<std::vector<std::vector<uint8_t>>*>(ctx)->emplace_back(f, f + n);
  return true;
}

void run_frame_case(const J& c) {
  const J* name = jfind(c, "name");
  const std::string label =
      name && name->t == J::T::Str ? name->s : std::string("?");

  if (const J* err = jfind(c, "expect_error")) {
    // デコード専用の失敗ケース。
    std::vector<uint8_t> raw;
    ASSERT_TRUE(hex_decode(jfind(c, "raw_hex")->s, &raw)) << label;
    proto::Frame f;
    EXPECT_EQ(proto::frame_parse(raw.data(), raw.size(), &f),
              error_of(err->s))
        << label;
    return;
  }

  const proto::FrameType type = type_of(jfind(c, "type")->s);
  const uint16_t msg_id = static_cast<uint16_t>(jfind(c, "msg_id")->i);
  const J* mtu_j = jfind(c, "mtu");
  const size_t mtu = mtu_j ? static_cast<size_t>(mtu_j->i) : 247;
  std::vector<uint8_t> payload;
  ASSERT_TRUE(hex_decode(jfind(c, "payload_hex")->s, &payload)) << label;
  const J* frames = jfind(c, "frames");
  ASSERT_TRUE(frames && frames->t == J::T::Arr) << label;

  // encode 側: payload → fragment → frames hex と一致。
  std::vector<std::vector<uint8_t>> out;
  ASSERT_TRUE(proto::send_message(type, msg_id, payload.data(), payload.size(),
                                  mtu, emit_collect, &out))
      << label;
  ASSERT_EQ(out.size(), frames->arr.size()) << label;
  for (size_t i = 0; i < out.size(); ++i) {
    EXPECT_EQ(hex_encode(out[i].data(), out[i].size()), frames->arr[i].s)
        << label << " frame " << i;
  }

  // decode 側: frames hex → parse → reassemble → payload と一致。
  proto::Reassembler r;
  proto::Frame done;
  bool complete = false;
  for (size_t i = 0; i < frames->arr.size(); ++i) {
    std::vector<uint8_t> raw;
    ASSERT_TRUE(hex_decode(frames->arr[i].s, &raw)) << label;
    proto::Frame f;
    ASSERT_EQ(proto::frame_parse(raw.data(), raw.size(), &f),
              proto::FrameError::Ok)
        << label << " frame " << i;
    EXPECT_EQ(f.type, type) << label;
    EXPECT_EQ(f.msg_id, msg_id) << label;
    EXPECT_EQ(f.seq, i) << label;
    complete = r.feed(f, &done);
  }
  ASSERT_TRUE(complete) << label;
  EXPECT_EQ(done.payload_len, payload.size()) << label;
  EXPECT_EQ(std::memcmp(done.payload, payload.data(), payload.size()), 0)
      << label;
}

void run_file(const char* name) {
  J root;
  ASSERT_TRUE(parse_file(name, &root));
  const J* cases = jfind(root, "cases");
  ASSERT_TRUE(cases && cases->t == J::T::Arr) << name;
  const std::string kind =
      jfind(root, "kind") ? jfind(root, "kind")->s : std::string();

  for (const J& c : cases->arr) {
    const J* nm = jfind(c, "name");
    const std::string label =
        std::string(name) + "/" + (nm ? nm->s : std::string("?"));
    if (kind == "cbor") {
      // encode: value → CBOR → hex と一致。
      const std::vector<uint8_t> enc = j_to_cbor(*jfind(c, "value"));
      const std::string want = jfind(c, "hex")->s;
      EXPECT_EQ(hex_encode(enc.data(), enc.size()), want) << label;
      // decode: hex → CBOR → value と一致。
      std::vector<uint8_t> raw;
      ASSERT_TRUE(hex_decode(want, &raw)) << label;
      cbor::Value top{raw.data(), raw.data() + raw.size()};
      J got;
      ASSERT_TRUE(cbor_to_j(top, &got)) << label;
      EXPECT_TRUE(j_eq(*jfind(c, "value"), got)) << label;
    } else {
      run_frame_case(c);
    }
  }
}

}  // namespace

TEST(Vectors, Cbor) { run_file("cbor.json"); }

TEST(Vectors, Frame) { run_file("frame.json"); }

TEST(Vectors, Messages) { run_file("messages.json"); }
