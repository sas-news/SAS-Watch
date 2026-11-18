// muse_link_voice — 音声ノートの WAV/base64 変換 (T54 Phase 2 ④)。
// 純 C / ESP-IDF 非依存。ホストテストは test/test_muse_voice.c。
#include "muse_link/muse_link_voice.h"

#include <string.h>

// SDK の MUSE_HATCH_NOTE_HEAD / MUSE_HATCH_NOTE_TAIL
// (esp32/components/muse/muse_chat_priv.h:81-87) と同じ。
static const char kNotePrefix[] =
    "{\"message\":\"\",\"output_modality\":\"text\",\"device\":\"mcu-wearable\","
    "\"items\":[{\"type\":\"file\","
    "\"mime_type\":\"audio/wav\",\"filename\":\"voice_note.wav\",\"data_base64\":\"";
static const char kNoteSuffix[] = "\"}]}";
static const char kB64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void put_le32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

static void put_le16(uint8_t* p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}

// 44B の RIFF/WAVE/fmt/data。data_bytes / riff_bytes を 0xFFFFFFFF にすると
// SDK と同じ「長さ未確定のストリーミング」ヘッダになる。
static void wav_header(uint8_t h[MUSE_LINK_VOICE_WAV_HEADER_BYTES], uint32_t rate,
                       uint32_t data_bytes, uint32_t riff_bytes)
{
    memcpy(h, "RIFF", 4);
    put_le32(h + 4, riff_bytes);
    memcpy(h + 8, "WAVEfmt ", 8);
    put_le32(h + 16, 16);          // fmt チャンクサイズ
    put_le16(h + 20, 1);           // PCM
    put_le16(h + 22, 1);           // mono
    put_le32(h + 24, rate);
    put_le32(h + 28, rate * 2);    // byte rate = rate * block align
    put_le16(h + 32, 2);           // block align (16bit * 1ch)
    put_le16(h + 34, 16);          // bits per sample
    memcpy(h + 36, "data", 4);
    put_le32(h + 40, data_bytes);
}

static muse_link_voice_status_t check_bytes(size_t bytes)
{
    if (bytes == 0) {
        return MUSE_LINK_VOICE_ERR_EMPTY;
    }
    if (bytes & 1u) {
        return MUSE_LINK_VOICE_ERR_ODD_BYTES;
    }
    if (bytes > MUSE_LINK_VOICE_MAX_PCM_BYTES) {
        return MUSE_LINK_VOICE_ERR_TOO_LONG;
    }
    return MUSE_LINK_VOICE_OK;
}

muse_link_voice_status_t muse_link_voice_validate(const void* pcm, size_t bytes)
{
    if (!pcm) {
        return MUSE_LINK_VOICE_ERR_EMPTY;
    }
    return check_bytes(bytes);
}

muse_link_voice_status_t muse_link_voice_note_plan(size_t pcm_bytes,
                                                   muse_link_voice_sizes_t* out)
{
    if (!out) {
        return MUSE_LINK_VOICE_ERR_BUFFER;
    }
    const muse_link_voice_status_t st = check_bytes(pcm_bytes);
    if (st != MUSE_LINK_VOICE_OK) {
        return st;
    }
    out->wav_bytes = MUSE_LINK_VOICE_WAV_HEADER_BYTES + pcm_bytes;
    out->b64_bytes = muse_link_voice_base64_len(pcm_bytes);
    out->json_bytes = strlen(kNotePrefix) + out->b64_bytes + strlen(kNoteSuffix);
    return MUSE_LINK_VOICE_OK;
}

void muse_link_voice_wav_header(uint8_t out[MUSE_LINK_VOICE_WAV_HEADER_BYTES],
                                uint32_t sample_rate)
{
    // 0xFFFFFFFF = 長さ未確定 (SDK の muse_hatch_wav_header と同じ)。
    wav_header(out, sample_rate, 0xFFFFFFFFu, 0xFFFFFFFFu);
}

void muse_link_voice_wav_header_sized(uint8_t out[MUSE_LINK_VOICE_WAV_HEADER_BYTES],
                                      uint32_t sample_rate, uint32_t pcm_bytes)
{
    // RIFF サイズ = "WAVE" 以降のバイト数 = 36 + data サイズ。
    wav_header(out, sample_rate, pcm_bytes, 36u + pcm_bytes);
}

size_t muse_link_voice_base64_len(size_t bytes)
{
    return ((bytes + 2) / 3) * 4;
}

void muse_link_voice_b64_init(muse_link_voice_b64_t* b64)
{
    if (b64) {
        b64->carry_len = 0;
    }
}

static size_t encode_group(const uint8_t* in, size_t n, char* out)
{
    uint32_t v = (uint32_t)in[0] << 16;
    if (n > 1) {
        v |= (uint32_t)in[1] << 8;
    }
    if (n > 2) {
        v |= in[2];
    }
    out[0] = kB64Alphabet[(v >> 18) & 63];
    out[1] = kB64Alphabet[(v >> 12) & 63];
    out[2] = (n > 1) ? kB64Alphabet[(v >> 6) & 63] : '=';
    out[3] = (n > 2) ? kB64Alphabet[v & 63] : '=';
    return 4;
}

bool muse_link_voice_b64_feed(muse_link_voice_b64_t* b64, const void* pcm, size_t bytes,
                              char* out, size_t out_cap, size_t* written)
{
    if (written) {
        *written = 0;
    }
    if (!b64 || !out || !written || (bytes && !pcm)) {
        return false;
    }

    // 端数 (最大 2B) と新規分を合わせたグループ数で必要量を先に計算する。
    const size_t total = b64->carry_len + bytes;
    const size_t groups = total / 3;
    const size_t need = groups * 4;
    if (out_cap < need) {
        return false;
    }

    size_t o = 0;
    const uint8_t* p = (const uint8_t*)pcm;
    size_t left = bytes;

    if (b64->carry_len) {
        while (b64->carry_len < 3 && left) {
            b64->carry[b64->carry_len++] = *p++;
            --left;
        }
        if (b64->carry_len == 3) {
            o += encode_group(b64->carry, 3, out + o);
            b64->carry_len = 0;
        }
    }
    while (left >= 3) {
        o += encode_group(p, 3, out + o);
        p += 3;
        left -= 3;
    }
    while (left) {
        b64->carry[b64->carry_len++] = *p++;
        --left;
    }
    *written = o;
    return true;
}

bool muse_link_voice_b64_finish(muse_link_voice_b64_t* b64, char* out, size_t out_cap,
                                size_t* written)
{
    if (written) {
        *written = 0;
    }
    if (!b64 || !out || !written || !b64->carry_len) {
        return b64 != NULL && out != NULL && written != NULL;
    }
    if (out_cap < 4) {
        return false;
    }
    *written = encode_group(b64->carry, b64->carry_len, out);
    b64->carry_len = 0;
    return true;
}

size_t muse_link_voice_note_json_prefix(char* out, size_t cap)
{
    const size_t n = strlen(kNotePrefix);
    if (!out || cap < n + 1) {
        return 0;
    }
    memcpy(out, kNotePrefix, n + 1);
    return n;
}

size_t muse_link_voice_note_json_suffix(char* out, size_t cap)
{
    const size_t n = strlen(kNoteSuffix);
    if (!out || cap < n + 1) {
        return 0;
    }
    memcpy(out, kNoteSuffix, n + 1);
    return n;
}
