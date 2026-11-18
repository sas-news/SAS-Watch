// muse_voice_logic — 音声入力 (T57) の純 C 状態機械。
//
// ESP-IDF に依存しない。PCM の蓄積量・経過秒・自動停止・日本語エラーの
// 判定だけを持ち、PSRAM 確保や送信は src/muse_voice_capture.cpp が行う。
// ホストテスト: test/test_muse_voice_capture.c (test/run.sh)。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MUSE_VOICE_ERROR_MAX 96
#define MUSE_VOICE_HARD_MAX_SEC 15       // Muse 側の打ち切り (15 秒)
#define MUSE_VOICE_SAMPLE_RATE 16000     // 16kHz mono PCM16

typedef struct {
    int state;              // muse_link_voice_state_t と同じ値
    uint32_t max_sec;       // 1..15 にクランプ済み
    size_t max_samples;     // max_sec * 16000
    size_t samples;         // 蓄積済みサンプル数
    uint32_t elapsed_s;     // 経過秒 (tick で更新)
    int64_t started_ms;     // 開始時刻 (monotonic ms)
    char error[MUSE_VOICE_ERROR_MAX];
} muse_voice_logic_t;

void muse_voice_logic_init(muse_voice_logic_t* l);
// 開始。max_sec は 1..15 にクランプ。既に RECORDING なら false。
bool muse_voice_logic_begin(muse_voice_logic_t* l, uint32_t max_sec,
                            int64_t now_ms);
// PCM サンプルを積む。容量を超えるなら false (error に日本語を設定)。
bool muse_voice_logic_append(muse_voice_logic_t* l, size_t samples);
// 経過秒を更新する (録音中のみ)。
void muse_voice_logic_tick(muse_voice_logic_t* l, int64_t now_ms);
// 上限に達したか (録音中のみ true)。
bool muse_voice_logic_should_auto_stop(const muse_voice_logic_t* l);
void muse_voice_logic_mark_sending(muse_voice_logic_t* l);
void muse_voice_logic_mark_error(muse_voice_logic_t* l, const char* msg);
// Idle に戻す (エラー文言も消す)。
void muse_voice_logic_reset(muse_voice_logic_t* l);

// 状態名 ("idle"/"recording"/"sending"/"error")。status JSON 用。
const char* muse_voice_state_name(int state);

#ifdef __cplusplus
}
#endif
