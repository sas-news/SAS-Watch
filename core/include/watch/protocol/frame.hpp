// protocol/frame.hpp — protocol-v1.md のフレーム。
//   ver:1 | type:1 | flags:1 | seq:1 | msg_id:2 | len:2 | payload | crc16:2
// crc16 = CRC-16/CCITT-FALSE (0x1021, init 0xFFFF) over ver..payload。
#pragma once

#include <cstddef>
#include <cstdint>

namespace watch {
namespace proto {

constexpr uint8_t kProtoVer = 1;

enum class FrameType : uint8_t {
  Req = 0x01,
  Res = 0x02,
  Evt = 0x03,
  BulkStart = 0x10,
  BulkChunk = 0x11,
  BulkAck = 0x12,
  BulkEnd = 0x13,
};

constexpr uint8_t kFlagMore = 0x01;      // フラグメント続き
constexpr size_t kFrameHeaderSize = 8;   // ver..len
constexpr size_t kFrameCrcSize = 2;
constexpr size_t kDefaultMtu = 247;      // ATT MTU 要求値 (payload は MTU-3)

// 組み立てるメッセージの最大サイズ。CBOR の REQ/RES/EVT と BULK の
// 1論理メッセージを想定。大きい転送は BULK_CHUNK ごとに処理する。
constexpr size_t kMaxMessage = 1024;

struct Frame {
  FrameType type;
  bool more;  // flags bit0
  uint8_t seq;
  uint16_t msg_id;
  const uint8_t* payload;  // buf 内を指す (コピーしない)
  size_t payload_len;
};

uint16_t crc16_ccitt_false(const uint8_t* data, size_t len);

// 1フレームをエンコードする。out_cap 不足なら 0。
size_t frame_encode(FrameType type, bool more, uint8_t seq, uint16_t msg_id,
                    const uint8_t* payload, size_t payload_len, uint8_t* out,
                    size_t out_cap);

// 1フレームを検証して Frame に展開する。
enum class FrameError : uint8_t {
  Ok = 0,
  TooShort,
  BadVersion,
  BadLength,
  BadCrc,
};
FrameError frame_parse(const uint8_t* buf, size_t len, Frame* out);

// 受信側のフラグメント組み立て機。1メッセージずつ直列に扱う
// (GATT の書き込みは順序付きという前提)。
class Reassembler {
 public:
  // 検証済み Frame を1つ渡す。メッセージ完成なら true で out に返す。
  // 続きがあれば false。エラー (seq 不連続・型不一致・容量超過) は error()。
  bool feed(const Frame& f, Frame* out);
  void reset();
  bool error() const { return error_; }
  bool in_progress() const { return len_ > 0 || started_; }

 private:
  FrameType type_ = FrameType::Req;
  uint16_t msg_id_ = 0;
  uint8_t next_seq_ = 0;
  uint8_t buf_[kMaxMessage] = {};
  size_t len_ = 0;
  bool started_ = false;
  bool error_ = false;
};

// 送信側: payload を MTU に収まるようにフラグメント化して emit する。
// emit(frame_buf, frame_len, ctx) が false を返したら中止して false。
bool send_message(FrameType type, uint16_t msg_id, const uint8_t* payload,
                  size_t payload_len, size_t mtu,
                  bool (*emit)(const uint8_t* frame, size_t len, void* ctx),
                  void* ctx);

}  // namespace proto
}  // namespace watch
