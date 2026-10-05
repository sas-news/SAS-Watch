#include "watch/runtime.hpp"

#include "watch/theme/manifest.hpp"

namespace watch {

bool ActionQueue::push(const Action& a) {
  if (lock_) lock_(lock_ctx_);
  bool ok = false;
  if (count_ < kCapacity) {
    buf_[(head_ + count_) % kCapacity] = a;
    ++count_;
    ok = true;
  }
  if (unlock_) unlock_(lock_ctx_);
  return ok;
}

bool ActionQueue::pop(Action* out) {
  if (lock_) lock_(lock_ctx_);
  bool ok = false;
  if (count_ > 0 && out) {
    *out = buf_[head_];
    head_ = (head_ + 1) % kCapacity;
    --count_;
    ok = true;
  }
  if (unlock_) unlock_(lock_ctx_);
  return ok;
}

Runtime::Runtime(EventBus& bus, Navigator& nav, PowerPolicy& power,
                 InputMapper& input, const FeatureRegistry& features,
                 Settings& settings)
    : bus_(bus),
      nav_(nav),
      power_(power),
      input_(input),
      features_(features),
      settings_(settings) {}

void Runtime::init(FeatureContext& ctx) { features_.init_all(ctx); }

bool Runtime::step(int64_t now_ms, FeatureContext& ctx) {
  Action a;
  const bool had = queue_.pop(&a);
  if (had) dispatch(a, ctx);
  features_.tick_all(now_ms, ctx);
  power_.update(now_ms);
  return had;
}

int64_t Runtime::next_deadline_ms() const {
  const int64_t f = features_.next_deadline_ms();
  const int64_t p = power_.next_transition_ms();
  if (f == 0) return p;
  if (p == 0) return f;
  return f < p ? f : p;
}

bool Runtime::dispatch(const Action& a, FeatureContext& ctx) {
  // 外部由来の Action (System 以外): 電源を蹴る。画面OFF中の入力は
  // 「起こすだけ」でアクションとしては処理しない (実機の挙動に合わせる)。
  if (a.source != ActionSource::System) {
    const PowerState before = power_.state();
    power_.kick_activity(ctx.clock.now_ms());
    if (before == PowerState::ScreenOff ||
        before == PowerState::DeepSleepCandidate) {
      return true;
    }
  }

  // 画面の主アクション: 現在の Route に結び付いた Feature へ。
  if (a.type == ActionType::PrimaryAction) {
    const FeatureDescriptor* f = features_.find_by_route(nav_.current());
    if (f && f->handle && f->handle(a, ctx)) return true;
  }

  // 画面遷移系。
  if (nav_.handle_action(a)) return true;

  switch (a.type) {
    case ActionType::Back:
    case ActionType::ScreenOff:
      // Home 末端の Back = 画面OFF (plan.md G章)。
      power_.request_screen_off(ctx.clock.now_ms());
      return true;
    case ActionType::TimeSync: {
      const int64_t epoch = static_cast<int64_t>(a.arg0) |
                          (static_cast<int64_t>(a.arg1) << 32);
      return ctx.clock.set_epoch_s(epoch);
    }
    case ActionType::SetBrightness:
      if (settings_set_u32(settings_, &ctx.storage, "brightness",
                           a.arg0 > 100 ? 100 : a.arg0)) {
        bus_.publish({EventType::BrightnessChanged,
                      a.arg0 > 100 ? 100 : a.arg0});
        return true;
      }
      return false;
    case ActionType::SetScreenOffAfter: {
      const uint32_t s = a.arg0 < 5 ? 5 : a.arg0 > 600 ? 600 : a.arg0;
      if (settings_set_u32(settings_, &ctx.storage, "screen_off_after_s", s)) {
        // しきい値の変更は即 PowerPolicy に反映 (画面OFFまでの秒数の設定画面)。
        power_.configure(settings_.dim_after_s, settings_.screen_off_after_s,
                         settings_.deep_sleep_after_s);
        bus_.publish({EventType::SettingsChanged, 0});
        return true;
      }
      return false;
    }
    case ActionType::SetAudioVolume:
      if (settings_set_u32(settings_, &ctx.storage, "audio.volume",
                           a.arg0 > 100 ? 100 : a.arg0)) {
        bus_.publish({EventType::SettingsChanged, 0});
        return true;
      }
      return false;
    case ActionType::SetAudioClick:
      if (settings_set_u32(settings_, &ctx.storage, "audio.click",
                           a.arg0 ? 1 : 0)) {
        bus_.publish({EventType::SettingsChanged, 0});
        return true;
      }
      return false;
    case ActionType::SetTheme:
      // theme id は [a-z0-9-]{1,31} (docs/theme-format.md)。
      // 実在するかは適用層が判定し、失敗時は standard にフォールバックする。
      if (!theme_id_ok(a.text)) return false;
      if (settings_set_str(settings_, &ctx.storage, "theme", a.text)) {
        bus_.publish({EventType::ThemeChanged, 0});
        return true;
      }
      return false;
    case ActionType::SetFace:
      // 文字盤 id。実在するかは UI 側が判定し、未知なら "bold" に倒す。
      if (a.text[0] == '\0') return false;
      if (settings_set_str(settings_, &ctx.storage, "face", a.text)) {
        bus_.publish({EventType::FaceChanged, 0});
        return true;
      }
      return false;
    case ActionType::SetClockFont:
      // 時計数字フォント id。未知なら UI 側で "auto" に倒す。
      if (a.text[0] == '\0') return false;
      if (settings_set_str(settings_, &ctx.storage, "clock_font", a.text)) {
        bus_.publish({EventType::FaceChanged, 0});
        return true;
      }
      return false;
    default:
      break;
  }

  return features_.handle(a, ctx);
}

}  // namespace watch
