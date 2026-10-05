// protocol/dispatch.hpp — REQ (CBOR {"m":method,"p":params}) のディスパッチ。
// ハード値 (電池・ヒープ等) はコールバック経由。core は実値を知らない。
#pragma once

#include <cstddef>
#include <cstdint>

#include "watch/event_bus.hpp"
#include "watch/input_mapper.hpp"
#include "watch/platform.hpp"
#include "watch/power.hpp"
#include "watch/protocol/cbor.hpp"
#include "watch/settings.hpp"

namespace watch {
namespace proto {

// 実機依存の値・操作を渡すコールバック群。nullptr は「未対応」を意味する。
struct Services {
  Clock* clock = nullptr;
  KeyValueStore* kv = nullptr;
  Settings* settings = nullptr;
  EventBus* bus = nullptr;             // SettingsChanged 等を出すのに使う
  PowerPolicy* power = nullptr;        // settings.set を dim/screen_off 等に反映
  InputMapper* input = nullptr;        // settings.set を button.* に反映

  // device.info 用 (実機値は firmware が返す)
  int (*battery_percent)(void* ctx) = nullptr;      // -1 = 不明
  bool (*is_charging)(void* ctx) = nullptr;
  const char* (*fw_version)(void* ctx) = nullptr;
  int64_t (*free_heap)(void* ctx) = nullptr;
  int64_t (*free_psram)(void* ctx) = nullptr;

  // Feature 連携 (戻り値 false = 失敗 → "internal")
  bool (*timer_start)(uint32_t seconds, void* ctx) = nullptr;
  bool (*timer_stop)(void* ctx) = nullptr;
  // memo id (>=0) か -1。
  int32_t (*memo_create)(const char* text, size_t len, void* ctx) = nullptr;

  // ---- memo.list / memo.get / memo.delete / memo.audio.get (Phase 9) ----
  // memo.list: 総件数。無ければ負。
  int32_t (*memo_count)(void* ctx) = nullptr;
  // memo.list: 新しい順に i 番目のエントリを {id,kind,sec,size} で w に書く。
  // 範囲外/失敗は false。
  bool (*memo_entry)(uint32_t i, cbor::Writer& w, void* ctx) = nullptr;
  // memo.get: id のエントリを {id,kind,sec,size,text?} で w に書く。
  bool (*memo_get)(uint32_t id, cbor::Writer& w, void* ctx) = nullptr;
  // memo.delete: 1=消えた / 0=無い / -1=内部エラー。
  int32_t (*memo_delete)(uint32_t id, void* ctx) = nullptr;
  // memo.audio.get: 音声メモの {size, sha256} を返す。voice でなければ false。
  bool (*memo_audio_info)(uint32_t id, uint32_t* size, uint8_t sha256[32],
                          void* ctx) = nullptr;
  // memo.audio.get: RES のあとに BULK push 転送を開始する。失敗は false。
  bool (*memo_audio_send)(uint32_t id, void* ctx) = nullptr;

  // ---- alarm.list / alarm.set / alarm.delete ----
  // alarm.list: 総件数 (>=0)。
  int32_t (*alarm_count)(void* ctx) = nullptr;
  // alarm.list: i 番目のエントリを {id,hour,min,dow,on} で w に書く。
  bool (*alarm_entry)(uint32_t i, cbor::Writer& w, void* ctx) = nullptr;
  // alarm.set: 確定 id (>0) / -1 引数不正 / -2 満杯 / -3 指定 id 無し。
  int32_t (*alarm_set)(uint32_t id, uint8_t hour, uint8_t min, uint8_t dow,
                       bool on, void* ctx) = nullptr;
  // alarm.delete: 1=消えた / 0=無い。
  int32_t (*alarm_delete)(uint32_t id, void* ctx) = nullptr;

  // 任意の通知 (未設定でも RES ok を返す)
  void (*notify_posted)(const char* app, const char* title, const char* body,
                        void* ctx) = nullptr;
  void (*media_state)(const char* title, const char* artist, bool playing,
                      void* ctx) = nullptr;
  // settings.* が1キー変わるごとに呼ぶ (反映用)
  void (*setting_changed)(const char* key, void* ctx) = nullptr;

  void* ctx = nullptr;
};

enum class DispatchError : uint8_t {
  Ok = 0,
  BadRequest,      // CBOR が壊れている / 形が違う
  UnknownMethod,
  UnsupportedProto,
  Busy,
  Internal,
  NotFound,        // id が無い (memo.delete など)
};

// REQ ペイロードを処理して RES ペイロード (CBOR) を out に書く。
// 戻り値は内部エラー分類用。どの場合でも out に RES は書き込む。
DispatchError dispatch_req(const uint8_t* req, size_t req_len, Services& svc,
                           cbor::Writer* out);

// EVT ペイロード {"e":name,"d":{...}} を書く。d は呼び出し側が書く
// (write_d コールバックの中で Writer に追記する)。d 無しなら write_d=nullptr。
bool encode_evt(cbor::Writer* w, const char* event_name,
                void (*write_d)(cbor::Writer&, void*), void* ctx);

// settings.set で変わった値を power/input へ反映する共通処理。
// Services に power/input が入っていれば dispatch 内からも呼ばれる。
void apply_settings_side_effects(const Settings& s, Services& svc);

}  // namespace proto
}  // namespace watch
