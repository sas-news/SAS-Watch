// features/walk.hpp — 歩く大会モード (Walk)。docs/walk-event.md §4.3。
// 測位・ルート・進捗計算の正はスマホ。時計は受け取った表示状態を持つだけで、
// 画面を開いている間だけ 10 秒間隔で walk.query (EVT) を送る。
// 状態は永続化しない (再起動後は次の問い合わせで取り直す)。
#pragma once

#include "watch/feature.hpp"

namespace watch {
namespace features {

enum class WalkStatus : uint8_t {
  Unknown = 0,  // まだ walk.state を受け取っていない
  Idle,
  Walking,
  Finished,
  NoRoute,
  NoFix,
};

// 電話から受けた表示状態 (protocol walk.state)。文字列は UTF-8 で NUL 終端。
// route 33B / next_cp 25B / msg 49B は protocol-v1.md の上限 + NUL。
struct WalkState {
  WalkStatus status = WalkStatus::Unknown;
  char route[33] = {};
  char next_cp[25] = {};
  char msg[49] = {};
  uint32_t total_m = 0, done_m = 0, remain_m = 0, permille = 0;
  uint32_t elapsed_s = 0, pace_s_per_km = 0, next_cp_m = 0;
  uint32_t off_route_m = 0, accuracy_m = 0;
  int64_t eta_epoch = 0, started_epoch = 0, updated_epoch = 0;
};

extern const FeatureDescriptor kWalk;

// UI 用スナップショット (値をコピーして返す)。
WalkState walk_state();

// BLE タスクから呼ぶ。状態を差し替えて WalkStateChanged を出す。
// EventBus への publish は呼び出し側 (ble_glue) が core_lock 内で行う既存の
// パターンに合わせる。ctx は FeatureContext (bus を引くため)。
void walk_set_state(const WalkState& s, FeatureContext& ctx);

// 即時問い合わせ。直近 1.2 秒以内に送っていれば捨てる (docs/walk-event.md §3.1)。
void walk_request_refresh(FeatureContext& ctx);

// 内部状態を初期値に戻す (主にテストと工場リセット用)。
void walk_reset_state();

}  // namespace features
}  // namespace watch
