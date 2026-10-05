#include "watch/input_mapper.hpp"

#include <cstring>

#include "watch/navigation.hpp"

namespace watch {

namespace {

// settings の button.* に入る「Action 名」。画面を開く系は nav.*、
// 機能系は feature 名に寄せる。phone 側はこの名前をそのまま保存する。
constexpr NamedAction kNamed[] = {
    {"none", ActionType::None, 0},
    {"back", ActionType::Back, 0},
    {"home", ActionType::Home, 0},
    {"primary", ActionType::PrimaryAction, 0},
    {"screen_off", ActionType::ScreenOff, 0},
    {"wake", ActionType::Wake, 0},
    {"power_menu", ActionType::Navigate, static_cast<uint32_t>(Route::PowerMenu)},
    {"nav.quick", ActionType::Navigate, static_cast<uint32_t>(Route::Quick)},
    {"nav.notifications", ActionType::Navigate,
     static_cast<uint32_t>(Route::Notifications)},
    {"nav.more", ActionType::Navigate, static_cast<uint32_t>(Route::More)},
    {"nav.dev", ActionType::Navigate, static_cast<uint32_t>(Route::Dev)},
    {"nav.agent", ActionType::Navigate, static_cast<uint32_t>(Route::Agent)},
    {"nav.settings", ActionType::Navigate,
     static_cast<uint32_t>(Route::Settings)},
    {"nav.media", ActionType::Navigate, static_cast<uint32_t>(Route::Media)},
    {"nav.steps", ActionType::Navigate, static_cast<uint32_t>(Route::Steps)},
    {"nav.ota", ActionType::Navigate, static_cast<uint32_t>(Route::Ota)},
    {"memo.record", ActionType::MemoRecordStart, 0},
    {"timer.start", ActionType::TimerStart, 0},
    {"timer.stop", ActionType::TimerStop, 0},
    {"stopwatch.toggle", ActionType::StopwatchToggle, 0},
    {"counter.add", ActionType::CounterAdd, 1},
    {"counter.sub", ActionType::CounterAdd,
     static_cast<uint32_t>(-1)},
};

}  // namespace

const NamedAction* named_actions() { return kNamed; }
size_t named_actions_count() { return sizeof(kNamed) / sizeof(kNamed[0]); }

bool action_from_name(const char* name, Action* out) {
  if (!name || !out) return false;
  for (size_t i = 0; i < named_actions_count(); ++i) {
    if (std::strcmp(kNamed[i].name, name) == 0) {
      out->type = kNamed[i].type;
      out->source = ActionSource::Button;
      out->arg0 = kNamed[i].arg0;
      return true;
    }
  }
  return false;
}

const char* action_to_name(ActionType type, uint32_t arg0) {
  for (size_t i = 0; i < named_actions_count(); ++i) {
    if (kNamed[i].type == type && kNamed[i].arg0 == arg0) {
      return kNamed[i].name;
    }
  }
  return "none";
}

void InputMapper::reset_defaults() {
  map(PhysicalButton::Boot, PressType::Short, "primary");
  map(PhysicalButton::Boot, PressType::Long, "nav.dev");
  map(PhysicalButton::Boot, PressType::Double, "memo.record");
  map(PhysicalButton::Pwr, PressType::Short, "back");
  map(PhysicalButton::Pwr, PressType::Long, "power_menu");
  map(PhysicalButton::Pwr, PressType::Double, "none");
}

bool InputMapper::map(PhysicalButton b, PressType p, const char* action_name) {
  Action a;
  if (!action_from_name(action_name, &a)) return false;
  table_[static_cast<int>(b)][static_cast<int>(p)] = a;
  return true;
}

const char* InputMapper::action_name(PhysicalButton b, PressType p) const {
  const Action& a = table_[static_cast<int>(b)][static_cast<int>(p)];
  return action_to_name(a.type, a.arg0);
}

bool InputMapper::translate(const ButtonEvent& ev, Action* out) const {
  if (!out) return false;
  if (ev.button >= PhysicalButton::_Count || ev.press >= PressType::_Count) {
    return false;
  }
  const Action& a =
      table_[static_cast<int>(ev.button)][static_cast<int>(ev.press)];
  if (a.type == ActionType::None) return false;
  *out = a;
  out->source = ActionSource::Button;
  return true;
}

}  // namespace watch
