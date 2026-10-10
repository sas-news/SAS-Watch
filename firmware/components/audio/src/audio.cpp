// audio.cpp — ES7210(録音)/ES8311+NS4150B(再生) の実機音声サービス。
//   録音タスク: mic codec → 16kHz/16bit PCM → IMA-ADPCM → /storage/memo/<id>.adp
//   再生タスク: ADP1 デコード or トーン生成 → speaker codec。PA(GPIO46)は
//               再生中だけ ON (駆動中のみ通電して消費電力を抑える)。
// 音声メモのコンテナは ADP1 (core/adpcm.hpp 参照)。
// TODO(hw): 実機で確認 — コーデックのゲイン/音量レンジ、PA の極性、
//           録音品質 (NSR/帯域) は仮の値。
#include "audio/audio.hpp"

#include <dirent.h>
#include <sys/stat.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include "bsp/esp-bsp.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_codec_dev.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "watch/adpcm.hpp"
#include "watch/power.hpp"
#include "watch/settings.hpp"

namespace audio {
namespace {

constexpr const char* TAG = "audio";
constexpr const char* kMountPoint = "/storage";
constexpr const char* kMemoDir = "/storage/memo";

// 録音の処理単位。16kHz/16bit/mono で 512 サンプル = 32ms。
constexpr size_t kPcmChunkSamples = watch::audio::kAdpcmBlockPcm;
constexpr size_t kPcmChunkBytes = kPcmChunkSamples * sizeof(int16_t);

Deps s_deps;
SemaphoreHandle_t s_mtx = nullptr;   // エンジン状態の排他
esp_codec_dev_handle_t s_spk = nullptr;  // ES8311 (lazy)
esp_codec_dev_handle_t s_mic = nullptr;  // ES7210 (lazy)
bool s_mounted = false;
bool s_ready = false;             // init() が最後まで走ったか
TaskHandle_t s_rec_task = nullptr;
TaskHandle_t s_play_task = nullptr;

// この個体で音声を使うか。スピーカー/コーデック未搭載機体は
// sdkconfig で CONFIG_WATCH_DISABLE_AUDIO=y にする。
// 無効なら I2S/codec/タスク初期化ごとスキップする(音声コード自体は残す)。
#if CONFIG_WATCH_DISABLE_AUDIO
constexpr bool kAudioEnabled = false;
#else
constexpr bool kAudioEnabled = true;
#endif

// コーデック IC の 7bit I2C アドレス (docs/board.md)。
// スピーカー/コーデック未搭載機体では BSP の codec init が
// 中の assert(BSP_NULL_CHECK) で落ちるため、呼ぶ前に I2C で有無を見る。
constexpr uint8_t kEs8311Addr = 0x18;
constexpr uint8_t kEs7210Addr = 0x40;
bool s_spk_codec = false;  // ES8311 が応答したか
bool s_mic_codec = false;  // ES7210 が応答したか

// ---- 録音ジョブ ----
struct RecJob {
  volatile bool active = false;   // ジョブ占有中
  volatile bool stop = false;     // record_end からの停止要求
  volatile bool done = false;     // タスクが片付け完了
  volatile bool commit = false;   // record_end(commit)
  uint32_t memo_id = 0;
  uint32_t max_sec = 60;
  uint32_t samples = 0;
  uint32_t size = 0;              // 書き込んだ総バイト数
  uint8_t level = 0;              // 直近チャンクのピーク 0..100
  char path[64];
};
RecJob s_rec;

// ---- 再生ジョブ ----
enum class PlaySrc : uint8_t { File, Beep };
struct PlayJob {
  volatile bool active = false;
  volatile bool cancel = false;
  volatile bool done = true;
  PlaySrc src = PlaySrc::File;
  uint32_t memo_id = 0;
  uint8_t vol = 0;
  watch::BeepKind beep = watch::BeepKind::Click;
};
PlayJob s_play;
watch::PowerPolicy::Lease s_play_lease;  // 再生/ビープ中の Deep Sleep 防止

// light sleep 抑止の PM lock。core の Lease (deep sleep 抑止) とは
// 別系統なので両方持つ。録音/再生ジョブの実行中だけ acquire する。
// (light sleep に入ると I2S/codec 処理が止まり転送が壊れる)
esp_pm_lock_handle_t s_pm_lock = nullptr;
void pm_acquire() { if (s_pm_lock) esp_pm_lock_acquire(s_pm_lock); }
void pm_release() { if (s_pm_lock) esp_pm_lock_release(s_pm_lock); }

void lock() { xSemaphoreTake(s_mtx, portMAX_DELAY); }
void unlock() { xSemaphoreGive(s_mtx); }

// ---- コーデック ----

esp_codec_dev_handle_t speaker() {
  if (!s_spk && kAudioEnabled && s_spk_codec)
    s_spk = bsp_audio_codec_speaker_init();
  return s_spk;
}
esp_codec_dev_handle_t mic() {
  if (!s_mic && kAudioEnabled && s_mic_codec)
    s_mic = bsp_audio_codec_microphone_init();
  return s_mic;
}

void codec_fs(esp_codec_dev_sample_info_t* fs) {
  std::memset(fs, 0, sizeof(*fs));
  fs->bits_per_sample = 16;
  fs->channel = 1;
  fs->sample_rate = watch::audio::kAdpcmSampleRate;
  // channel_mask/mclk_multiple = 0 → ドライバ既定 // TODO(hw): 実機で確認
}

// PA (GPIO46) は再生系ジョブの間だけ HIGH。
void pa_on() { gpio_set_level(BSP_POWER_AMP_IO, 1); }
void pa_off() { gpio_set_level(BSP_POWER_AMP_IO, 0); }

// ---- 録音タスク ----

void memo_path(uint32_t id, char* out, size_t cap, bool tmp) {
  std::snprintf(out, cap, "%s/%08lx.%s", kMemoDir,
                static_cast<unsigned long>(id), tmp ? "tmp" : "adp");
}

// 先頭16B に確定ヘッダを書き込む (samples は保存時の実値)。
bool patch_header(FILE* f, uint32_t samples) {
  uint8_t hdr[watch::audio::kAdp1HeaderSize];
  watch::audio::adp1_write_header(hdr, samples);
  if (fseek(f, 0, SEEK_SET) != 0) return false;
  return fwrite(hdr, 1, sizeof(hdr), f) == sizeof(hdr);
}

void rec_task(void*) {
  int16_t pcm[kPcmChunkSamples];
  uint8_t enc[watch::audio::kAdpcmBlockBytes];
  watch::audio::ImaAdpcmEncoder encoder;

  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);  // ジョブ待ち

