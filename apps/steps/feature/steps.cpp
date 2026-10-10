// steps.cpp — 歩数 Feature。
//   主経路: firmware の HW pedometer 読み出し → StepsHwSync Action (arg0 =
//   チップの 24bit 累積カウンタ生値)。ここで delta 化して当日歩数に加算。
//   HW が無効 (init 失敗 = チップ非対応) の間は ImuSample → StepDetector
//   のソフト検出で数える (フォールバック)。
//   RaiseDetected は ImuSample → 検出器 → bus まで。画面ON/OFF制御はfirmware。
// 永続化: "feat.steps" ({ver=2,day,steps,hw_last})。NVS 摩耗対策で
// kPersistEvery 歩ごとに保存。日次リセット時は必ず保存。
#include "watch/features/steps.hpp"

#include <cstring>

#include "watch/event.hpp"
#include "watch/sensors.hpp"
#include "watch/settings.hpp"

namespace watch {
namespace features {

namespace {

// TODO(hw): 日付境界・保存間隔・異常 delta の扱いは実機で確認。
constexpr const char* kKey = "feat.steps";
constexpr uint8_t kPersistVer = 2;
constexpr uint32_t kPersistEvery = 32;  // 何歩ぶん貯まったら KV へ書くか
// 同期間隔 (最悪でも数分) ではありえない歩数差 → カウンタリセット疑いとみなす。
// 24bit ラップは (cur-last)&0xFFFFFF で吸収できるが、チップ電源落ち等の
// リセットも同じ見え方をするので区別不可。日に10万歩は無い値として破棄。
constexpr uint32_t kMaxPlausibleDelta = 100000;

struct PersistBlob {
  uint8_t ver;
  uint8_t reserved[3];
  int32_t day;       // local 暦日 (epoch/86400)
  uint32_t steps;    // その日の歩数
  uint32_t hw_last;  // 最後に見た HW カウンタ生値 (再起動・deep sleep またぎ用)
};

int32_t s_day = -1;       // 現在のローカル日 (-1 = 未初期化)
uint32_t s_steps = 0;     // 今日の歩数
int64_t s_midnight_ms = 0;  // 次のローカル深夜 (now_ms 換算)
uint32_t s_unsaved = 0;   // persist してから増えた分
bool s_hw_active = false; // HW pedometer 経路が生きているか
uint32_t s_hw_last = 0;   // 最後に受け取った HW カウンタ生値

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
  b.hw_last = s_hw_last;
  c.storage.set(kKey, &b, sizeof(b));
  s_unsaved = 0;
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

void add_steps(uint32_t n, FeatureContext& c) {
  s_steps += n;
  s_unsaved += n;
  c.bus.publish({EventType::StepsChanged, s_steps});
  if (s_unsaved >= kPersistEvery) persist(c);
}

void tick(int64_t now_ms, FeatureContext& c) { check_day(now_ms, c); }

int64_t next_deadline() { return s_midnight_ms; }

bool handle(const Action& a, FeatureContext& c) {
  const int64_t now = c.clock.now_ms();
  if (a.type == ActionType::StepsHwSync) {
    check_day(now, c);
    // HW 同期が届いた = HW pedometer が有効 → ソフト検出は止める。
    s_hw_active = true;
    // 24bit カウンタの差分 (ラップ吸収)。初回は hw_last=0 なので
    // cur そのままが差分になる (boot 直後なので実害なし)。
    uint32_t delta = (a.arg0 - s_hw_last) & 0xFFFFFFu;
    s_hw_last = a.arg0;
    if (delta > kMaxPlausibleDelta) {
      // カウンタリセット疑い (チップ電断等): 差分は信用しない。
      delta = 0;
    }
    if (delta) add_steps(delta, c);
    return true;
  }
  if (a.type != ActionType::ImuSample) return false;
  const int16_t x = static_cast<int16_t>(a.arg0 & 0xFFFF);
  const int16_t y = static_cast<int16_t>((a.arg0 >> 16) & 0xFFFF);
  const int16_t z = static_cast<int16_t>(a.arg1 & 0xFFFF);
  check_day(now, c);
  // HW 経路が生きている間はソフト検出を走らせない (二重計上防止)。
  if (!s_hw_active && s_step_det.feed(x, y, z, now)) {
    add_steps(1, c);
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
    s_hw_last = blob.hw_last;
    // HW が一度でも同期していれば生きていた証拠 → 直後の sw 計数を防ぐ。
    s_hw_active = (blob.hw_last != 0);
  }
  check_day(c.clock.now_ms(), c);
}

}  // namespace

uint32_t steps_today() { return s_steps; }
void steps_reset_state() {
  s_steps = 0;
  s_day = -1;
  s_midnight_ms = 0;
  s_unsaved = 0;
  s_hw_active = false;
  s_hw_last = 0;
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
