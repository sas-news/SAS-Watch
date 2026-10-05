// platform.hpp — ハード依存を切り離すためのインターフェース。
// core/ は ESP-IDF / LVGL / FreeRTOS のヘッダを include しない。
// 実装は firmware/ 側が提供する (RTC, NVS, ESP_LOG など)。
#pragma once

#include <cstddef>
#include <cstdint>

namespace watch {

// 時計。now_ms は monotonic (起動からの経過)、epoch_s は壁時計 (Unix秒)。
class Clock {
 public:
  virtual ~Clock() = default;
  // monotonic ミリ秒。sleep 中も狂わない実装を期待する (esp_timer 相当)。
  virtual int64_t now_ms() = 0;
  // 壁時計 (Unix epoch 秒)。RTC から読む。
  virtual int64_t epoch_s() = 0;
  // 壁時計を合わせる (phone の time.set)。未対応なら false。
  virtual bool set_epoch_s(int64_t epoch_s) = 0;
};

// Key-Value ストア。キーは短い文字列、値はバイト列。
// 実装は firmware 側 (NVS blob 相当)。core はバックエンドを知らない。
class KeyValueStore {
 public:
  virtual ~KeyValueStore() = default;
  // key が無ければ 0。あれば必要なバイト数を返す。
  virtual size_t size(const char* key) = 0;
  // key から読む。buf_cap 不足なら false を返し out_len に必要量を入れる。
  virtual bool get(const char* key, void* buf, size_t buf_cap, size_t* out_len) = 0;
  // key に書く。len==0 は値の消去と同等でもよい。
  virtual bool set(const char* key, const void* data, size_t len) = 0;
  virtual bool erase(const char* key) = 0;
};

// ロガー。実装は ESP_LOG や printf。
enum class LogLevel : uint8_t { Debug, Info, Warn, Error };

class Log {
 public:
  virtual ~Log() = default;
  virtual void write(LogLevel level, const char* tag, const char* message) = 0;
};

// ログ出力先の差し替え (未設定なら何もしない)。
void log_set(Log* log);
Log* log_get();
void log_write(LogLevel level, const char* tag, const char* message);

// ---------- Audio ----------
// 録音・再生・効果音。実機のみ提供 (sim/テストでは差し替え可)。
// FeatureContext.audio は nullptr でもよい: その場合音声機能は静かに無効。
// 音声メモの実ファイルの置き場 (littlefs 等) は実装側の都合。
// memo_id ↔ ファイルの対応も実装側が持つ。core は id で引くだけ。

// 効果音の種類。音量は呼び出し側が Settings から取って渡す。
enum class BeepKind : uint8_t {
  Click = 0,      // ボタンのクリック (settings の audio.click でON/OFF)
  TimerDone = 1,  // タイマー終了
  Alarm = 2,      // アラーム鳴動
};

class AudioPort {
 public:
  virtual ~AudioPort() = default;

  // ---- 録音 (音声メモ) ----
  // memo_id 宛に録り始める。開始できなければ false。
  virtual bool record_begin(uint32_t memo_id, uint32_t max_sec) = 0;
  // 録音を止める。commit=false なら捨てる。
  // 確定した秒数とファイルバイト数を out_* に入れて true。
  // 録っていない/失敗は false。
  virtual bool record_end(bool commit, uint32_t* out_sec,
                          uint32_t* out_size) = 0;
  virtual bool recording() const = 0;
  virtual uint32_t record_elapsed_s() const = 0;  // 録音中の経過秒
  virtual uint8_t record_level() const = 0;       // 入力レベル 0..100

  // ---- 音声メモの再生 ----
  // volume は 0..100。再生開始できなければ false。
  virtual bool play_begin(uint32_t memo_id, uint8_t volume) = 0;
  virtual void play_stop() = 0;
  virtual bool playing() const = 0;

  // ---- 効果音 (非同期に鳴らす。volume 0..100) ----
  virtual void beep(BeepKind kind, uint8_t volume) = 0;

  // ---- 音声メモ実体 (BLE 転送用の読み出し) ----
  virtual bool memo_audio_size(uint32_t memo_id, uint32_t* out_size) = 0;
  // buf に最大 *len_inout バイト読み、実際に読んだ数に更新。末尾以降は 0。
  virtual bool memo_audio_read(uint32_t memo_id, uint32_t offset, void* buf,
                               size_t* len_inout) = 0;
  // メモ削除時に実ファイルも消す。
  virtual void memo_audio_erase(uint32_t memo_id) = 0;
};

}  // namespace watch