    FILE* f = std::fopen(s_rec.path, "w+b");
    if (!f) {
      ESP_LOGE(TAG, "fopen %s failed", s_rec.path);
      lock();
      s_rec.done = true;
      s_rec.active = false;
      unlock();
      pm_release();  // ジョブ終了: light sleep 抑止を解除
      continue;
    }
    uint8_t hdr[watch::audio::kAdp1HeaderSize];
    watch::audio::adp1_write_header(hdr, 0);
    fwrite(hdr, 1, sizeof(hdr), f);

    encoder.reset();
    esp_codec_dev_sample_info_t fs;
    codec_fs(&fs);
    esp_codec_dev_handle_t m = mic();
    if (m && esp_codec_dev_open(m, &fs) == ESP_CODEC_DEV_OK) {
      esp_codec_dev_set_in_gain(m, 24.0f);  // TODO(hw): 実機で確認
      while (!s_rec.stop) {
        if (s_rec.samples >= s_rec.max_sec * watch::audio::kAdpcmSampleRate) {
          break;
        }
        const int rd = esp_codec_dev_read(m, pcm, sizeof(pcm));
        if (rd <= 0) {
          vTaskDelay(pdMS_TO_TICKS(5));
          continue;
        }
        const size_t got = static_cast<size_t>(rd) / sizeof(int16_t);
        int16_t peak = 0;
        for (size_t i = 0; i < got; ++i) {
          const int v = pcm[i] < 0 ? -pcm[i] : pcm[i];
          if (v > peak) peak = static_cast<int16_t>(v);
        }
        lock();
        s_rec.level = static_cast<uint8_t>((static_cast<int>(peak) * 100) / 32767);
        const size_t wb = encoder.encode(pcm, got, enc, sizeof(enc));
        if (wb > 0) s_rec.size += fwrite(enc, 1, wb, f);
        s_rec.samples += got;
        unlock();
      }
      esp_codec_dev_close(m);
    } else {
      ESP_LOGE(TAG, "mic codec open failed");
    }

