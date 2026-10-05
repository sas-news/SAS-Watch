// BULK 転送の受信側ロジック。protocol-v1.md の BULK_* を参照。
#include "watch/protocol/bulk.hpp"

#include <cstring>

#include "watch/protocol/cbor.hpp"

namespace watch {
namespace proto {

namespace {

uint16_t read_le16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t read_le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

void write_le16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}

void write_le32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
  p[2] = static_cast<uint8_t>(v >> 16);
  p[3] = static_cast<uint8_t>(v >> 24);
}

}  // namespace

bool bulk_parse_start(const uint8_t* cbor, size_t len, BulkStart* out) {
  if (!cbor || !out) return false;
  cbor::Value top{cbor, cbor + len};
  if (cbor::type(top) != cbor::Type::Map) return false;

  cbor::Value v;
  uint64_t u;

  if (!cbor::map_find(top, "id", &v) || !cbor::as_uint(v, &u) ||
      u > 0xFFFF) {
    return false;
  }
  out->id = static_cast<uint16_t>(u);

  out->kind[0] = '\0';
  if (cbor::map_find(top, "kind", &v)) {
    const char* s = nullptr;
    size_t n = 0;
    if (!cbor::as_text(v, &s, &n) || n >= sizeof(out->kind)) return false;
    std::memcpy(out->kind, s, n);
    out->kind[n] = '\0';
  }

  if (!cbor::map_find(top, "size", &v) || !cbor::as_uint(v, &u) ||
      u > 0xFFFFFFFF) {
    return false;
  }
  out->size = static_cast<uint32_t>(u);

  if (!cbor::map_find(top, "sha256", &v)) return false;
  const uint8_t* p = nullptr;
  size_t n = 0;
  if (!cbor::as_bytes(v, &p, &n) || n != sizeof(out->sha256)) return false;
  std::memcpy(out->sha256, p, n);

  out->chunk = 0;
  if (cbor::map_find(top, "chunk", &v) && cbor::as_uint(v, &u)) {
    out->chunk = static_cast<uint32_t>(u & 0xFFFFFFFF);
  }
  return true;
}

bool bulk_parse_end(const uint8_t* cbor, size_t len, uint16_t* id_out) {
  if (!cbor || !id_out) return false;
  cbor::Value top{cbor, cbor + len};
  if (cbor::type(top) != cbor::Type::Map) return false;
  cbor::Value v;
  uint64_t u;
  if (!cbor::map_find(top, "id", &v) || !cbor::as_uint(v, &u) || u > 0xFFFF) {
    return false;
  }
  *id_out = static_cast<uint16_t>(u);
  return true;
}

size_t bulk_encode_ack(uint16_t id, uint32_t next, uint8_t* out,
                       size_t out_cap) {
  if (!out) return 0;
  cbor::Writer w(out, out_cap);
  w.map(2).text("id").uint_v(id).text("next").uint_v(next);
  return w.ok() ? w.size() : 0;
}

bool bulk_parse_ack(const uint8_t* cbor, size_t len, uint16_t* id_out,
                    uint32_t* next_out) {
  if (!cbor || !id_out || !next_out) return false;
  cbor::Value top{cbor, cbor + len};
  if (cbor::type(top) != cbor::Type::Map) return false;
  cbor::Value v;
  uint64_t u;
  if (!cbor::map_find(top, "id", &v) || !cbor::as_uint(v, &u) || u > 0xFFFF) {
    return false;
  }
  *id_out = static_cast<uint16_t>(u);
  if (!cbor::map_find(top, "next", &v) || !cbor::as_uint(v, &u) ||
      u > 0xFFFFFFFF) {
    return false;
  }
  *next_out = static_cast<uint32_t>(u);
  return true;
}

