// event.hpp — 「起きたこと」。Feature が出し、UI/BLE が購読する。
// plan.md C章。
#pragma once

#include <cstdint>

namespace watch {

enum class EventType : uint16_t {
  None = 0,
  RouteChanged,          // arg0 = Route
  PowerStateChanged,     // arg0 = PowerState
  SettingsChanged,       // arg0 = SettingKey のハッシュ (0 = 全体)
  BrightnessChanged,     // arg0 = 0-100
  ThemeChanged,          // arg0 = 0, theme id は Settings 参照
  ClockTick,             // 秒が変わるごと (arg0 = epoch_s 下位)
  BatteryChanged,        // arg0 = 0-100
  ChargingChanged,       // arg0 = 0/1
  BleConnChanged,        // arg0 = 0/1
  TimerStarted,          // arg0 = 秒
  TimerStopped,
  TimerFinished,
  StopwatchChanged,      // arg0 = 0:stop 1:run 2:lap
  CounterChanged,        // arg0 = 値 (uint32解釈)
  MemoSaved,             // arg0 = memo id
  MemoDeleted,           // arg0 = memo id (一覧の再描画用)
  NotificationPosted,    // 通知が来た (中身は NotificationStore 側)
  NotificationsCleared,  // 通知一覧を全消しした
  MediaCmdRequested,     // arg0 = MediaCmd (時計→スマホ)
  MediaStateChanged,     // media.state を受け取って表示が変わった
  AlarmRinging,          // arg0 = alarm id。鳴動開始 (繰り返し再通知も出る)
  AlarmChanged,          // 一覧変更・鳴動停止・スヌーズ設定など表示更新用
  AgentStatusChanged,
  StepsChanged,          // arg0 = 今日の歩数
  RaiseDetected,         // 腕上げ判定 (firmware が画面ONに使う。arg0=0)
  OtaProgress,           // arg0 = 0-100 (状態詳細は firmware 側参照)
};

struct Event {
  EventType type = EventType::None;
  uint32_t arg0 = 0;
};

}  // namespace watch
