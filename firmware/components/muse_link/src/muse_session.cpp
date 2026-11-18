// muse_session — Muse セッションの起動/停止/送受信 (T55 Phase 2 ③)。
//
// 役割:
//   - 資格情報 (T52) / Wi-Fi on-demand (T54) / SDK chat 層をつないでセッションを開始
//   - 16kHz PCM の音声ノートとテキストを送り、返答を 1 本のバッファに貯める
//   - 状態機械・開始前チェック・status JSON は src/session_logic.c (純 C,
//     ホストテスト対象)
//
// 実行モデル:
//   - SDK のセッションタスク (muse_hatch_start, 48KB PSRAM スタック) が
//     TLS/Noise/受信を回す。
//   - こちらは「voice task」役の内蔵 SRAM タスク (8KB, core0) を 1 本だけ作る。
//     起動シーケンス (Wi-Fi ensure → PSRAM 確保 → SDK 起動) もこのタスクで行い、
//     console (4KB スタック) や LVGL/app タスクをブロックしない。API は開始要求を
//     受けて即返り、成否は session state と last_error で分かる。
//   - TLS ハンドシェイクをこのタスクで走らせない: ターン開始前に
//     muse_hatch_status() が REACHABLE であることを確認する (未接続なら
//     muse_hatch_chat_connect() で SDK タスクに接続させるだけ)。切断直後に
//     turn_begin が ensure_connected に入る競合は残るため 8KB 固定。
//     TODO(hw): 実機でクラッシュ有無を確認。足りなければ PSRAM スタック化。
//
// 秘密: token / 返答本文はログに出さない (長さだけ)。返答は PSRAM バッファに
// 保持し、UI (Phase 2 ⑤) と muse_link_last_reply() だけが読む。
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "muse_chat.h"
#include "muse_chat_priv.h"  // muse_hatch_chat_connect (SDK 内部 API)
#include "muse_link/muse_link.h"
#include "muse_link/muse_link_voice.h"
#include "muse_link_internal.h"
#include "session_logic.h"
#include "wifi/wifi.hpp"