    // 半端 nibble を確定 → ヘッダを書き直す。
    const size_t tail = encoder.flush(enc, sizeof(enc));
    if (tail > 0) s_rec.size += fwrite(enc, 1, tail, f);
    patch_header(f, s_rec.samples);
    s_rec.size += watch::audio::kAdp1HeaderSize;
    std::fclose(f);

    const bool commit = s_rec.commit;
    if (commit) {
      char final_path[64];
      memo_path(s_rec.memo_id, final_path, sizeof(final_path), false);
      if (rename(s_rec.path, final_path) != 0) {
        ESP_LOGW(TAG, "rename %s failed", s_rec.path);
        remove(s_rec.path);
      }
    } else {
      remove(s_rec.path);
    }
    ESP_LOGI(TAG, "rec done id=%lu samples=%lu size=%lu commit=%d",
             static_cast<unsigned long>(s_rec.memo_id),
             static_cast<unsigned long>(s_rec.samples),
             static_cast<unsigned long>(s_rec.size), commit ? 1 : 0);
    lock();
    s_rec.done = true;
    s_rec.active = false;
    unlock();
    pm_release();  // ジョブ終了: light sleep 抑止を解除
  }
}

// ---- 再生タスク ----

// ビープの音色テーブル {Hz, ms}。Hz=0 は無音。
struct Tone { uint16_t hz; uint16_t ms; };
const Tone kTonesClick[] = {{1900, 30}};
const Tone kTonesTimer[] = {
    {880, 200}, {0, 120}, {880, 200}, {0, 120}, {880, 400},
};
// アラーム: 上昇3音。繰り返しは UI 側が ~5s ごとに beep を呼び直す。
const Tone kTonesAlarm[] = {
    {988, 120},  {0, 60},  {1319, 120}, {0, 60},
    {1760, 200}, {0, 160}, {988, 120},  {0, 60},
    {1319, 120}, {0, 60},  {1760, 300},
};

// PCM int16 を codec に流す。キャンセルしたら false。
bool play_pcm(esp_codec_dev_handle_t spk, const int16_t* pcm, size_t n) {
  size_t off = 0;
  while (off < n) {
    if (s_play.cancel) return false;
    const size_t part = (n - off > 256) ? 256 : n - off;
    // codec API が void* を取るので const を外す (書き込みはしない)。
    esp_codec_dev_write(spk, const_cast<int16_t*>(pcm + off),
                        static_cast<int>(part * sizeof(int16_t)));
    off += part;
  }
  return true;
}

// トーン列を生成して再生。ビープ用 (ファイルを介さない)。
void play_tones(esp_codec_dev_handle_t spk, const Tone* tones, size_t count,
                uint8_t vol) {
  int16_t buf[kPcmChunkSamples];
  const float amp = 32767.0f * 0.35f * (static_cast<float>(vol) / 100.0f);
  const uint32_t rate = watch::audio::kAdpcmSampleRate;
  const uint32_t fade = rate / 500;  // 2ms の立ち上がり/下がり
  for (size_t t = 0; t < count && !s_play.cancel; ++t) {
    const Tone& tn = tones[t];
    const uint32_t total = (static_cast<uint32_t>(tn.ms) * rate) / 1000;
    uint32_t i = 0;
    while (i < total && !s_play.cancel) {
      const size_t n =
          (total - i > kPcmChunkSamples) ? kPcmChunkSamples : (total - i);
      for (size_t k = 0; k < n; ++k) {
        const uint32_t idx = i + k;
        int16_t s = 0;
        if (tn.hz != 0 && amp > 0) {
          float g = 1.0f;
          if (idx < fade) g = static_cast<float>(idx) / fade;
          if (idx > total - fade) g = static_cast<float>(total - idx) / fade;
          s = static_cast<int16_t>(
              amp * g *
              sinf(6.283185307179586f * tn.hz * idx / rate));
        }
        buf[k] = s;
      }
      play_pcm(spk, buf, n);
      i += n;
    }
  }
}

