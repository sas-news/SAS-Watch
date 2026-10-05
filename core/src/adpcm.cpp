// IMA-ADPCM コーデック。テーブルは標準の 89 段ステップ。
#include "watch/adpcm.hpp"

#include <cstring>

namespace watch {
namespace audio {
namespace {

const int16_t kStepTable[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,
    19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
    337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
    876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
    5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};

const int8_t kIndexTable[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8,
};

void put_u32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
  p[2] = static_cast<uint8_t>(v >> 16);
  p[3] = static_cast<uint8_t>(v >> 24);
}

uint32_t get_u32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) |
         (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

}  // namespace

// ---------- ADP1 ----------

void adp1_write_header(uint8_t* out, uint32_t samples) {
  std::memcpy(out, "ADP1", 4);
  out[4] = 1;   // version
  out[5] = 1;   // channels
  out[6] = 4;   // bits/sample
  out[7] = 0;
  put_u32(out + 8, kAdpcmSampleRate);
  put_u32(out + 12, samples);
}

bool adp1_parse(const uint8_t* data, size_t len, uint32_t* out_samples,
                const uint8_t** body, size_t* body_len) {
  if (!data || len < kAdp1HeaderSize) return false;
  if (std::memcmp(data, "ADP1", 4) != 0) return false;
  if (data[4] != 1 || data[5] != 1 || data[6] != 4) return false;
  if (get_u32(data + 8) != kAdpcmSampleRate) return false;
  if (out_samples) *out_samples = get_u32(data + 12);
  if (body) *body = data + kAdp1HeaderSize;
  if (body_len) *body_len = len - kAdp1HeaderSize;
  return true;
}

// ---------- encoder ----------

uint8_t ImaAdpcmEncoder::step(int16_t s) {
  int diff = static_cast<int>(s) - pred_;
  uint8_t nib = 0;
  if (diff < 0) {
    nib = 8;
    diff = -diff;
  }
  int step = kStepTable[index_];
  int vpdiff = step >> 3;
  if (diff >= step) {
    nib |= 4;
    diff -= step;
    vpdiff += step;
  }
  step >>= 1;
  if (diff >= step) {
    nib |= 2;
    diff -= step;
    vpdiff += step;
  }
  step >>= 1;
  if (diff >= step) {
    nib |= 1;
    vpdiff += step;
  }
  pred_ += (nib & 8) ? -vpdiff : vpdiff;
  if (pred_ > 32767) pred_ = 32767;
  if (pred_ < -32768) pred_ = -32768;
  index_ += kIndexTable[nib];
  if (index_ < 0) index_ = 0;
  if (index_ > 88) index_ = 88;
  return nib;
}

size_t ImaAdpcmEncoder::encode(const int16_t* pcm, size_t n, uint8_t* out,
                               size_t cap) {
  size_t w = 0;
  for (size_t i = 0; i < n; ++i) {
    const uint8_t nib = step(pcm[i]);
    if (!have_half_) {
      pending_ = nib;
      have_half_ = true;
    } else {
      if (w >= cap) break;
      out[w++] = static_cast<uint8_t>(pending_ | (nib << 4));
      have_half_ = false;
    }
  }
  return w;
}

size_t ImaAdpcmEncoder::flush(uint8_t* out, size_t cap) {
  if (!have_half_ || cap == 0) return 0;
  out[0] = pending_;
  have_half_ = false;
  return 1;
}

// ---------- decoder ----------

int16_t ImaAdpcmDecoder::step(uint8_t nib) {
  const int step = kStepTable[index_];
  int vpdiff = step >> 3;
  if (nib & 4) vpdiff += step;
  if (nib & 2) vpdiff += step >> 1;
  if (nib & 1) vpdiff += step >> 2;
  pred_ += (nib & 8) ? -vpdiff : vpdiff;
  if (pred_ > 32767) pred_ = 32767;
  if (pred_ < -32768) pred_ = -32768;
  index_ += kIndexTable[nib & 0x0F];
  if (index_ < 0) index_ = 0;
  if (index_ > 88) index_ = 88;
  return static_cast<int16_t>(pred_);
}

size_t ImaAdpcmDecoder::decode(const uint8_t* data, size_t n, int16_t* out,
                               size_t cap) {
  size_t w = 0;
  for (size_t i = 0; i < n; ++i) {
    cur_ = data[i];
    have_half_ = true;
    if (w < cap) out[w++] = step(cur_ & 0x0F);
    if (w < cap) out[w++] = step(cur_ >> 4);
    have_half_ = false;
  }
  return w;
}

}  // namespace audio
}  // namespace watch