namespace {

constexpr const char* kTag = "muse_link";

// 返答の保持上限 (PSRAM, 開始時確保)。UI は 960B ページングだが、ここでは
// 生テキストを保持する (Phase 2 ⑤ でページング)。
constexpr size_t kReplyCap = 4096;
// 音声ノートの上限。Muse は 15 秒で打ち切るので 15 秒 (= 480,000B) に固定。
constexpr uint32_t kVoiceMaxSeconds = 15;
constexpr size_t kVoiceMaxSamples = MUSE_LINK_VOICE_SAMPLE_RATE * kVoiceMaxSeconds;
// タスクスタック (内蔵 SRAM, 起動時確保)。
constexpr uint32_t kTaskStackBytes = 8192;
constexpr UBaseType_t kTaskPrio = 3;
constexpr BaseType_t kTaskCore = 0;  // BLE / SDK セッションと同じ core0
// タイムアウト (ms)
constexpr int64_t kConnectTimeoutMs = 30000;
constexpr int64_t kTurnTimeoutMs = 90000;       // 音声ノートの返答待ち
constexpr int64_t kTextTurnTimeoutMs = 300000;  // テキストの返答待ち (5 分)

#ifndef CONFIG_WATCH_MUSE_MIN_INTERNAL_KB
#define CONFIG_WATCH_MUSE_MIN_INTERNAL_KB 12
#endif
#ifndef CONFIG_WATCH_MUSE_MIN_PSRAM_KB
#define CONFIG_WATCH_MUSE_MIN_PSRAM_KB 1400
#endif
constexpr size_t kMinInternalBytes = (size_t)CONFIG_WATCH_MUSE_MIN_INTERNAL_KB * 1024u;
constexpr size_t kMinPsramBytes = (size_t)CONFIG_WATCH_MUSE_MIN_PSRAM_KB * 1024u;

// ---- 状態 ----
std::atomic<int> s_state{(int)MUSE_LINK_SESSION_OFF};
SemaphoreHandle_t s_reply_mtx = nullptr;
SemaphoreHandle_t s_task_done = nullptr;
TaskHandle_t s_task = nullptr;
std::atomic<bool> s_task_alive{false};
volatile bool s_run = false;
volatile bool s_stop_requested = false;

char* s_reply = nullptr;         // PSRAM, kReplyCap
size_t s_reply_len = 0;          // 保持している長さ
size_t s_reply_total = 0;        // 受信した総バイト数
int16_t* s_voice_pcm = nullptr;  // PSRAM, kVoiceMaxSamples
size_t s_voice_samples = 0;
bool s_voice_pending = false;
bool s_voice_active = false;
bool s_text_active = false;
int64_t s_deadline_ms = 0;
int64_t s_starting_since_ms = 0;
bool s_configured = false;

char s_error[96];
muse_link_reply_cb s_reply_cb = nullptr;
void* s_reply_cb_ctx = nullptr;

int64_t now_ms() {
    return esp_timer_get_time() / 1000;
}

void set_state(muse_link_session_state_t st) {
    const int prev = s_state.exchange((int)st);
    if (prev != (int)st) {
        ESP_LOGI(kTag, "session: %s -> %s",
                 muse_link_session_state_name((muse_link_session_state_t)prev),
                 muse_link_session_state_name(st));
    }
}

void set_error_text(const char* text) {
    if (!text) {
        s_error[0] = '\0';
    } else {
        strncpy(s_error, text, sizeof(s_error) - 1);
        s_error[sizeof(s_error) - 1] = '\0';
    }
    ESP_LOGW(kTag, "session error (%u bytes)", (unsigned)strlen(s_error));
    set_state(MUSE_LINK_SESSION_ERROR);
}

void set_start_error(muse_link_start_error_t err) {
    set_error_text(muse_link_session_error_text(err));
    ESP_LOGW(kTag, "start refused: %s", muse_link_session_error_text(err));
}

void advance(muse_session_event_t ev) {
    bool changed = false;
    const muse_link_session_state_t next = muse_link_session_advance(
        (muse_link_session_state_t)s_state.load(), ev, &changed);
    if (changed) {
        set_state(next);
    }
}

// ---- 返答バッファ ----
void reply_reset() {
    if (!s_reply_mtx) {
        return;
    }
    xSemaphoreTake(s_reply_mtx, portMAX_DELAY);
    s_reply_len = 0;
    s_reply_total = 0;
    if (s_reply) {
        s_reply[0] = '\0';
    }
    xSemaphoreGive(s_reply_mtx);
}

// 返答の断片を追記する (SDK タスクから呼ばれる)。
void reply_append(const char* text) {
    if (!text || !*text || !s_reply_mtx) {
        return;
    }
    const size_t n = strlen(text);
    xSemaphoreTake(s_reply_mtx, portMAX_DELAY);
    s_reply_total += n;
    if (s_reply && s_reply_len + n < kReplyCap) {
        memcpy(s_reply + s_reply_len, text, n);
        s_reply_len += n;
        s_reply[s_reply_len] = '\0';
    }
    xSemaphoreGive(s_reply_mtx);
}

void reply_finish() {
    if (s_reply_cb) {
        s_reply_cb(s_reply_total, s_reply_cb_ctx);
    }
    ESP_LOGI(kTag, "reply done (%u bytes, kept %u)", (unsigned)s_reply_total,
             (unsigned)s_reply_len);
}

void free_buffers() {
    if (s_voice_pcm) {
        heap_caps_free(s_voice_pcm);
        s_voice_pcm = nullptr;
    }
    if (s_reply) {
        heap_caps_free(s_reply);
        s_reply = nullptr;
    }
    if (s_reply_mtx) {
        vSemaphoreDelete(s_reply_mtx);
        s_reply_mtx = nullptr;
    }
    s_reply_len = 0;
    s_reply_total = 0;
    s_voice_pending = false;
    s_voice_active = false;
    s_text_active = false;
    s_voice_samples = 0;
}

// ---- 音声ノートの送出 (このタスクで TLS を走らせない) ----
bool send_voice_turn() {
    muse_hatch_status_t st;
    muse_hatch_status(&st);
    if (st.state != MUSE_HATCH_REACHABLE) {
        return false;  // 接続待ち
    }
    reply_reset();
    muse_hatch_turn_begin();
    size_t off = 0;
    while (off < s_voice_samples && s_run) {
        const size_t take = (s_voice_samples - off) < 320 ? (s_voice_samples - off) : 320;
        muse_hatch_turn_audio(s_voice_pcm + off, take);
        off += take;
    }
    muse_hatch_turn_end();
    s_voice_active = true;
    s_deadline_ms = now_ms() + kTurnTimeoutMs;
    ESP_LOGI(kTag, "voice note sent (%u samples)", (unsigned)s_voice_samples);
    return true;
}

// ---- セッションループ (起動タスクが start_sequence の後に走らせる) ----
void session_loop() {
    while (s_run) {
        const muse_link_session_state_t st = (muse_link_session_state_t)s_state.load();

        // 1) 音声ノートの送信要求
        if (s_voice_pending && !s_voice_active && send_voice_turn()) {
            s_voice_pending = false;
        }

        // 2) SDK からのイベント (音声ターンの字幕/エラー/完了)
        if (s_voice_active) {
            char text[96];
            for (;;) {
                const muse_hatch_ev_t ev = muse_hatch_turn_event(text, sizeof(text));
                if (ev == MUSE_HATCH_EV_NONE) {
                    break;
                }
                if (ev == MUSE_HATCH_EV_REPLY) {
                    reply_append(text);
                } else if (ev == MUSE_HATCH_EV_DONE) {
                    s_voice_active = false;
                    if (text[0]) {
                        set_error_text("Muse の返答が不完全でした");
                    } else {
                        reply_finish();
                        advance(MUSE_SESSION_EV_TURN_DONE);
                    }
                } else if (ev == MUSE_HATCH_EV_ERROR) {
                    s_voice_active = false;
                    set_error_text("Muse に接続できません");
                    advance(MUSE_SESSION_EV_TURN_ERROR);
                }
            }
        }

        // 3) SDK のセッション状態を FSM に反映
        {
            muse_hatch_status_t hs;
            muse_hatch_status(&hs);
            if (hs.state == MUSE_HATCH_REACHABLE) {
                if (st == MUSE_LINK_SESSION_STARTING || st == MUSE_LINK_SESSION_ERROR) {
                    advance(MUSE_SESSION_EV_CONNECTED);
                }
            } else if (hs.state == MUSE_HATCH_UNREACHABLE || hs.state == MUSE_HATCH_OFFLINE ||
                       hs.state == MUSE_HATCH_NOT_SET) {
                if (st == MUSE_LINK_SESSION_STARTING || st == MUSE_LINK_SESSION_READY ||
                    st == MUSE_LINK_SESSION_BUSY) {
                    set_error_text("Muse に接続できません");
                    advance(MUSE_SESSION_EV_DISCONNECTED);
                }
            }
        }

        // 4) 開始タイムアウト (SDK の allocate 失敗などで STARTING のまま)
        if (st == MUSE_LINK_SESSION_STARTING) {
            if (s_starting_since_ms == 0) {
                s_starting_since_ms = now_ms();
            } else if (now_ms() - s_starting_since_ms > kConnectTimeoutMs) {
                set_error_text("Muse セッションを開始できません");
            }
        } else {
            s_starting_since_ms = 0;
        }

        // 5) ターンのタイムアウト
        if ((s_voice_active || s_text_active) && s_deadline_ms && now_ms() > s_deadline_ms) {
            if (s_voice_active) {
                muse_hatch_turn_cancel();
                s_voice_active = false;
            }
            s_text_active = false;
            set_error_text("Muse の返答がタイムアウトしました");
            advance(MUSE_SESSION_EV_TURN_ERROR);
        }

        // 6) Wi-Fi アイドル管理 (T54)
        muse_link_wifi_poll();

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

}  // namespace

// ---- 起動シーケンス / タスク終了 (グローバルスコープ) ----
static void start_sequence(uint32_t wifi_timeout_ms);
static void start_task(void* arg);

static void task_exit(bool keep_error) {
    if (!keep_error) {
        set_error_text("");
    }
    s_task_alive.store(false);
    muse_hatch_set_resting(true);
    muse_hatch_config_changed();
    muse_link_wifi_release();
    free_buffers();
    if (!keep_error) {
        set_state(MUSE_LINK_SESSION_OFF);
    }
    if (s_task_done) {
        xSemaphoreGive(s_task_done);
    }
    vTaskDelete(nullptr);
}

static void fail_start(muse_link_start_error_t err) {
    s_run = false;
    set_start_error(err);
}

static void start_sequence(uint32_t wifi_timeout_ms) {
    char host[64], vm[64];
    size_t token_len = 0;
    muse_link_cfg_get(host, sizeof(host), vm, sizeof(vm), &token_len);
    s_configured = token_len > 0 && host[0] != '\0';

    const size_t int_largest =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t psram_largest =
        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGI(kTag,
             "start: configured=%d wifi_configured=%d int_largest=%u psram_largest=%u "
             "(min int=%u psram=%u)",
             (int)s_configured, (int)wifi::configured(), (unsigned)int_largest,
             (unsigned)psram_largest, (unsigned)kMinInternalBytes, (unsigned)kMinPsramBytes);

    const muse_link_start_error_t check = muse_link_session_check_start(
        s_configured, wifi::configured(), int_largest, psram_largest, kMinInternalBytes,
        kMinPsramBytes);
    if (check != MUSE_LINK_START_OK) {
        fail_start(check);
        return;
    }

    if (!muse_link_wifi_ensure(wifi_timeout_ms)) {
        fail_start(MUSE_LINK_START_ERR_WIFI_FAILED);
        return;
    }
    if (s_stop_requested) {
        muse_link_wifi_release();
        return;
    }

    s_reply = static_cast<char*>(heap_caps_malloc(kReplyCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_voice_pcm = static_cast<int16_t*>(
        heap_caps_malloc(kVoiceMaxSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_reply_mtx = xSemaphoreCreateMutex();
    if (!s_reply || !s_voice_pcm || !s_reply_mtx) {
        fail_start(MUSE_LINK_START_ERR_NO_PSRAM);
        return;
    }
    s_reply[0] = '\0';

    s_starting_since_ms = now_ms();
    set_state(MUSE_LINK_SESSION_STARTING);
    if (s_stop_requested) {
        return;
    }
    // SDK のセッションタスクを起動し、VM 接続を促す (接続は SDK タスク側で走る)。
    muse_hatch_start();
    muse_hatch_set_resting(false);
    muse_hatch_chat_connect();
    ESP_LOGI(kTag, "session starting (wifi owned=%d)", (int)muse_link_wifi_owned());
}

static void start_task(void* arg) {
    const uint32_t wifi_timeout_ms =
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
    start_sequence(wifi_timeout_ms);
    if (!s_run || s_stop_requested) {
        // 失敗 (last_error 設定済み) または stop。エラーは保持する。
        task_exit(s_state.load() == (int)MUSE_LINK_SESSION_ERROR);
        return;
    }
    session_loop();
    task_exit(false);
}

// ---- 公開 API ----

extern "C" bool muse_link_start(uint32_t wifi_timeout_ms) {
    const int st = s_state.load();
    if (st == (int)MUSE_LINK_SESSION_STARTING || st == (int)MUSE_LINK_SESSION_READY ||
        st == (int)MUSE_LINK_SESSION_BUSY || s_task_alive.load()) {
        ESP_LOGW(kTag, "start ignored: already running (%s)",
                 muse_link_session_state_name((muse_link_session_state_t)st));
        return false;
    }

    // 前回の停止で残ったセマフォを片付ける。
    if (s_task_done) {
        (void)xSemaphoreTake(s_task_done, 0);
        vSemaphoreDelete(s_task_done);
        s_task_done = nullptr;
    }
    s_task_done = xSemaphoreCreateBinary();
    if (!s_task_done) {
        set_start_error(MUSE_LINK_START_ERR_NO_INTERNAL_RAM);
        return false;
    }

    set_error_text("");
    s_stop_requested = false;
    s_run = true;
    set_state(MUSE_LINK_SESSION_STARTING);
    ESP_LOGI(kTag, "start requested (wifi timeout %u ms)", (unsigned)wifi_timeout_ms);

    // 起動処理は専用タスク (内蔵 8KB)。console (4KB) や LVGL/app をブロックしない。
    s_task_alive.store(true);  // タスク終了時に false へ (失敗しても再 start できる)
    if (xTaskCreatePinnedToCoreWithCaps(
            start_task, "muse_sess", kTaskStackBytes,
            reinterpret_cast<void*>(static_cast<uintptr_t>(wifi_timeout_ms)), kTaskPrio,
            &s_task, kTaskCore, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        s_run = false;
        s_task_alive.store(false);
        vSemaphoreDelete(s_task_done);
        s_task_done = nullptr;
        set_start_error(MUSE_LINK_START_ERR_NO_INTERNAL_RAM);
        return false;
    }
    return true;  // 受理。成否は status (session=starting/ready/error) で分かる
}

extern "C" void muse_link_stop(void) {
    if (s_state.load() == (int)MUSE_LINK_SESSION_OFF && s_task == nullptr) {
        return;
    }
    if (!s_task_alive.load()) {
        // 起動失敗などで既にタスクが終わっている: セマフォだけ片付ける。
        s_run = false;
        s_stop_requested = true;
        if (s_task_done) {
            (void)xSemaphoreTake(s_task_done, 0);
            vSemaphoreDelete(s_task_done);
            s_task_done = nullptr;
        }
        free_buffers();
        muse_link_wifi_release();
        set_error_text("");
        set_state(MUSE_LINK_SESSION_OFF);
        ESP_LOGI(kTag, "session stopped (already idle)");
        return;
    }
    s_stop_requested = true;
    s_run = false;
    if (s_voice_active) {
        muse_hatch_turn_cancel();
        s_voice_active = false;
    }
    if (s_text_active) {
        muse_hatch_text_cancel();
        s_text_active = false;
    }
    s_voice_pending = false;
    // タスクは終了時に Wi-Fi release / PSRAM 解放 / 状態更新を行う。
    bool stopped = false;
    if (s_task_done) {
        stopped = xSemaphoreTake(s_task_done, pdMS_TO_TICKS(1500)) == pdTRUE;
    }
    if (stopped) {
        s_task = nullptr;
        vSemaphoreDelete(s_task_done);
        s_task_done = nullptr;
        set_state(MUSE_LINK_SESSION_OFF);
        ESP_LOGI(kTag, "session stopped");
    } else {
        ESP_LOGW(kTag, "session stop: task still finishing (will clean up itself)");
    }
}

extern "C" bool muse_link_send_text(const char* text) {
    if (!text || !*text) {
        return false;
    }
    if (s_state.load() != (int)MUSE_LINK_SESSION_READY) {
        ESP_LOGW(kTag, "send_text: session not ready (%s)",
                 muse_link_session_state_name((muse_link_session_state_t)s_state.load()));
        return false;
    }
    char* copy = static_cast<char*>(malloc(strlen(text) + 1));
    if (!copy) {
        return false;
    }
    strcpy(copy, text);
    reply_reset();
    s_text_active = true;
    s_deadline_ms = now_ms() + kTextTurnTimeoutMs;
    advance(MUSE_SESSION_EV_TURN_BEGIN);
    ESP_LOGI(kTag, "send_text: %u bytes", (unsigned)strlen(text));
    muse_hatch_text_turn(copy);  // 所有権は SDK 側へ移る (free される)
    return true;
}

extern "C" bool muse_link_send_voice_note(const int16_t* pcm, size_t samples) {
    if (!pcm || samples == 0) {
        return false;
    }
    if (s_state.load() != (int)MUSE_LINK_SESSION_READY) {
        ESP_LOGW(kTag, "send_voice_note: session not ready (%s)",
                 muse_link_session_state_name((muse_link_session_state_t)s_state.load()));
        return false;
    }
    if (samples > kVoiceMaxSamples) {
        set_error_text("音声が長すぎます (15 秒まで)");
        return false;
    }
    // T54 の変換層で形式を事前検証 (16kHz mono PCM16)。
    const muse_link_voice_status_t vs =
        muse_link_voice_validate(pcm, samples * sizeof(int16_t));
    if (vs != MUSE_LINK_VOICE_OK) {
        set_error_text("音声データが不正です");
        return false;
    }
    if (!s_voice_pcm) {
        return false;
    }
    memcpy(s_voice_pcm, pcm, samples * sizeof(int16_t));
    s_voice_samples = samples;
    s_voice_pending = true;
    advance(MUSE_SESSION_EV_TURN_BEGIN);
    ESP_LOGI(kTag, "send_voice_note: %u samples queued", (unsigned)samples);
    return true;
}

extern "C" size_t muse_link_last_reply(char* out, size_t cap) {
    if (!out || cap == 0) {
        return 0;
    }
    size_t n = 0;
    if (s_reply && s_reply_mtx) {
        xSemaphoreTake(s_reply_mtx, portMAX_DELAY);
        n = s_reply_len;
        if (n >= cap) {
            n = cap - 1;
        }
        memcpy(out, s_reply, n);
        out[n] = '\0';
        xSemaphoreGive(s_reply_mtx);
    } else {
        out[0] = '\0';
    }
    return n;
}

extern "C" size_t muse_link_last_reply_len(void) {
    return s_reply_len;
}

extern "C" size_t muse_link_total_reply_len(void) {
    return s_reply_total;
}

extern "C" void muse_link_set_reply_callback(muse_link_reply_cb cb, void* ctx) {
    s_reply_cb = cb;
    s_reply_cb_ctx = ctx;
}

extern "C" muse_link_session_state_t muse_link_session_state(void) {
    return (muse_link_session_state_t)s_state.load();
}

extern "C" const char* muse_link_last_error(void) {
    return s_error;
}

extern "C" size_t muse_link_min_internal_bytes(void) {
    return kMinInternalBytes;
}

extern "C" size_t muse_link_min_psram_bytes(void) {
    return kMinPsramBytes;
}

// muse_link.cpp の status 組み立てから呼ばれる (セッション由来のフィールド)。
extern "C" void muse_link_session_fill_status(muse_link_status_fields_t* f) {
    if (!f) {
        return;
    }
    f->session = (muse_link_session_state_t)s_state.load();
    f->last_reply_len = s_reply_len;
    f->total_reply_len = s_reply_total;
    f->last_error = s_error;
}

// ---- @chat コンソールフック (グローバルスコープ) ----
// SDK の typed turn は返答を "@chat {...}" として stdout に書く。SAS-Watch では
// 返答本文をログに出さない方針なので、ここで取り込む (stdout には出さない)。
// SDK 側の定義は CMake でシンボル名を退避している。呼び出し元は SDK の
// セッションタスク (と、開始前チェックの呼び出し元)。
extern "C" void muse_hatch_console(const char* type, const char* text, const char* fields, ...) {
    (void)fields;
    if (!type) {
        return;
    }
    if (strcmp(type, "text") == 0) {
        reply_append(text);
        advance(MUSE_SESSION_EV_TURN_BEGIN);
    } else if (strcmp(type, "final") == 0) {
        // final はメッセージ全文。text 断片を既に受けていれば二重追記しない。
        if (s_reply_len == 0) {
            reply_append(text);
        }
    } else if (strcmp(type, "done") == 0) {
        s_text_active = false;
        reply_finish();
        advance(MUSE_SESSION_EV_TURN_DONE);
    } else if (strcmp(type, "error") == 0) {
        s_text_active = false;
        set_error_text("Muse でエラーが発生しました");
        advance(MUSE_SESSION_EV_TURN_ERROR);
    } else if (strcmp(type, "busy") == 0) {
        advance(MUSE_SESSION_EV_TURN_BEGIN);
    }
    // sent / message_done / options は無視 (進捗のみ)。
}
