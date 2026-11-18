/*
 * T49 spike / B-min 用の minimp3 シム。
 *
 * B-min は「返答は字幕のみ・音声再生なし」(SAS-Watch はスピーカー無効) なので、
 * MP3 デコードは不要。SDK の muse_chat_session.cpp は minimp3 の 3 つの API と
 * mp3dec_t / MINIMP3_MAX_SAMPLES_PER_FRAME を参照するだけなので、このヘッダで
 * 差し替えられる (chat 側のソースは無改変)。
 *
 * 実 MP3 デコードが要る場合 (将来 TTS を足す) は SDK の components/minimp3 を
 * 戻す。flash +19,510B、静的 PSRAM +sizeof(mp3dec_t) (実測 +5.4KB)。
 */
#pragma once

#include <stdint.h>
#include <string.h>

#define MINIMP3_MAX_SAMPLES_PER_FRAME (1152 * 2)

typedef int16_t mp3d_sample_t;

/* 本物は数 KB のデコーダ状態。B-min では 32B のダミー。 */
typedef struct {
    int dummy[8];
} mp3dec_t;

typedef struct {
    int frame_bytes;
    int channels;
    int hz;
    int bitrate_kbps;
} mp3dec_frame_info_t;

static inline void mp3dec_init(mp3dec_t *dec)
{
    if (dec) memset(dec, 0, sizeof(*dec));
}

static inline int mp3dec_decode_frame(mp3dec_t *dec, const uint8_t *mp3, int mp3_bytes,
                                      mp3d_sample_t *pcm, mp3dec_frame_info_t *info)
{
    (void)dec;
    (void)mp3;
    (void)mp3_bytes;
    (void)pcm;
    if (info) memset(info, 0, sizeof(*info));
    return 0;   /* フレームなし */
}
