// internal.hpp — watch_app 内部の公開口 (ble_glue / input / power_apply が使う)。
#pragma once

#include "watch/input_mapper.hpp"
#include "watch/power.hpp"
#include "watch/runtime.hpp"
#include "watch_app/watch_app.hpp"

namespace watch_app {

// app タスクを起こす (pending 通知・queue push 後に呼ぶ)。
void wake_task();

// core 状態 (Runtime/Feature/Settings) の排他。
// app タスクの step 全体と BLE dispatch_req を直列化する。
void core_lock();
void core_unlock();

watch::PowerPolicy& power();
watch::InputMapper& input();
// input.cpp: mapper の Double 割り当てをボタン即時化の atomic キャッシュへ
// 反映する。core_lock 下 (mapper を書く側と同じ排他) で呼ぶ。
void input_sync_double_enabled();
watch::FeatureRegistry& features();
watch::FeatureContext* fctx();
watch::Clock* clock();
watch::KeyValueStore* kv();
watch::Settings& settings_mut();

// ble_glue.cpp: BLE 有効時に EventBus → EVT notify を配線する (start 内で1回)。
void ble_glue_init();
// ble_glue.cpp: app タスクのループで保留分 (接続状態/パスキー/bulk送信) を処理する。
void ble_glue_poll();

// imu_service.cpp: QMI8658 の常時サービス (raise-to-wake + 歩数入力)。
void imu_service_start();
// deep sleep 移行中は imu_task が WoM arm を上書きするのを止める
// (app 側の init_high arm が眠る直前に通常 arm へ戻されるのを防ぐ)。
void imu_service_freeze_for_deep_sleep();

}  // namespace watch_app
