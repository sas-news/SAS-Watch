// steps.cpp — 歩数 Feature。
//   ImuSample → 日付チェック → ピーク検出 (+StepsChanged 発行)。
//   RaiseDetected は検出器 → bus まで。画面 ON/OFF の制御は firmware 側。
// 永続化: "feat.steps" (新しい {ver,day,steps}、ver=1)。NVS 摩耗対策で
// kPersistEvery 歩ごとに保存。日次リセット時は必ず保存 (過去日付の残数を消す)。
#include "watch/features/steps.hpp"

#include <cstring>

#include "watch/event.hpp"
#include "watch/sensors.hpp"
#include "watch/settings.hpp"

namespace watch {
namespace features {

namespace {

// TODO(hw): 日付境界・保存間隔の動作は実機で確認。
constexpr const char* kKey = "feat.steps";
constexpr uint8_t kPersistVer = 1;
constexpr uint32_t kPersistEvery = 32;  // 何歩ごとに KV へ書くか

struct PersistBlob {
  uint8_t ver;
  uint8_t reserved[3];
  int32_t day;     // local 暦日 (epoch/86400)
  uint32_t steps;  // その日の歩数
};

int32_t s_day = -1;       // 現在のローカル日 (-1 = 未初期化)
uint32_t s_steps = 0;     // 今日の歩数
int64_t s_midnight_ms = 0;  // 次のローカル深夜 (now_ms 換算)

sensors::StepDetector s_step_det;
sensors::RaiseDetector s_raise_det;

int32_t local_day(const FeatureContext& c) {
  const int64_t tz = c.settings ? c.settings->tz_offset_min : 0;
  return static_cast<int32_t>(
      (c.clock.epoch_s() + tz * 60) / 86400);
}

int64_t next_midnight_epoch(const FeatureContext& c) {
  const int64_t tz = c.settings ? c.settings->tz_offset_min : 0;
  const int64_t local = c.clock.epoch_s() + tz * 60;
  return (local / 86400 + 1) * 86400 - tz * 60;
}

void persist(FeatureContext& c) {
  PersistBlob b{};
  b.ver = kPersistVer;
  b.day = s_day;
  b.steps = s_steps;
  c.storage.set(kKey, &b, sizeof(b));
}

// 日付変化の確認 + 深夜 deadline の更新。
void check_day(int64_t now_ms, FeatureContext& c) {
  const int32_t d = local_day(c);
  if (d != s_day) {
    s_day = d;
    s_steps = 0;
    persist(c);
    c.bus.publish({EventType::StepsChanged, 0});
  }
  s_midnight_ms = now_ms + (next_midnight_epoch(c) - c.clock.epoch_s()) * 1000;
}

void tick(int64_t now_ms, FeatureContext& c) { check_day(now_ms, c); }

int64_t next_deadline() { return s_midnight_ms; }

bool handle(const Action& a, FeatureContext& c) {
  if (a.type != ActionType::ImuSample) return false;
  const int16_t x = static_cast<int16_t>(a.arg0 & 0xFFFF);
  const int16_t y = static_cast<int16_t>((a.arg0 >> 16) & 0xFFFF);
  const int16_t z = static_cast<int16_t>(a.arg1 & 0xFFFF);
  const int64_t now = c.clock.now_ms();
  check_day(now, c);
  if (s_step_det.feed(x, y, z, now)) {
    ++s_steps;
    c.bus.publish({EventType::StepsChanged, s_steps});
    if (s_steps % kPersistEvery == 0) persist(c);
  }
  if (s_raise_det.feed(x, y, z, now)) {
    c.bus.publish({EventType::RaiseDetected, 0});
  }
  return true;
}

void save(FeatureContext& c) { persist(c); }

void restore(FeatureContext& c) {
  // GCC13 -Wdangling-pointer 対策 (counter.cpp と同じ): static に逃がす。
  static PersistBlob blob;
  size_t n = 0;
  if (c.storage.get(kKey, &blob, sizeof(blob), &n) && n == sizeof(blob) &&
      blob.ver == kPersistVer) {
    const int32_t d = local_day(c);
    s_day = d;
    s_steps = (blob.day == d) ? blob.steps : 0;
  }
  check_day(c.clock.now_ms(), c);
}

}  // namespace

uint32_t steps_today() { return s_steps; }
void steps_reset_state() {
  s_steps = 0;
  s_day = -1;
  s_midnight_ms = 0;
  s_step_det.reset();
  s_raise_det.reset();
}

const FeatureDescriptor kSteps = {
    "steps",
    HasScreen,
    Route::Steps,
    nullptr,
    handle,
    tick,
    next_deadline,
    save,
    restore,
};

}  // namespace features
}  // namespace watch