size_t bulk_encode_start(uint16_t id, const char* kind, uint32_t size,
                         const uint8_t sha256[32], uint32_t chunk,
                         uint8_t* out, size_t out_cap) {
  if (!out || !kind || !sha256) return 0;
  cbor::Writer w(out, out_cap);
  w.map(5)
      .text("id").uint_v(id)
      .text("kind").text(kind)
      .text("size").uint_v(size)
      .text("sha256").bytes(sha256, 32)
      .text("chunk").uint_v(chunk);
  return w.ok() ? w.size() : 0;
}

size_t bulk_encode_end(uint16_t id, uint8_t* out, size_t out_cap) {
  if (!out) return 0;
  cbor::Writer w(out, out_cap);
  w.map(1).text("id").uint_v(id);
  return w.ok() ? w.size() : 0;
}

void bulk_chunk_head(uint16_t id, uint32_t offset, uint8_t* out) {
  write_le16(out, id);
  write_le32(out + 2, offset);
}

void BulkReceiver::init(const Sink& sink) {
  abort_();
  sink_ = sink;
}

void BulkReceiver::abort_() {
  if (active_ && sink_.abort) sink_.abort(id_, sink_.ctx);
  active_ = false;
}

void BulkReceiver::reset() { abort_(); }

bool BulkReceiver::start(const BulkStart& s, uint32_t* next_out) {
  // 同じ転送の再送なら再開位置を返す (protocol-v1.md の再開手順)。
  if (active_ && s.id == id_ && s.size == size_ &&
      std::memcmp(s.sha256, expected_sha_, sizeof(expected_sha_)) == 0) {
    if (next_out) *next_out = received_;
    return true;
  }

  abort_();
  if (sink_.begin && !sink_.begin(s.id, s.kind, s.size, sink_.ctx)) {
    return false;
  }
  id_ = s.id;
  size_ = s.size;
  received_ = 0;
  chunks_since_ack_ = 0;
  std::memcpy(expected_sha_, s.sha256, sizeof(expected_sha_));
  sha_.reset();
  active_ = true;
  if (next_out) *next_out = 0;
  return true;
}

bool BulkReceiver::feed_chunk(const uint8_t* payload, size_t len,
                              uint32_t* next_out, bool* ack_due) {
  if (ack_due) *ack_due = false;
  if (!active_ || len < 6) return false;

  const uint16_t id = read_le16(payload);
  const uint32_t offset = read_le32(payload + 2);
  if (id != id_) return false;

  const uint8_t* data = payload + 6;
  const size_t data_len = len - 6;

  // オフセットがずれている → 今の受信位置を ACK で知らせて再開してもらう。
  if (offset != received_ || received_ + data_len > size_) {
    if (ack_due) *ack_due = true;
    if (next_out) *next_out = received_;
    return true;
  }

  if (sink_.write && !sink_.write(id_, received_, data, data_len, sink_.ctx)) {
    // 保存失敗: 受理しないので現在位置を ACK で返す。
    if (ack_due) *ack_due = true;
    if (next_out) *next_out = received_;
    return true;
  }

  sha_.update(data, data_len);
  received_ += static_cast<uint32_t>(data_len);
  if (++chunks_since_ack_ >= kBulkAckEvery) {
    chunks_since_ack_ = 0;
    if (ack_due) *ack_due = true;
    if (next_out) *next_out = received_;
  }
  return true;
}

bool BulkReceiver::finish(uint16_t id) {
  // 関係ない転送の END は無視する (進行中の転送は守る)。
  if (!active_ || id != id_) return false;

  // 宣言サイズまで届いていない / hash 不一致 / commit 失敗は全部
  // 「この転送は失敗」として破棄する (中途半端な状態を残さない)。
  bool ok = false;
  if (received_ == size_) {
    uint8_t digest[32];
    sha_.finish(digest);
    ok = std::memcmp(digest, expected_sha_, sizeof(digest)) == 0 &&
         (!sink_.commit || sink_.commit(id_, sink_.ctx));
  }
  if (!ok) {
    abort_();
    return false;
  }
  active_ = false;
  return true;
}

}  // namespace proto
}  // namespace watch
