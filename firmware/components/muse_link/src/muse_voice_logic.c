// muse_voice_logic.c — 音声入力の純 C 状態機械 (ESP-IDF 非依存)。
#include "muse_voice_logic.h"

#include <stdio.h>
#include <string.h>

#include "muse_link/muse_link.h"  // muse_link_voice_state_t

static void set_error(muse_voice_logic_t* l, const char* msg)
{
    if (!l) {
        return;
    }
    snprintf(l->error, sizeof(l->error), "%s", msg ? msg : "");
}

void muse_voice_logic_init(muse_voice_logic_t* l)
{
    if (!l) {
        return;
    }
    memset(l, 0, sizeof(*l));
    l->state = (int)MUSE_LINK_VOICE_IDLE;
}

bool muse_voice_logic_begin(muse_voice_logic_t* l, uint32_t max_sec,
                            int64_t now_ms)
{
    if (!l) {
        return false;
    }
    if (l->state == (int)MUSE_LINK_VOICE_RECORDING ||
        l->state == (int)MUSE_LINK_VOICE_SENDING) {
        return false;
    }
    if (max_sec < 1) {
        max_sec = 1;
    }
    if (max_sec > MUSE_VOICE_HARD_MAX_SEC) {
        max_sec = MUSE_VOICE_HARD_MAX_SEC;
    }
    l->state = (int)MUSE_LINK_VOICE_RECORDING;
    l->max_sec = max_sec;
    l->max_samples = (size_t)max_sec * MUSE_VOICE_SAMPLE_RATE;
    l->samples = 0;
    l->elapsed_s = 0;
    l->started_ms = now_ms;
    l->error[0] = '\0';
    return true;
}

bool muse_voice_logic_append(muse_voice_logic_t* l, size_t samples)
{
    if (!l || l->state != (int)MUSE_LINK_VOICE_RECORDING || samples == 0) {
        return false;
    }
    if (l->samples + samples > l->max_samples) {
        set_error(l, "音声が長すぎます (15 秒まで)");
        return false;
    }
    l->samples += samples;
    return true;
}

void muse_voice_logic_tick(muse_voice_logic_t* l, int64_t now_ms)
{
    if (!l || l->state != (int)MUSE_LINK_VOICE_RECORDING) {
        return;
    }
    int64_t ms = now_ms - l->started_ms;
    if (ms < 0) {
        ms = 0;
    }
    uint32_t s = (uint32_t)(ms / 1000);
    if (s > l->max_sec) {
        s = l->max_sec;
    }
    l->elapsed_s = s;
}

bool muse_voice_logic_should_auto_stop(const muse_voice_logic_t* l)
{
    return l && l->state == (int)MUSE_LINK_VOICE_RECORDING &&
           l->elapsed_s >= l->max_sec;
}

void muse_voice_logic_mark_sending(muse_voice_logic_t* l)
{
    if (!l) {
        return;
    }
    l->state = (int)MUSE_LINK_VOICE_SENDING;
    l->elapsed_s = 0;
}

void muse_voice_logic_mark_error(muse_voice_logic_t* l, const char* msg)
{
    if (!l) {
        return;
    }
    l->state = (int)MUSE_LINK_VOICE_ERROR;
    l->elapsed_s = 0;
    set_error(l, msg ? msg : "音声入力を失敗しました");
}

void muse_voice_logic_reset(muse_voice_logic_t* l)
{
    if (!l) {
        return;
    }
    l->state = (int)MUSE_LINK_VOICE_IDLE;
    l->samples = 0;
    l->elapsed_s = 0;
    l->error[0] = '\0';
}

const char* muse_voice_state_name(int state)
{
    switch ((muse_link_voice_state_t)state) {
        case MUSE_LINK_VOICE_RECORDING:
            return "recording";
        case MUSE_LINK_VOICE_SENDING:
            return "sending";
        case MUSE_LINK_VOICE_ERROR:
            return "error";
        case MUSE_LINK_VOICE_IDLE:
        default:
            return "idle";
    }
}
