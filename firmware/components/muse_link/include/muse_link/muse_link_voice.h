// muse_link_voice — 音声ノートの WAV/base64 変換 (T54 Phase 2 ④)。
//
// ESP-IDF に依存しない純 C。ホストテスト (test/test_muse_voice.c) から
// そのままコンパイルできる。実送信 (POST /chat/stream) は Phase 2 ③。
//
// SDK の音声ノート形式 (esp32/components/muse/muse_chat_priv.h:79-87 と
// muse_chat_text.c:41-66 で確認):
//   - 16kHz / 16bit / mono の PCM を WAV (RIFF, 44B ヘッダ) にする
//   - WAV はストリーミング (RIFF/data のサイズを 0xFFFFFFFF) として送り、
//     受信側は data の終端まで読む
//   - それを base64 にして JSON の data_base64 に載せる:
//       {"message":"","output_modality":"text","device":"mcu-wearable",
//        "items":[{"type":"file","mime_type":"audio/wav",
//                  "filename":"voice_note.wav","data_base64":"..."}]}
//
// メモリ方針: 変換は逐次。PCM 全部・base64 全部を一度に持たない
// (60 秒 = 1.92MB を超えたらエラー)。呼び出し側がチャンクごとに feed する。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MUSE_LINK_VOICE_SAMPLE_RATE 16000
#define MUSE_LINK_VOICE_WAV_HEADER_BYTES 44
// 受け付ける音声ノートの上限。Muse は 15 秒で打ち切るが、変換層は安全弁として
// 60 秒 (= 1,920,000B PCM / 2,560,000B base64) まで許す。UI 側は 15 秒を推奨。
#define MUSE_LINK_VOICE_MAX_SECONDS 60
#define MUSE_LINK_VOICE_MAX_SAMPLES (MUSE_LINK_VOICE_SAMPLE_RATE * MUSE_LINK_VOICE_MAX_SECONDS)
#define MUSE_LINK_VOICE_MAX_PCM_BYTES ((size_t)MUSE_LINK_VOICE_MAX_SAMPLES * 2)

typedef enum {
    MUSE_LINK_VOICE_OK = 0,
    MUSE_LINK_VOICE_ERR_EMPTY,      // 0 バイト
    MUSE_LINK_VOICE_ERR_ODD_BYTES,  // PCM16 なのに奇数長
    MUSE_LINK_VOICE_ERR_TOO_LONG,   // 60 秒超
    MUSE_LINK_VOICE_ERR_RATE,       // 16kHz 以外 (ヘッダ生成時)
    MUSE_LINK_VOICE_ERR_BUFFER,     // 出力バッファ不足
} muse_link_voice_status_t;

// PCM の妥当性 (16kHz mono PCM16 前提)。bytes は PCM のバイト数。
muse_link_voice_status_t muse_link_voice_validate(const void* pcm, size_t bytes);

// 必要なバッファサイズの見積り。
typedef struct {
    size_t wav_bytes;   // 44 + pcm_bytes
    size_t b64_bytes;   // ceil(pcm_bytes / 3) * 4 (パディング込み)
    size_t json_bytes;  // prefix + b64_bytes + suffix (NUL は含まない)
} muse_link_voice_sizes_t;

muse_link_voice_status_t muse_link_voice_note_plan(size_t pcm_bytes,
                                                   muse_link_voice_sizes_t* out);

// WAV ヘッダ。ストリーミング用 (RIFF/data = 0xFFFFFFFF) は SDK と同じ。
// 完成したバッファ用に実サイズを書く版もある (ホストテスト/デバッグ用)。
void muse_link_voice_wav_header(uint8_t out[MUSE_LINK_VOICE_WAV_HEADER_BYTES],
                                uint32_t sample_rate);
void muse_link_voice_wav_header_sized(uint8_t out[MUSE_LINK_VOICE_WAV_HEADER_BYTES],
                                      uint32_t sample_rate, uint32_t pcm_bytes);

// base64 (標準アルファベット, 改行なし, 末尾 '=' パディング)。
size_t muse_link_voice_base64_len(size_t bytes);

// 逐次 base64。feed はパディングしない (端数は内部に保持)。
// finish でパディングして終端。out に書いたバイト数を *written に入れる。
typedef struct {
    uint8_t carry[3];
    size_t carry_len;
} muse_link_voice_b64_t;

void muse_link_voice_b64_init(muse_link_voice_b64_t* b64);
bool muse_link_voice_b64_feed(muse_link_voice_b64_t* b64, const void* pcm, size_t bytes,
                              char* out, size_t out_cap, size_t* written);
bool muse_link_voice_b64_finish(muse_link_voice_b64_t* b64, char* out, size_t out_cap,
                                size_t* written);

// voice note の JSON 前後 (SDK の MUSE_HATCH_NOTE_HEAD / TAIL と同じ)。
// 戻り値は書いたバイト数。cap 不足なら 0。
size_t muse_link_voice_note_json_prefix(char* out, size_t cap);
size_t muse_link_voice_note_json_suffix(char* out, size_t cap);

#ifdef __cplusplus
}
#endif
