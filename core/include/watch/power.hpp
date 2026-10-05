// power.hpp — 電源ポリシーの純粋ロジック (plan.md L章)。
// 実際のハード操作 (画面OFF, esp_light_sleep_start, ALDO2 など) はしない。
// 「今どうすべきか」= State と wants_deep_sleep() を返すだけ。
#pragma once

#include <cstdint>

#include "watch/event_bus.hpp"

namespace watch {

// リソースの種類。借りている間は Deep Sleep に入らない。
enum class Res : uint8_t { CpuMax = 0, Wifi, Audio, Display, Imu, BleFast, _Count };

// plan.md L章の状態機械。
//   Active → Dim → ScreenOff → DeepSleepCandidate
// DeepSleepCandidate は「実際に Deep Sleep してよい」という推奨。
// firmware 側が esp_deep_sleep_start() する判断材料に使う。
enum class PowerState : uint8_t { Active = 0, Dim, ScreenOff, DeepSleepCandidate };

class PowerPolicy {
 public:
  static constexpr uint8_t kMaxLeases = 8;

  struct Thresholds {
    uint32_t dim_after_s;         // 無操作から Dim まで
    uint32_t screen_off_after_s;  // 無操作から ScreenOff まで (dim を含む累積)
    uint32_t deep_sleep_after_s;  // 無操作から DeepSleepCandidate まで
  };

  explicit PowerPolicy(EventBus* bus = nullptr) : bus_(bus) {}

  // plan.md L章の初期値: Dim 8s / ScreenOff +4s(=12s) / DeepSleep 30min。
  void configure(uint32_t dim_after_s, uint32_t screen_off_after_s,
                 uint32_t deep_sleep_after_s);
  const Thresholds& thresholds() const { return th_; }

  // なんらかの入力があった。Active に戻す。返り値は直前の状態。
  PowerState kick_activity(int64_t now_ms);

  // Home で Back (= 画面OFF要求)。ScreenOff に落とす。
  void request_screen_off(int64_t now_ms);

  // 毎ループ呼ぶ。現在の状態を返す。遷移があれば PowerStateChanged を出す。
  PowerState update(int64_t now_ms);

  PowerState state() const { return state_; }
  int64_t last_activity_ms() const { return last_activity_ms_; }

  // 次の状態遷移が起きる時刻 (なければ 0)。Runtime::next_deadline_ms が使う。
  int64_t next_transition_ms() const;

  // BLE 接続中は DeepSleep に行かない (plan.md L章)。
  void set_ble_connected(bool connected) { ble_connected_ = connected; }
  bool ble_connected() const { return ble_connected_; }

  // Deep Sleep に進んでよいか。状態 + Lease + BLE の総合判断。
  bool wants_deep_sleep() const { return state_ == PowerState::DeepSleepCandidate; }

  // Lease: 借りて返す。返し忘れは DEV 画面に出す想定。
  class Lease {
   public:
    Lease() = default;
    ~Lease() { release(); }
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
    Lease(Lease&& o) noexcept : owner_(o.owner_), slot_(o.slot_) {
      o.owner_ = nullptr;
      o.slot_ = 0xFF;
    }
    Lease& operator=(Lease&& o) noexcept {
      if (this != &o) {
        release();
        owner_ = o.owner_;
        slot_ = o.slot_;
        o.owner_ = nullptr;
        o.slot_ = 0xFF;
      }
      return *this;
    }
    bool valid() const { return owner_ != nullptr; }
    void release();

   private:
    friend class PowerPolicy;
    Lease(PowerPolicy* owner, uint8_t slot) : owner_(owner), slot_(slot) {}
    PowerPolicy* owner_ = nullptr;
    uint8_t slot_ = 0xFF;
  };

  // who は最大15文字に切り詰めて保持する (DEV 表示用)。
  Lease acquire(Res res, const char* who);
  uint8_t lease_count() const { return lease_count_; }
  // 借り主の一覧をコールバックで回す (fn(who, res))。DEV 画面用。
  void for_each_lease(void (*fn)(Res, const char*, void*), void* ctx) const;

 private:
  struct LeaseSlot {
    bool used = false;
    Res res = Res::CpuMax;
    char who[16] = {};
  };

  void set_state(PowerState s);
  void release_slot(uint8_t slot);

  EventBus* bus_;
  Thresholds th_{8, 12, 1800};
  PowerState state_ = PowerState::Active;
  int64_t last_activity_ms_ = 0;
  bool ble_connected_ = false;
  bool screen_off_forced_ = false;  // request_screen_off で立つ。kick で降りる
  LeaseSlot leases_[kMaxLeases] = {};
  uint8_t lease_count_ = 0;
};

}  // namespace watch
