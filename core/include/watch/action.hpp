// action.hpp — 「やってほしいこと」。入力元 (Touch/Button/Imu/BLE/Voice) は問わない。
// plan.md C章。malloc しない: 文字列は固定長バッファ。
#pragma once

#include <cstdint>
#include <cstring>

namespace watch {

enum class ActionType : uint16_t {
  None = 0,
  // Navigation
  Navigate,       // arg0 = Route
  Back,
  Home,
  ScreenOff,      // 画面を消す要求 (Home で Back 相当)
  PrimaryAction,  // 画面ごとの主アクション (Timer開始/停止 など)
  Wake,           // 明示的な復帰要求 (処理はしない)
  // Timer
  TimerStart,     // arg0 = 秒 (0 = 前回/既定)
  TimerStop,
  TimerReset,
  // Stopwatch
  StopwatchToggle,
  StopwatchLap,
  StopwatchReset,
  // Counter
  CounterAdd,     // arg0 = 増減 (int32_t 解釈)
  CounterReset,
  // Memo
  MemoCreate,       // text = 本文
  MemoDelete,       // arg0 = memo id (閲覧画面の削除)
  MemoRecordStart,  // 録音開始 (firmware 側で Audio Lease + 録音)
  MemoRecordStop,
  MemoPlay,         // arg0 = 音声メモの id
  MemoStopPlay,     // 再生中の音声メモを止める
  // Agent (将来)
  AgentSend,
  AgentApprove,
  AgentReject,
  // Settings / System
  SetBrightness,  // arg0 = 0-100
  SetScreenOffAfter, // arg0 = 画面OFFまでの秒数 (5-600 にクランプ)
  SetAudioVolume, // arg0 = 0-100 (クリック音・ビープ・メモ再生の音量)
  SetAudioClick,  // arg0 = 0/1 (ボタンのクリック音 ON/OFF)
  SetTheme,       // text = theme id
  SetFace,        // text = 文字盤 id ("bold" など)
  SetClockFont,   // text = 時計数字フォント id ("auto" など)
  TimeSync,       // arg0/arg1 = epoch 秒 (low/high)
  MediaCommand,   // arg0 = MediaCmd
  // Alarm (鳴動中の操作 + 一覧の ON/OFF)
  AlarmStop,      // 鳴動を止める
  AlarmSnooze,    // 5分後にもう一度鳴らす
  AlarmToggle,    // arg0 = alarm id。ON/OFF を切替
  // Notifications
  NotifyClearAll, // 通知一覧を全消し
  // Settings / System (追加)
  SetNotifyVibrate, // arg0 = 0/1 (通知受信時の振動)
  // Sensors (Phase 10)
  ImuSample,        // arg0 = x|y<<16, arg1 = z (int16 mg)。source=System で
                    // 投げること (測定は「操作」ではないので電源を蹴らない)
  SetRaiseToWake,   // arg0 = 0/1 (腕を上げて画面オン)
  StepsHwSync,      // arg0 = HW pedometer の累積歩数 (24bit 生値)。
                    // firmware が定期/起床時に読んで投げる。source=System 必須
};

enum class ActionSource : uint8_t {
  System = 0,
  Touch,
  Button,
  Imu,
  Voice,
  Phone,
};

// メディア操作 (時計→スマホ)。protocol-v1.md media.cmd と対応。
enum class MediaCmd : uint8_t { PlayPause = 0, Next, Prev, VolUp, VolDown };

// Action 本文の最大長。memo の 256 バイトを収められるサイズ。
constexpr size_t kActionTextMax = 260;

struct Action {
  ActionType type = ActionType::None;
  ActionSource source = ActionSource::System;
  uint32_t arg0 = 0;  // 秒数・Route・delta など小さい値
  int32_t arg1 = 0;   // 予備 (epoch 上位など)
  // 文字列ペイロード。固定長なのでキュー越しにコピーしても安全。
  char text[kActionTextMax] = {};

  void set_text(const char* s) {
    if (!s) {
      text[0] = '\0';
      return;
    }
    size_t n = std::strlen(s);
    if (n >= kActionTextMax) n = kActionTextMax - 1;
    std::memcpy(text, s, n);
    text[n] = '\0';
  }
  void set_text(const char* s, size_t n) {
    if (!s || n == 0) {
      text[0] = '\0';
      return;
    }
    if (n >= kActionTextMax) n = kActionTextMax - 1;
    std::memcpy(text, s, n);
    text[n] = '\0';
  }
};

}  // namespace watch
