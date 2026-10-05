// protocol/cbor.hpp — 最小限の CBOR (RFC 8949) エンコーダ/デコーダ。
// map/array/int/bool/text/bytes/null のみ。 indefinite-length と
// 浮動小数点は扱わない (protocol-v1.md は全部 definite-length)。
#pragma once

#include <cstddef>
#include <cstdint>

namespace watch {
namespace cbor {

enum class Type : uint8_t {
  Invalid = 0,
  Uint,
  Nint,  // 負の整数
  Bytes,
  Text,
  Array,
  Map,
  Bool,
  Null,
};

// ---------- Writer ----------
// 固定バッファに追記するだけ。失敗したら以後 ok()==false。
class Writer {
 public:
  Writer(uint8_t* buf, size_t cap) : buf_(buf), cap_(cap) {}

  bool ok() const { return ok_; }
  size_t size() const { return len_; }
  const uint8_t* data() const { return buf_; }

  Writer& uint_v(uint64_t v);
  Writer& int_v(int64_t v);  // 負数は Nint
  Writer& bool_v(bool v);
  Writer& null_v();
  Writer& text(const char* s);
  Writer& text(const char* s, size_t n);
  Writer& bytes(const uint8_t* p, size_t n);
  Writer& map(size_t pairs);    // 以後 pairs*2 要素を書く
  Writer& array(size_t count);  // 以後 count 要素を書く
  // 完成済みの CBOR バイト列をそのまま追記する (子 map の埋め込み用)。
  Writer& raw(const uint8_t* p, size_t n);

 private:
  void head(uint8_t major, uint64_t v);
  void put(uint8_t b);
  void put_n(const uint8_t* p, size_t n);

  uint8_t* buf_;
  size_t cap_;
  size_t len_ = 0;
  bool ok_ = true;
};

// ---------- Reader ----------
// バッファ上の1値への参照。コピーしない。
struct Value {
  const uint8_t* p = nullptr;
  const uint8_t* end = nullptr;
};

Type type(const Value& v);
// 値を読む系 (型が合わなければ false)。
bool as_int(const Value& v, int64_t* out);
bool as_uint(const Value& v, uint64_t* out);
bool as_bool(const Value& v, bool* out);
bool as_text(const Value& v, const char** s, size_t* n);
bool as_bytes(const Value& v, const uint8_t** p, size_t* n);
// コンテナの要素数 (pairs ではなく要素数: map は 2*ペア)。
bool container_count(const Value& v, size_t* out);
// map: key (text) に対応する値を out に。無ければ false。
bool map_find(const Value& map, const char* key, Value* out);
// map: 各ペアに fn(key_v, val_v, ctx) を呼ぶ。途中で fn が false なら止める。
bool map_foreach(const Value& map, bool (*fn)(const Value& k, const Value& v,
                                              void* ctx),
                 void* ctx);
// array: i 番目の要素。
bool array_at(const Value& arr, size_t i, Value* out);
// 1値をスキップして次の値の位置を返す (内部用だが再利用可)。
bool skip(const Value& v, Value* next);

}  // namespace cbor
}  // namespace watch
