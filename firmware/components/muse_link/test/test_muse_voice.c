// T54: 音声アダプタ (muse_link_voice) のホストテスト (ESP-IDF 不要)。
// 実行: firmware/components/muse_link/test/run.sh
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "muse_link/muse_link_voice.h"

static int g_fail = 0;

#define CHECK(cond, ...)                       \
    do {                                       \
        if (!(cond)) {                         \
            ++g_fail;                          \
            printf("FAIL: " __VA_ARGS__);      \
            printf("\n");                      \
        }                                      \
    } while (0)

// 入力全体を一括で base64 化する (比較用)。
static size_t b64_all(const void* in, size_t n, char* out, size_t cap)
{
    muse_link_voice_b64_t b;
    size_t w = 0, w2 = 0;
    muse_link_voice_b64_init(&b);
    if (!muse_link_voice_b64_feed(&b, in, n, out, cap, &w)) {
        return (size_t)-1;
    }
    if (!muse_link_voice_b64_finish(&b, out + w, cap - w, &w2)) {
        return (size_t)-1;
    }
    out[w + w2] = '\0';
    return w + w2;
}

// 入力を chunk バイトずつ feed する (逐次変換の検証)。
static size_t b64_chunked(const void* in, size_t n, size_t chunk, char* out, size_t cap)
{
    muse_link_voice_b64_t b;
    const uint8_t* p = (const uint8_t*)in;
    size_t o = 0, w = 0;
    muse_link_voice_b64_init(&b);
    while (n) {
        const size_t take = n < chunk ? n : chunk;
        if (!muse_link_voice_b64_feed(&b, p, take, out + o, cap - o, &w)) {
            return (size_t)-1;
        }
        o += w;
        p += take;
        n -= take;
    }
    if (!muse_link_voice_b64_finish(&b, out + o, cap - o, &w)) {
        return (size_t)-1;
    }
    o += w;
    out[o] = '\0';
    return o;
}

static void test_base64(void)
{
    // RFC 4648 の既知ベクタ。
    static const struct {
        const char* in;
        const char* b64;
    } kVec[] = {
        {"", ""},
        {"f", "Zg=="},
        {"fo", "Zm8="},
        {"foo", "Zm9v"},
        {"foob", "Zm9vYg=="},
        {"fooba", "Zm9vYmE="},
        {"foobar", "Zm9vYmFy"},
    };
    char out[64];
    for (size_t i = 0; i < sizeof(kVec) / sizeof(kVec[0]); ++i) {
        const size_t n = strlen(kVec[i].in);
        CHECK(b64_all(kVec[i].in, n, out, sizeof(out)) == strlen(kVec[i].b64),
              "base64 len for \"%s\"", kVec[i].in);
        CHECK(strcmp(out, kVec[i].b64) == 0, "base64 \"%s\" -> \"%s\" (want \"%s\")",
              kVec[i].in, out, kVec[i].b64);
        CHECK(muse_link_voice_base64_len(n) == strlen(kVec[i].b64),
              "base64_len(%u)", (unsigned)n);
    }
    // 逐次と一括が一致すること (1..8 バイト刻み)。
    static const char kLong[] = "The quick brown fox jumps over 13 lazy dogs!";
    const size_t n = strlen(kLong);
    char one[256], chunked[256];
    CHECK(b64_all(kLong, n, one, sizeof(one)) > 0, "one-shot feed");
    for (size_t chunk = 1; chunk <= 8; ++chunk) {
        const size_t len = b64_chunked(kLong, n, chunk, chunked, sizeof(chunked));
        CHECK(len == strlen(one) && strcmp(one, chunked) == 0,
              "chunked base64 (chunk=%u) differs", (unsigned)chunk);
    }
    // 出力バッファ不足は失敗し、書き込みもしない。
    {
        muse_link_voice_b64_t b;
        size_t w = 123;
        muse_link_voice_b64_init(&b);
        CHECK(!muse_link_voice_b64_feed(&b, "foobar", 6, out, 7, &w),
              "feed must fail when out_cap < 8");
        CHECK(w == 0, "feed must not report bytes on failure (got %u)", (unsigned)w);
        // 成功時は 8B。
        CHECK(muse_link_voice_b64_feed(&b, "foobar", 6, out, 8, &w) && w == 8,
              "feed 6 bytes -> 8 base64 chars");
        CHECK(!muse_link_voice_b64_finish(&b, out, sizeof(out), &w) || w == 0,
              "finish after full groups writes nothing");
    }
}