// ADP1 ファイルをデコードして再生。
void play_file(esp_codec_dev_handle_t spk, uint32_t memo_id, uint8_t vol) {
  char path[64];
  memo_path(memo_id, path, sizeof(path), false);
  FILE* f = std::fopen(path, "rb");
  if (!f) {
    ESP_LOGW(TAG, "no file %s", path);
    return;
  }
  uint8_t hdr[watch::audio::kAdp1HeaderSize];
  if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
    std::fclose(f);
    return;
  }
  uint32_t samples = 0;
  const uint8_t* body = nullptr;
  size_t body_len = 0;
  if (!watch::audio::adp1_parse(hdr, sizeof(hdr), &samples, &body, &body_len)) {
    ESP_LOGW(TAG, "bad adp1 %s", path);
    std::fclose(f);
    return;
  }
  (void)body;
  (void)body_len;

  watch::audio::ImaAdpcmDecoder dec;
  dec.reset();
  uint8_t enc[512];
  int16_t pcm[512 * 2];  // nibble は 1 バイトに 2 サンプル
  size_t rd;
  while (!s_play.cancel && (rd = fread(enc, 1, sizeof(enc), f)) > 0) {
    const size_t n = dec.decode(enc, rd, pcm, sizeof(pcm) / sizeof(int16_t));
    if (n > 0) play_pcm(spk, pcm, n);
  }
  std::fclose(f);
}

void play_task(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);  // ジョブ待ち
    PlayJob job;
    lock();
    std::memcpy(&job, &s_play, sizeof(job));
    unlock();

    esp_codec_dev_handle_t spk = speaker();
    esp_codec_dev_sample_info_t fs;
    codec_fs(&fs);
    if (spk && esp_codec_dev_open(spk, &fs) == ESP_CODEC_DEV_OK) {
      esp_codec_dev_set_out_vol(spk, job.vol);  // TODO(hw): 実機で音量感確認
      pa_on();
      if (job.src == PlaySrc::File) {
        play_file(spk, job.memo_id, job.vol);
      } else {
        const Tone* tones = kTonesClick;
        size_t n = sizeof(kTonesClick) / sizeof(kTonesClick[0]);
        if (job.beep == watch::BeepKind::TimerDone) {
          tones = kTonesTimer;
          n = sizeof(kTonesTimer) / sizeof(kTonesTimer[0]);
        } else if (job.beep == watch::BeepKind::Alarm) {
          tones = kTonesAlarm;
          n = sizeof(kTonesAlarm) / sizeof(kTonesAlarm[0]);
        }
        play_tones(spk, tones, n, job.vol);
      }
      pa_off();
      esp_codec_dev_close(spk);
    } else {
      ESP_LOGE(TAG, "spk codec open failed");
    }
    lock();
    s_play.done = true;
    s_play.active = false;
    s_play.memo_id = 0;
    s_play_lease.release();
    unlock();
    pm_release();  // ジョブ終了: light sleep 抑止を解除
  }
}

// ---- AudioPort 実装 ----

class EspAudio : public watch::AudioPort {
 public:
  bool record_begin(uint32_t memo_id, uint32_t max_sec) override {
    if (!s_ready || !s_mounted || !mic()) return false;
    lock();
    if (s_rec.active || s_play.active) {
      unlock();
      return false;
    }
    s_rec = RecJob{};
    s_rec.active = true;
    s_rec.memo_id = memo_id;
    s_rec.max_sec = max_sec;
    memo_path(memo_id, s_rec.path, sizeof(s_rec.path), true);
    unlock();
    pm_acquire();  // ジョブ完了 (タスク側の pm_release) まで対
    if (s_rec_task) xTaskNotifyGive(s_rec_task);
    return true;
  }

  bool record_end(bool commit, uint32_t* out_sec, uint32_t* out_size) override {
    lock();
    if (!s_rec.active) {
      unlock();
      return false;
    }
    s_rec.commit = commit;
    s_rec.stop = true;
    unlock();
    // タスクの掃除が終わるまで待つ (codec read の timeout 分だけ待機)。
    const int64_t deadline = esp_timer_get_time() + 3'000'000;
    while (esp_timer_get_time() < deadline) {
      lock();
      const bool done = s_rec.done;
      unlock();
      if (done) break;
      vTaskDelay(pdMS_TO_TICKS(10));
    }
    lock();
    const bool done = s_rec.done;
    const uint32_t samples = s_rec.samples;
    const uint32_t size = s_rec.size;
    unlock();
    if (!done) return false;
    if (out_sec) *out_sec = samples / watch::audio::kAdpcmSampleRate;
    if (out_size) *out_size = size;
    return true;
  }

