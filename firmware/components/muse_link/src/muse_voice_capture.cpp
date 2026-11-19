// muse_voice_capture.cpp — 音声入力 (T57) の ESP 側実装。
//
// 時計マイクの 16kHz PCM を PSRAM (最大 15 秒 = 480,000B) に蓄積し、
// 停止時に muse_link_send_voice_note() で Muse へ送る。**音声はファイルに
// 保存しない** (PSRAM バッファは開始時に確保し、送信/停止で解放する)。
// 録音の実体 (AudioPort の ADPCM リング読み出し → PCM デコード) は
// watch_app の MusePort アダプタが行い、ここは状態機械と PCM バッファを持つ。
#include "muse_link/muse_link.h"
#include "muse_link_internal.h"
#include "muse_voice_logic.h"

#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

namespace {

constexpr const char* kTag = "muse_link.voice";

muse_voice_logic_t s_logic;
int16_t* s_pcm = nullptr;   // PSRAM, max_sec * 16000 samples
size_t s_pcm_cap = 0;       // サンプル数

int64_t now_ms() { return esp_timer_get_time() / 1000; }

void free_pcm() {
    if (s_pcm) {
        heap_caps_free(s_pcm);
        s_pcm = nullptr;
    }
    s_pcm_cap = 0;
}

// SENDING はセッションのターン完了 (BUSY でなくなった) で Idle/Error に戻す。
void reap_sending() {
    if (s_logic.state != (int)MUSE_LINK_VOICE_SENDING) {
        return;
    }
    const muse_link_session_state_t ss = muse_link_session_state();
    if (ss == MUSE_LINK_SESSION_BUSY) {
        return;
    }
    if (ss == MUSE_LINK_SESSION_ERROR) {
        muse_voice_logic_mark_error(&s_logic, muse_link_last_error());
    } else {
        muse_voice_logic_reset(&s_logic);
    }
}

}  // namespace

extern "C" bool muse_link_voice_start(uint32_t max_sec) {
    if (s_logic.state == (int)MUSE_LINK_VOICE_RECORDING) {
        return false;
    }
    reap_sending();
    if (s_logic.state == (int)MUSE_LINK_VOICE_SENDING) {
        return false;  // 前の送信がまだ終わっていない
    }
    if (muse_link_session_state() != MUSE_LINK_SESSION_READY) {
        muse_voice_logic_mark_error(&s_logic, "Muse に接続してから話してください");
        return false;
    }
    if (max_sec < 1) {
        max_sec = 1;
    }
    if (max_sec > MUSE_VOICE_HARD_MAX_SEC) {
        max_sec = MUSE_VOICE_HARD_MAX_SEC;
    }
    const size_t samples = (size_t)max_sec * MUSE_VOICE_SAMPLE_RATE;
    int16_t* pcm = static_cast<int16_t*>(
        heap_caps_malloc(samples * sizeof(int16_t),
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!pcm) {
        muse_voice_logic_mark_error(
            &s_logic, "PSRAM が不足しています（テーマを軽くしてください）");
        ESP_LOGW(kTag, "voice start: psram alloc failed (%u bytes)",
                 (unsigned)(samples * sizeof(int16_t)));
        return false;
    }
    free_pcm();  // 念のため前回の残りを捨てる
    s_pcm = pcm;
    s_pcm_cap = samples;
    muse_voice_logic_begin(&s_logic, max_sec, now_ms());
    muse_link_wifi_touch();
    ESP_LOGI(kTag, "voice start: max=%us samples=%u psram=%uB", (unsigned)max_sec,
             (unsigned)samples, (unsigned)(samples * sizeof(int16_t)));
    return true;
}

extern "C" bool muse_link_voice_append(const int16_t* pcm, size_t samples) {
    if (!pcm || !s_pcm || s_logic.state != (int)MUSE_LINK_VOICE_RECORDING) {
        return false;
    }
    const size_t before = s_logic.samples;
    if (before + samples > s_pcm_cap) {
        // ここに来る前に app 側が 15 秒で止める想定。念のためエラーにする。
        muse_voice_logic_mark_error(&s_logic, "音声が長すぎます (15 秒まで)");
        return false;
    }
    if (!muse_voice_logic_append(&s_logic, samples)) {
        return false;
    }
    std::memcpy(s_pcm + before, pcm, samples * sizeof(int16_t));
    muse_voice_logic_tick(&s_logic, now_ms());
    muse_link_wifi_touch();
    return true;
}

extern "C" void muse_link_voice_stop(void) {
    if (s_logic.state != (int)MUSE_LINK_VOICE_RECORDING) {
        return;
    }
    muse_voice_logic_tick(&s_logic, now_ms());
    const size_t samples = s_logic.samples;
    if (samples == 0 || !s_pcm) {
        free_pcm();
        muse_voice_logic_mark_error(&s_logic, "音声が取れませんでした");
        return;
    }
    // 録画用 PSRAM バッファの所有権をそのままセッションへ渡す (480KB 二重確保・コピーなし)。
    const bool ok = muse_link_take_voice_note(s_pcm, samples);
    if (ok) {
        s_pcm = nullptr;
        s_pcm_cap = 0;
    } else {
        free_pcm();
        const char* e = muse_link_last_error();
        muse_voice_logic_mark_error(
            &s_logic, (e && *e) ? e : "音声を送信できませんでした");
        return;
    }
    muse_voice_logic_mark_sending(&s_logic);
    ESP_LOGI(kTag, "voice queued: %u samples", (unsigned)samples);
}

extern "C" void muse_link_voice_cancel(void) {
    if (s_logic.state == (int)MUSE_LINK_VOICE_RECORDING) {
        free_pcm();
        muse_voice_logic_reset(&s_logic);
        ESP_LOGI(kTag, "voice cancelled");
    }
}

extern "C" muse_link_voice_state_t muse_link_voice_state(void) {
    reap_sending();
    return (muse_link_voice_state_t)s_logic.state;
}

extern "C" uint32_t muse_link_voice_elapsed_s(void) {
    if (s_logic.state != (int)MUSE_LINK_VOICE_RECORDING) {
        return 0;
    }
    muse_voice_logic_tick(&s_logic, now_ms());
    return s_logic.elapsed_s;
}

extern "C" const char* muse_link_voice_error(void) { return s_logic.error; }
