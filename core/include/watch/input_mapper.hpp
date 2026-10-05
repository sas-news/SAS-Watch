// input_mapper.hpp — 物理ボタン → Action の割り当て表。
// plan.md G章の既定表。割り当ては settings 経由で変更可 (button.* キー)。
#pragma once

#include <cstdint>

#include "watch/action.hpp"

namespace watch {

enum class PhysicalButton : uint8_t { Boot = 0, Pwr, _Count };
enum class PressType : uint8_t { Short = 0, Long, Double, _Count };

// 生のボタンイベント (firmware の button driver が作る)。
struct ButtonEvent {
  PhysicalButton button;
  PressType press;
};

// 割り当てに使える名前付き Action のテーブルを見る。
// 戻り値は {name, ActionType, arg0} の配列と件数。
struct NamedAction {
  const char* name;
  ActionType type;
  uint32_t arg0;
};
const NamedAction* named_actions();
size_t named_actions_count();
// 名前 → Action。未知なら false。
bool action_from_name(const char* name, Action* out);
// Action → 名前 (button 割り当てで使う代表的なものだけ)。無ければ "none"。
const char* action_to_name(ActionType type, uint32_t arg0);

class InputMapper {
 public:
  InputMapper() { reset_defaults(); }

  // 既定 (plan.md G章):
  //   BOOT: short=primary / long=nav.dev / double=memo.record
  //   PWR : short=back     / long=power_menu / double=none
  void reset_defaults();

  // 割り当てを名前で変更。未知の名前なら false (現在値は維持)。
  bool map(PhysicalButton b, PressType p, const char* action_name);
  // 現在の割り当て名を返す (settings 永続化用)。
  const char* action_name(PhysicalButton b, PressType p) const;

  // ボタンイベントを Action に翻訳。None 相当なら false。
  bool translate(const ButtonEvent& ev, Action* out) const;

 private:
  Action table_[static_cast<int>(PhysicalButton::_Count)]
               [static_cast<int>(PressType::_Count)] = {};
};

}  // namespace watch
