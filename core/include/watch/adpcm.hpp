// adpcm.hpp — IMA-ADPCM (4bit/sample) エンコーダ/デコーダ。
// 音声メモのコンテナは ADP1: 16バイトの LE ヘッダ + ニブル列。
//   0-3  "ADP1"
//   4    version (=1)
//   5    channels (=1)
//   6    bits/sample (=4)
//   7    reserved (=0)
//   8-11 sample_rate u32 LE (=16000)
//   12-15 num_samples u32 LE (PCM サンプル数)
// ニブルは 1 バイトに2個、下位ニブルが先。エンコーダは 0/pred0 から開始。
// 16kHz mono で 1 秒 ≒ 8000 バイト (WAV の 1/4)。
#pragma once

#include <cstddef>
#include <cstdint>

namespace watch {
namespace audio {

constexpr uint32_t kAdpcmSampleRate = 16000;
constexpr size_t kAdp1HeaderSize = 16;
constexpr size_t kAdpcmBlockPcm = 512;    // 1処理あたりの PCM 目安
constexpr size_t kAdpcmBlockBytes = 256;  // ↑ の ADPCM サイズ

// ADP1 ヘッダを書く (out は 16 バイト以上)。
void adp1_write_header(uint8_t* out, uint32_t samples);

// 先頭が ADP1 なら samples と本文を返す。違う/短いなら false。
bool adp1_parse(const uint8_t* data, size_t len, uint32_t* out_samples,
                const uint8_t** body, size_t* body_len);

// PCM int16 → ADPCM ニブル詰め。ファイル分割して連続呼び出し可。
class ImaAdpcmEncoder {
 public:
  void reset() {
    pred_ = 0;
    index_ = 0;
    have_half_ = false;
    pending_ = 0;
  }
  // pcm を out に圧縮して追記。書いたバイト数を返す。
  // 奇数 nibble は内部に保留する (flush() で確定)。
  size_t encode(const int16_t* pcm, size_t n, uint8_t* out, size_t cap);
  // 保留中の nibble を1バイトで吐き出す (0 or 1)。
  size_t flush(uint8_t* out, size_t cap);

 private:
  uint8_t step(int16_t s);

  int32_t pred_ = 0;
  int32_t index_ = 0;
  bool have_half_ = false;
  uint8_t pending_ = 0;
};

// ADPCM → PCM int16。ファイル分割して連続呼び出し可。
class ImaAdpcmDecoder {
 public:
  void reset() {
    pred_ = 0;
    index_ = 0;
    have_half_ = false;
    cur_ = 0;
  }
  // data のニブルを out の PCM に展開。書いたサンプル数を返す。
  size_t decode(const uint8_t* data, size_t n, int16_t* out, size_t cap);

 private:
  int16_t step(uint8_t nibble);

  int32_t pred_ = 0;
  int32_t index_ = 0;
  bool have_half_ = false;
  uint8_t cur_ = 0;
};

}  // namespace audio
}  // namespace watch
