// Frame エンコード/デコードとフラグメント組み立て。
#include "watch/protocol/frame.hpp"

#include <cstring>

namespace watch {
namespace proto {

namespace {

uint16_t read_le16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

void write_le16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}

}  // namespace

size_t frame_encode(FrameType type, bool more, uint8_t seq, uint16_t msg_id,
                    const uint8_t* payload, size_t payload_len, uint8_t* out,
                    size_t out_cap) {
  const size_t total = kFrameHeaderSize + payload_len + kFrameCrcSize;
  if (payload_len > 0xFFFF || total > out_cap) return 0;
  out[0] = kProtoVer;
  out[1] = static_cast<uint8_t>(type);
  out[2] = more ? kFlagMore : 0;
  out[3] = seq;
  write_le16(out + 4, msg_id);
  write_le16(out + 6, static_cast<uint16_t>(payload_len));
  if (payload_len > 0 && payload) {
    std::memcpy(out + kFrameHeaderSize, payload, payload_len);
  }
  const uint16_t crc = crc16_ccitt_false(out, kFrameHeaderSize + payload_len);
  write_le16(out + kFrameHeaderSize + payload_len, crc);
  return total;
}

FrameError frame_parse(const uint8_t* buf, size_t len, Frame* out) {
  if (len < kFrameHeaderSize + kFrameCrcSize) return FrameError::TooShort;
  if (buf[0] != kProtoVer) return FrameError::BadVersion;
  const size_t payload_len = read_le16(buf + 6);
  if (kFrameHeaderSize + payload_len + kFrameCrcSize != len) {
    return FrameError::BadLength;
  }
  const uint16_t want = read_le16(buf + len - 2);
  const uint16_t got = crc16_ccitt_false(buf, len - kFrameCrcSize);
  if (want != got) return FrameError::BadCrc;
  if (out) {
    out->type = static_cast<FrameType>(buf[1]);
    out->more = (buf[2] & kFlagMore) != 0;
    out->seq = buf[3];
    out->msg_id = read_le16(buf + 4);
    out->payload = buf + kFrameHeaderSize;
    out->payload_len = payload_len;
  }
  return FrameError::Ok;
}

bool Reassembler::feed(const Frame& f, Frame* out) {
  if (error_) return false;
  if (f.seq == 0) {
    // 新しいメッセージ。組み立て途中なら捨てて上書き。
    type_ = f.type;
    msg_id_ = f.msg_id;
    next_seq_ = 0;
    len_ = 0;
    started_ = true;
  }
  if (!started_ || f.type != type_ || f.msg_id != msg_id_ ||
      f.seq != next_seq_) {
    error_ = true;
    return false;
  }
  if (f.payload_len > kMaxMessage - len_) {
    error_ = true;
    return false;
  }
  std::memcpy(buf_ + len_, f.payload, f.payload_len);
  len_ += f.payload_len;
  ++next_seq_;

  if (f.more) return false;  // 続きあり

  // 完成。
  if (out) {
    out->type = type_;
    out->more = false;
    out->seq = next_seq_;
    out->msg_id = msg_id_;
    out->payload = buf_;
    out->payload_len = len_;
  }
  started_ = false;
  next_seq_ = 0;
  return true;
}

void Reassembler::reset() {
  type_ = FrameType::Req;
  msg_id_ = 0;
  next_seq_ = 0;
  len_ = 0;
  started_ = false;
  error_ = false;
}

bool send_message(FrameType type, uint16_t msg_id, const uint8_t* payload,
                  size_t payload_len, size_t mtu,
                  bool (*emit)(const uint8_t* frame, size_t len, void* ctx),
                  void* ctx) {
  if (!emit || mtu <= kFrameHeaderSize + kFrameCrcSize + 3) return false;
  // 1フレームに入る payload の最大 (フレーム全体が MTU-3 まで)。
  const size_t max_frame = mtu - 3;
  const size_t chunk = max_frame - kFrameHeaderSize - kFrameCrcSize;
  uint8_t seq = 0;
  size_t off = 0;
  uint8_t frame_buf[256];  // MTU 247 前提。デカい MTU なら呼び出し側で調整。
  if (max_frame > sizeof(frame_buf)) return false;
  do {
    const size_t n = payload_len - off;
    const size_t take = n > chunk ? chunk : n;
    const bool more = (off + take) < payload_len;
    const size_t flen =
        frame_encode(type, more, seq, msg_id, payload + off, take, frame_buf,
                     sizeof(frame_buf));
    if (flen == 0) return false;
    if (!emit(frame_buf, flen, ctx)) return false;
    off += take;
    ++seq;
    if (seq == 0) return false;  // 255 フラグメント以上は扱わない
  } while (off < payload_len);
  return true;
}

}  // namespace proto
}  // namespace watch
