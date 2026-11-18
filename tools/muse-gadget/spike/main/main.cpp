/*
 * T49 spike main。
 *
 * 段階:
 *   S0: noise_core の ClientSession / PsaCryptoBackend を実体化してリンクできるか。
 *   S1: SDK の chat 層 (muse_chat.c / muse_chat_session.cpp / ...) をリンクできるか。
 *
 * 実機で動かすためのコードではない。リンカがオブジェクトを落とさないように
 * 主要 API を参照するだけ (muse_hatch_ready() はトークン未設定なので false を返し、
 * 実際の通信は起きない)。
 */
#include <stdio.h>
#include <stdlib.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"

#if CONFIG_SPIKE_WITH_CHAT
#include "muse_chat.h"
#endif

#include "xplat/noise/core/ClientSession.h"
#include "xplat/noise/core/PsaCryptoBackend.h"

using namespace musegadgets::noise::core;

static const char *TAG = "spike";

/* noise_core をリンクさせるための最小の参照。 */
static void spike_noise(void)
{
    static PsaCryptoBackend crypto;   /* 大きさは数百 B 程度 */
    static ClientSession session(crypto);

    const size_t cap = ClientSession::kMaxOutboundWebSocketPayloadSize;
    uint8_t *buf = static_cast<uint8_t *>(malloc(cap));
    if (!buf) {
        ESP_LOGE(TAG, "no buffer (%u bytes)", (unsigned)cap);
        return;
    }
    auto m1 = session.WriteHandshakeMessage1(ByteSpan(buf, cap));
    ESP_LOGI(TAG, "noise handshake msg1: ok=%d size=%u (buf %u)",
             (int)m1.ok(), (unsigned)m1.size(), (unsigned)cap);
    free(buf);
}

/* chat 層をリンクさせるための参照。ready でないので通信は起きない。 */
#if CONFIG_SPIKE_WITH_CHAT
static void spike_chat(void)
{
    muse_hatch_start();

    muse_hatch_status_t st;
    muse_hatch_status(&st);
    ESP_LOGI(TAG, "chat: state=%s detail=%s vm=%s", muse_hatch_state_name(st.state),
             st.detail, st.vm_id);

    muse_hatch_test();
    muse_hatch_config_changed();
    muse_hatch_set_resting(true);

    char text[96] = {0};
    muse_hatch_ev_t ev = muse_hatch_turn_event(text, sizeof(text));
    ESP_LOGI(TAG, "chat: event=%d", (int)ev);

    char caption[96] = {0};
    (void)muse_hatch_turn_caption(0, caption, sizeof(caption));
    int message = -1;
    (void)muse_turn_progress(0, &message);
    (void)muse_hatch_incoming();
    muse_chat_set_caption_px(320, nullptr);

    if (muse_hatch_ready()) {
        /* トークン未設定なのでここには入らない。参照だけ残す。 */
        int16_t pcm[320] = {0};
        muse_hatch_turn_begin();
        muse_hatch_turn_audio(pcm, 320);
        muse_hatch_turn_end();
        muse_hatch_turn_cancel();
        (void)muse_hatch_turn_audio_wait(pcm, 320, 0);
        (void)muse_hatch_turn_read(pcm, 320, 0);
        char *typed = static_cast<char *>(malloc(8));
        if (typed) {
            snprintf(typed, 8, "hello");
            muse_hatch_text_turn(typed);
        }
        int16_t *mp3 = nullptr;
        (void)muse_hatch_mp3_selftest(&mp3);
        free(mp3);
    }
}
#endif

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "muse chat spike: noise_core + chat layer");
    spike_noise();
#if CONFIG_SPIKE_WITH_CHAT
    spike_chat();
#endif
    ESP_LOGI(TAG, "internal free=%u psram free=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    ESP_LOGI(TAG, "done");
}
