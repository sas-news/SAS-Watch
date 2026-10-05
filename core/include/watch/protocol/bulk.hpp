// protocol/bulk.hpp — protocol-v1.md の BULK_* 転送の受信側ロジック。
// BLE/GATT には一切依存しない。保存先は Sink コールバックで抽象化し、
// ここでは受信順序・再開・SHA-256 検証だけを扱う。
#pragma once

#include <cstddef>
#include <cstdint>

#include "watch/protocol/sha256.hpp"

namespace watch {
namespace proto {

// kind の最大長 (NUL 除く)。"firmware" まで入る 15。
constexpr size_t kBulkKindMax = 15;

// BULK_START payload (CBOR {id, kind, size, sha256, chunk}) のパース結果。
struct BulkStart {
  uint16_t id = 0;
  char kind[kBulkKindMax + 1] = {0};  // "theme" / "asset" / "ota" / "firmware" など
  uint32_t size = 0;
  uint8_t sha256[32] = {0};
  uint32_t chunk = 0;  // 送信側推奨のチャンク長 (情報)
};

// BULK_START payload をパース。必須: id, size, sha256(32B)。形式不正なら false。
bool bulk_parse_start(const uint8_t* cbor, size_t len, BulkStart* out);

// BULK_END payload ({id}) から id を読む。
bool bulk_parse_end(const uint8_t* cbor, size_t len, uint16_t* id_out);

// BULK_ACK payload {id, next} を CBOR で書く。戻り値は長さ (cap 不足なら 0)。
size_t bulk_encode_ack(uint16_t id, uint32_t next, uint8_t* out, size_t out_cap);

// BULK_ACK payload ({id, next}) を読む (送信側が受け取る方)。
bool bulk_parse_ack(const uint8_t* cbor, size_t len, uint16_t* id_out,
                    uint32_t* next_out);

// ---------- 送信側 (時計→スマホ方向も同じ手順) ----------
// BULK_START payload {id, kind, size, sha256, chunk} を CBOR で書く。
// 戻り値は長さ (cap 不足なら 0)。
size_t bulk_encode_start(uint16_t id, const char* kind, uint32_t size,
                         const uint8_t sha256[32], uint32_t chunk,
                         uint8_t* out, size_t out_cap);

// BULK_END payload {id} を CBOR で書く。
size_t bulk_encode_end(uint16_t id, uint8_t* out, size_t out_cap);

// BULK_CHUNK payload の先頭 6 バイト (id:u16 | offset:u32) を書く。
constexpr size_t kBulkChunkHead = 6;
void bulk_chunk_head(uint16_t id, uint32_t offset, uint8_t* out);

// BULK_ACK を送る間隔 (チャンク数)。protocol-v1.md の「8チャンクごと」。
constexpr uint8_t kBulkAckEvery = 8;

// 受信側の状態機械。切断されても状態を保持し、Phone が同じ id で
// BULK_START を再送したら next_offset() から再開できる。
class BulkReceiver {
 public:
  // 保存先。全部 NULL でもよい (その場合受信して検証だけする)。
  // begin/write/commit が false を返したら、その時点の next_offset まで
  // 巻き戻して失敗として扱う (Phone は再送で再開する)。
  struct Sink {
    // 新しい転送の開始。size バイトを受け取る用意が無ければ false。
    bool (*begin)(uint16_t id, const char* kind, uint32_t size,
                  void* ctx) = nullptr;
    // offset への追記。失敗したら false (チャンクは受理しない)。
    bool (*write)(uint16_t id, uint32_t offset, const uint8_t* data, size_t len,
                  void* ctx) = nullptr;
    // sha256 検証OKのあとに呼ぶ (確定処理)。失敗したら false。
    bool (*commit)(uint16_t id, void* ctx) = nullptr;
    // 中断・失敗で転送を捨てるときに呼ぶ (掃除用)。NULL 可。
    void (*abort)(uint16_t id, void* ctx) = nullptr;
    void* ctx = nullptr;
  };

  void init(const Sink& sink);
  // 進行中の転送を捨てる (sink.abort あり)。切断しても呼ばないこと
  // (再接続後の BULK_START 再送で再開するため)。
  void reset();

  bool in_progress() const { return active_; }
  uint16_t current_id() const { return id_; }
  // 次に受け取るべきオフセット = BULK_ACK{next} の値。
  uint32_t next_offset() const { return received_; }

  // BULK_START を処理。true = 受理したので *next_out を ACK で返す。
  //   同じ id・同じ内容で進行中なら再開 (begin は呼ばない)。
  //   別 id なら旧転送を abort して新規開始。
  // false = 拒否 (begin 失敗など)。呼び出し側はエラー応答を返す。
  bool start(const BulkStart& s, uint32_t* next_out);

  // BULK_CHUNK payload (transfer_id:u16 | offset:u32 | bytes) を処理。
  // *ack_due が true なら *next_out を BULK_ACK で返す
  //   (8 チャンクごと / オフセット不整合 / 書き込み失敗の通知)。
  // false = 現在の転送のチャンクではない (無視してよい)。
  bool feed_chunk(const uint8_t* payload, size_t len, uint32_t* next_out,
                  bool* ack_due);

  // BULK_END。受信バイト数と sha256 を検証し、OK なら commit して転送完了。
  // false = 検証失敗・転送不整合 (転送は破棄済み)。呼び出し側はエラー応答。
  bool finish(uint16_t id);

 private:
  void abort_();

  Sink sink_ = {};
  bool active_ = false;
  uint16_t id_ = 0;
  uint32_t size_ = 0;
  uint32_t received_ = 0;
  uint8_t expected_sha_[32] = {0};
  uint8_t chunks_since_ack_ = 0;
  Sha256 sha_;
};

}  // namespace proto
}  // namespace watch