  bool recording() const override { return s_rec.active; }
  uint32_t record_elapsed_s() const override {
    return s_rec.samples / watch::audio::kAdpcmSampleRate;
  }
  uint8_t record_level() const override { return s_rec.level; }

  bool play_begin(uint32_t memo_id, uint8_t volume) override {
    if (!s_ready || !s_mounted || !speaker()) return false;
    lock();
    if (s_rec.active || s_play.active) {
      unlock();
      return false;
    }
    s_play = PlayJob{};
    s_play.active = true;
    s_play.done = false;
    s_play.src = PlaySrc::File;
    s_play.memo_id = memo_id;
    s_play.vol = volume;
    unlock();
    if (s_deps.power) {
      s_play_lease = s_deps.power->acquire(watch::Res::Audio, "audio.play");
    }
    pm_acquire();  // ジョブ完了 (タスク側の pm_release) まで対
    if (s_play_task) xTaskNotifyGive(s_play_task);
    return true;
  }

  void play_stop() override {
    s_play.cancel = true;
    s_play_lease.release();
    // 終了を待つ (play タスクは ~32ms ごとに cancel を見る)。
    const int64_t deadline = esp_timer_get_time() + 1'000'000;
    while (esp_timer_get_time() < deadline) {
      lock();
      const bool done = s_play.done;
      unlock();
      if (done) return;
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }

  bool playing() const override {
    return s_play.active && s_play.src == PlaySrc::File;
  }

  void beep(watch::BeepKind kind, uint8_t volume) override {
    if (!s_ready || !kAudioEnabled || !s_spk_codec || volume == 0) return;
    lock();
    // 録音中・再生中は鳴らさない (時計には1系統しかない)。
    if (s_rec.active || s_play.active) {
      unlock();
      return;
    }
    s_play = PlayJob{};
    s_play.active = true;
    s_play.done = false;
    s_play.src = PlaySrc::Beep;
    s_play.beep = kind;
    s_play.vol = volume;
    unlock();
    if (s_deps.power) {
      s_play_lease = s_deps.power->acquire(watch::Res::Audio, "audio.beep");
    }
    pm_acquire();  // ジョブ完了 (タスク側の pm_release) まで対
    if (s_play_task) xTaskNotifyGive(s_play_task);
  }

  bool memo_audio_size(uint32_t memo_id, uint32_t* out_size) override {
    if (!s_mounted) return false;
    if (s_rec.active && s_rec.memo_id == memo_id) return false;
    char path[64];
    memo_path(memo_id, path, sizeof(path), false);
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fclose(f);
    if (n <= 0) return false;
    if (out_size) *out_size = static_cast<uint32_t>(n);
    return true;
  }

  bool memo_audio_read(uint32_t memo_id, uint32_t offset, void* buf,
                       size_t* len_inout) override {
    if (!s_mounted || !buf || !len_inout) return false;
    if (s_rec.active && s_rec.memo_id == memo_id) return false;
    char path[64];
    memo_path(memo_id, path, sizeof(path), false);
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    fseek(f, static_cast<long>(offset), SEEK_SET);
    const size_t got = std::fread(buf, 1, *len_inout, f);
    std::fclose(f);
    *len_inout = got;
    return true;
  }

  void memo_audio_erase(uint32_t memo_id) override {
    if (!s_mounted) return;
    char path[64];
    memo_path(memo_id, path, sizeof(path), false);
    remove(path);
    memo_path(memo_id, path, sizeof(path), true);
    remove(path);
  }
};

EspAudio s_audio;

void on_event(const watch::Event& e, void*) {
  if (e.type == watch::EventType::TimerFinished ||
      e.type == watch::EventType::AlarmRinging) {
    const watch::BeepKind kind = e.type == watch::EventType::TimerFinished
                                     ? watch::BeepKind::TimerDone
                                     : watch::BeepKind::Alarm;
    const uint8_t vol = s_deps.settings
                            ? static_cast<uint8_t>(
                                  s_deps.settings->audio_volume > 100
                                      ? 100
                                      : s_deps.settings->audio_volume)
                            : 70;
    s_audio.beep(kind, vol);
  }
}

// 前回起動の残骸 (録音中に電源断した .tmp) を消す。
void cleanup_tmp_files() {
  DIR* d = opendir(kMemoDir);
  if (!d) return;
  struct dirent* e;
  while ((e = readdir(d)) != nullptr) {
    const size_t n = std::strlen(e->d_name);
    if (n > 4 && std::strcmp(e->d_name + n - 4, ".tmp") == 0) {
      char path[64];
      std::snprintf(path, sizeof(path), "%s/%s", kMemoDir, e->d_name);
      remove(path);
    }
  }
  closedir(d);
}

}  // namespace

