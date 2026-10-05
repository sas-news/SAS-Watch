#include "watch/power.hpp"

#include <cstring>

namespace watch {

void PowerPolicy::configure(uint32_t dim_after_s, uint32_t screen_off_after_s,
                            uint32_t deep_sleep_after_s) {
  th_.dim_after_s = dim_after_s;
  th_.screen_off_after_s = screen_off_after_s;
  th_.deep_sleep_after_s = deep_sleep_after_s;
}

PowerState PowerPolicy::kick_activity(int64_t now_ms) {
  const PowerState before = state_;
  last_activity_ms_ = now_ms;
  screen_off_forced_ = false;
  set_state(PowerState::Active);
  return before;
}

void PowerPolicy::request_screen_off(int64_t /*now_ms*/) {
  // ユーザー要求での消灯。無操作タイマー由来ではないので、
  // 次の update() で Active に戻らないよう forced 印を付ける。
  screen_off_forced_ = true;
  set_state(PowerState::ScreenOff);
}

PowerState PowerPolicy::update(int64_t now_ms) {
  const int64_t idle_ms = now_ms - last_activity_ms_;
  const bool can_deep = !ble_connected_ && lease_count_ == 0;
  PowerState next;
  if (can_deep &&
      idle_ms >= static_cast<int64_t>(th_.deep_sleep_after_s) * 1000) {
    next = PowerState::DeepSleepCandidate;
  } else if (screen_off_forced_ ||
             idle_ms >= static_cast<int64_t>(th_.screen_off_after_s) * 1000) {
    next = PowerState::ScreenOff;
  } else if (idle_ms >= static_cast<int64_t>(th_.dim_after_s) * 1000) {
    next = PowerState::Dim;
  } else {
    next = PowerState::Active;
  }
  set_state(next);
  return state_;
}

int64_t PowerPolicy::next_transition_ms() const {
  const int64_t s = 1000;
  switch (state_) {
    case PowerState::Active:
      return last_activity_ms_ + static_cast<int64_t>(th_.dim_after_s) * s;
    case PowerState::Dim:
      return last_activity_ms_ + static_cast<int64_t>(th_.screen_off_after_s) * s;
    case PowerState::ScreenOff:
      if (!ble_connected_ && lease_count_ == 0) {
        return last_activity_ms_ +
               static_cast<int64_t>(th_.deep_sleep_after_s) * s;
      }
      return 0;
    case PowerState::DeepSleepCandidate:
      return 0;
  }
  return 0;
}

void PowerPolicy::Lease::release() {
  if (owner_ != nullptr && slot_ != 0xFF) {
    owner_->release_slot(slot_);
  }
  owner_ = nullptr;
  slot_ = 0xFF;
}

PowerPolicy::Lease PowerPolicy::acquire(Res res, const char* who) {
  for (uint8_t i = 0; i < kMaxLeases; ++i) {
    if (!leases_[i].used) {
      leases_[i].used = true;
      leases_[i].res = res;
      if (who) {
        std::strncpy(leases_[i].who, who, sizeof(leases_[i].who) - 1);
        leases_[i].who[sizeof(leases_[i].who) - 1] = '\0';
      } else {
        leases_[i].who[0] = '\0';
      }
      ++lease_count_;
      // Lease 中は DeepSleep に入らない。候補状態なら ScreenOff に戻す。
      if (state_ == PowerState::DeepSleepCandidate) {
        set_state(PowerState::ScreenOff);
      }
      return Lease(this, i);
    }
  }
  return Lease();  // 空きなし → 無効な Lease
}

void PowerPolicy::for_each_lease(void (*fn)(Res, const char*, void*),
                                 void* ctx) const {
  if (!fn) return;
  for (uint8_t i = 0; i < kMaxLeases; ++i) {
    if (leases_[i].used) fn(leases_[i].res, leases_[i].who, ctx);
  }
}

void PowerPolicy::release_slot(uint8_t slot) {
  if (slot >= kMaxLeases || !leases_[slot].used) return;
  leases_[slot].used = false;
  leases_[slot].who[0] = '\0';
  if (lease_count_ > 0) --lease_count_;
}

void PowerPolicy::set_state(PowerState s) {
  if (state_ == s) return;
  state_ = s;
  if (bus_) {
    bus_->publish({EventType::PowerStateChanged, static_cast<uint32_t>(s)});
  }
}

}  // namespace watch
