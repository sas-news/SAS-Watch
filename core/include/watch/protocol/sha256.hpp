// protocol/sha256.hpp — 最小限の SHA-256 (FIPS 180-4)。
// BULK 転送の完全性検証用。ストリーム処理で大きいデータをバッファ無しで扱う。
// ハード依存なし (mbedtls は使わず core 内で完結させ、ホストテストできる)。
#pragma once

#include <cstddef>
#include <cstdint>

namespace watch {
namespace proto {

class Sha256 {
 public:
  Sha256();

  void reset();
  void update(const uint8_t* data, size_t len);
  // out は 32 バイト。呼んだ時点の状態で確定する (内部状態はリセットされない)。
  void finish(uint8_t out[32]) const;

 private:
  void compress(const uint8_t block[64]);

  uint32_t h_[8];
  uint8_t buf_[64];
  size_t buf_len_ = 0;
  uint64_t total_len_ = 0;  // bit ではなく byte 単位で保持
};

// 一発計算のヘルパ。
void sha256(const uint8_t* data, size_t len, uint8_t out[32]);

}  // namespace proto
}  // namespace watch