const char* memo_dir() { return kMemoDir; }

watch::AudioPort* port() { return &s_audio; }

void attach(watch::EventBus& bus) {
  bus.subscribe(watch::EventType::TimerFinished, on_event, nullptr);
  bus.subscribe(watch::EventType::AlarmRinging, on_event, nullptr);
}

void click() {
  if (!s_deps.settings) return;
  if (!s_deps.settings->audio_click) return;
  const uint8_t vol = static_cast<uint8_t>(
      s_deps.settings->audio_volume > 100 ? 100 : s_deps.settings->audio_volume);
  s_audio.beep(watch::BeepKind::Click, vol);
}

bool init(const Deps& deps) {
  s_deps = deps;
  s_mtx = xSemaphoreCreateMutex();
  // light sleep 抑止の PM lock は初期化時に1回だけ確保する
  // (CONFIG_PM_ENABLE 無しでは NOT_SUPPORTED で NULL のまま → no-op)。
  const esp_err_t pl = esp_pm_lock_create(
      ESP_PM_NO_LIGHT_SLEEP, ESP_PM_CPU_FREQ_MAX, "audio", &s_pm_lock);
  if (pl != ESP_OK) {
    ESP_LOGW(TAG, "pm lock create: %s", esp_err_to_name(pl));
  }

  if (kAudioEnabled) {
    // PA ピンは「再生中だけON」。起動時は OFF 固定。
    gpio_config_t io{};
    io.pin_bit_mask = 1ULL << BSP_POWER_AMP_IO;
    io.mode = GPIO_MODE_OUTPUT;
    io.pull_up_en = GPIO_PULLUP_DISABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io);
    pa_off();

    // コーデック IC の有無を I2C で確認。未搭載なら codec init に進まない
    // (BSP 側が assert で落ちるため)。音声機能のみ無効化し残りは動かす。
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    s_spk_codec = bus && i2c_master_probe(bus, kEs8311Addr, 50) == ESP_OK;
    s_mic_codec = bus && i2c_master_probe(bus, kEs7210Addr, 50) == ESP_OK;
    if (!s_spk_codec) {
      ESP_LOGW(TAG, "ES8311(0x%02x) no response - speaker/beep disabled", kEs8311Addr);
    }
    if (!s_mic_codec) {
      ESP_LOGW(TAG, "ES7210(0x%02x) no response - mic/rec disabled", kEs7210Addr);
    }
  } else {
    ESP_LOGW(TAG, "audio disabled (CONFIG_WATCH_DISABLE_AUDIO) - speaker unit absent");
  }

  esp_vfs_littlefs_conf_t conf{};
  conf.base_path = kMountPoint;
  conf.partition_label = "storage";
  conf.format_if_mount_failed = true;
  conf.dont_mount = false;
  const esp_err_t mret = esp_vfs_littlefs_register(&conf);
  if (mret != ESP_OK) {
    ESP_LOGE(TAG, "littlefs mount failed: %s", esp_err_to_name(mret));
    return false;
  }
  s_mounted = true;
  mkdir(kMemoDir, 0775);
  cleanup_tmp_files();

  if (kAudioEnabled &&
      (xTaskCreate(rec_task, "audio_rec", 4096, nullptr, 8, &s_rec_task) !=
           pdPASS ||
       xTaskCreate(play_task, "audio_play", 4096, nullptr, 8, &s_play_task) !=
           pdPASS)) {
    ESP_LOGE(TAG, "task create failed");
    return false;
  }
  s_ready = true;
  return true;
}

}  // namespace audio