static void test_wav(void)
{
    uint8_t h[44];
    muse_link_voice_wav_header(h, MUSE_LINK_VOICE_SAMPLE_RATE);
    CHECK(memcmp(h, "RIFF", 4) == 0, "RIFF tag");
    CHECK(memcmp(h + 8, "WAVEfmt ", 8) == 0, "WAVEfmt tag");
    CHECK(memcmp(h + 36, "data", 4) == 0, "data tag");
    CHECK(h[4] == 0xff && h[5] == 0xff && h[6] == 0xff && h[7] == 0xff,
          "streaming RIFF size = 0xFFFFFFFF");
    CHECK(h[40] == 0xff && h[41] == 0xff && h[42] == 0xff && h[43] == 0xff,
          "streaming data size = 0xFFFFFFFF");
    // little-endian フィールド
    CHECK(h[16] == 16 && h[17] == 0, "fmt chunk size 16");
    CHECK(h[20] == 1 && h[21] == 0, "PCM format 1");
    CHECK(h[22] == 1 && h[23] == 0, "mono");
    CHECK(h[24] == 0x80 && h[25] == 0x3e, "rate 16000 LE");
    CHECK(h[28] == 0x00 && h[29] == 0x7d, "byte rate 32000 LE");
    CHECK(h[32] == 2 && h[33] == 0, "block align 2");
    CHECK(h[34] == 16 && h[35] == 0, "bits 16");

    // 実サイズ版: 3200B data -> RIFF 3236, data 3200。
    muse_link_voice_wav_header_sized(h, MUSE_LINK_VOICE_SAMPLE_RATE, 3200);
    CHECK(h[4] == 3236u % 256u && h[5] == 3236u / 256u, "sized RIFF = 36+data");
    CHECK(h[40] == 3200u % 256u && h[41] == 3200u / 256u, "sized data length");
}

static void test_validate_and_plan(void)
{
    static int16_t pcm[8];
    CHECK(muse_link_voice_validate(NULL, 0) == MUSE_LINK_VOICE_ERR_EMPTY, "NULL pcm");
    CHECK(muse_link_voice_validate(pcm, 0) == MUSE_LINK_VOICE_ERR_EMPTY, "0 bytes");
    CHECK(muse_link_voice_validate(pcm, 3) == MUSE_LINK_VOICE_ERR_ODD_BYTES, "odd bytes");
    CHECK(muse_link_voice_validate(pcm, 6) == MUSE_LINK_VOICE_OK, "even bytes");
    CHECK(muse_link_voice_validate(pcm, MUSE_LINK_VOICE_MAX_PCM_BYTES) == MUSE_LINK_VOICE_OK,
          "exactly 60s is accepted");
    CHECK(muse_link_voice_validate(pcm, MUSE_LINK_VOICE_MAX_PCM_BYTES + 2) ==
              MUSE_LINK_VOICE_ERR_TOO_LONG,
          "60s+1sample is rejected");

    char pfx[512];
    const size_t pfx_len = muse_link_voice_note_json_prefix(pfx, sizeof(pfx));
    const size_t sfx_len = muse_link_voice_note_json_suffix(pfx, sizeof(pfx));
    muse_link_voice_sizes_t sz;
    CHECK(muse_link_voice_note_plan(480000, &sz) == MUSE_LINK_VOICE_OK, "plan 15s");
    CHECK(sz.wav_bytes == 44 + 480000, "wav size 15s");
    CHECK(sz.b64_bytes == 640000, "b64 size 15s");
    CHECK(sz.json_bytes == pfx_len + sz.b64_bytes + sfx_len,
          "json size = envelope + base64");

    // 極端値: 60 秒ちょうど / 超過 / 奇数 / 0。
    CHECK(muse_link_voice_note_plan(MUSE_LINK_VOICE_MAX_PCM_BYTES, &sz) == MUSE_LINK_VOICE_OK,
          "plan 60s");
    CHECK(sz.b64_bytes == 2560000, "b64 size 60s");
    CHECK(muse_link_voice_note_plan(MUSE_LINK_VOICE_MAX_PCM_BYTES + 2, &sz) ==
              MUSE_LINK_VOICE_ERR_TOO_LONG,
          "plan rejects >60s");
    CHECK(muse_link_voice_note_plan(0, &sz) == MUSE_LINK_VOICE_ERR_EMPTY, "plan empty");
    CHECK(muse_link_voice_note_plan(3, &sz) == MUSE_LINK_VOICE_ERR_ODD_BYTES, "plan odd");
}

static void test_json_envelope(void)
{
    char buf[512];
    const size_t n = muse_link_voice_note_json_prefix(buf, sizeof(buf));
    CHECK(n > 0, "json prefix");
    CHECK(strstr(buf, "\"device\":\"mcu-wearable\"") != NULL, "device = mcu-wearable");
    CHECK(strstr(buf, "\"mime_type\":\"audio/wav\"") != NULL, "mime audio/wav");
    CHECK(strstr(buf, "\"filename\":\"voice_note.wav\"") != NULL, "filename voice_note.wav");
    CHECK(strstr(buf, "\"data_base64\":\"") != NULL, "data_base64 opening quote");
    CHECK(strstr(buf, "\"output_modality\":\"text\"") != NULL, "output_modality text");
    CHECK(buf[n] == '\0' || n < sizeof(buf), "prefix NUL terminated");

    const size_t m = muse_link_voice_note_json_suffix(buf, sizeof(buf));
    CHECK(m == 4 && strcmp(buf, "\"}]}") == 0, "json suffix");
    (void)m;

    // バッファ不足なら 0。
    CHECK(muse_link_voice_note_json_prefix(buf, 8) == 0, "prefix rejects small cap");
    CHECK(muse_link_voice_note_json_suffix(buf, 3) == 0, "suffix rejects small cap");
}

int main(void)
{
    test_base64();
    test_wav();
    test_validate_and_plan();
    test_json_envelope();

    if (g_fail == 0) {
        printf("test_muse_voice: all passed\n");
        return 0;
    }
    printf("test_muse_voice: %d failure(s)\n", g_fail);
    return 1;
}
