// audio — ES7210(録音) / ES8311+NS4150B(再生) の音声サービス。
//   - 録音: BSP の bsp_audio_codec_microphone_init() → 16kHz mono PCM
//           → IMA-ADPCM → littlefs の storage パーティション (ADP1 形式)。
//   - 再生: ADP1 → PCM → esp_codec_dev_write。PA (GPIO46) は再生中だけON。
//   - ビープ: タイマー終了音 / ボタンのクリック音 (音量・ON/OFF は Settings)。
// 全部静的確保・各エンジン1本ずつ (録音と再生は排他)。
#pragma once

#include "watch/event_bus.hpp"
#include "watch/feature.hpp"

namespace audio {

struct Deps {
  watch::Clock* clock = nullptr;
  watch::PowerPolicy* power = nullptr;
  const watch::Settings* settings = nullptr;
};

// littlefs マウント + コーデック準備 + ワーカータスク起動。
// 失敗しても false を返すだけ (audio なしでも時計は動く)。
bool init(const Deps& deps);

// FeatureContext.audio に入れる実装。init 前なら nullptr。
watch::AudioPort* port();

// EventBus に TimerFinished → ビープ を配線する。
void attach(watch::EventBus& bus);

// UI ボタンのクリック音 (audio.click / audio.volume を見て鳴らす)。
void click();

// 音声メモファイルのディレクトリ ("/storage/memo")。
const char* memo_dir();

}  // namespace audio
