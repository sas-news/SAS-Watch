// Alarm Feature。最大5件の時刻アラーム (曜日繰り返し + ON/OFF)。
// スケジュールは「次回発火の epoch 秒」を保持し tick で比較するだけ。
// Deep Sleep 中は next_deadline_ms → RTC timer wakeup で起きる。
// 鳴動中は Lease を取って Deep Sleep に行かせない。
#include "watch/features/alarm.hpp"

#include <cstring>

#include "watch/power.hpp"
#include "watch/settings.hpp"

namespace watch {
namespace features {

namespace {

AlarmEntry g_entries[kAlarmMax];
uint8_t g_count = 0;
uint32_t g_next_id = 1;

// スケジュール済みの次回発火 (epoch 秒)。0 = なし。
int64_t g_fire_epoch = 0;
uint32_t g_fire_id = 0;
// 直前に鳴らした発火時刻。同じ枠 (同じ秒) の再発火を防ぐ。
int64_t g_last_fire_epoch = 0;
// スヌーズ中の発火予定 (epoch 秒)。0 = なし。
int64_t g_snooze_epoch = 0;
uint32_t g_snooze_id = 0;
// monotonic 側のデッドライン。next_deadline_ms() は ctx を取れないので
// 再計算のたびに mono へ変換してここにキャッシュする。
int64_t g_next_mono = 0;

// 鳴動状態 (永続化しない)。
bool g_ringing = false;
uint32_t g_ring_id = 0;
int64_t g_ring_until_ms = 0;
PowerPolicy::Lease g_ring_lease;

constexpr const char* kKey = "feat.alarm";
constexpr uint8_t kPersistVersion = 1;

struct Persist {
  uint8_t version;
  uint8_t count;
  uint8_t pad[2];
  uint32_t next_id;
  uint32_t fire_id;
  uint32_t snooze_id;
  int64_t fire_epoch;
  int64_t snooze_epoch;
  int64_t last_fire_epoch;
  AlarmEntry entries[kAlarmMax];
};

int64_t tz_s(const FeatureContext& ctx) {
  return ctx.settings ? static_cast<int64_t>(ctx.settings->tz_offset_min) * 60
                      : 0;
}

// local_now (epoch+tz) の時点で最初に鳴るローカル時刻を返す。なければ -1。
// 比較は分の粒度: 設定した「今この分」も発火対象にする。
// skip_fl: このローカル時刻と一致する枠は飛ばす (鳴らした直後の再発火防止)。
int64_t next_fire_local(const AlarmEntry& e, int64_t local_now,
                        int64_t skip_fl) {
  const int64_t day0 = local_now / 86400;
  const int64_t sod = local_now - day0 * 86400;
  const int64_t target = static_cast<int64_t>(e.hour) * 3600 + e.min * 60;
  for (int d = 0; d < 8; ++d) {
    const int64_t day = day0 + d;
    const int wday = static_cast<int>((day + 4) % 7);  // 1970-01-01 = 木 = 4
    if (e.dow != 0 && ((e.dow >> wday) & 1) == 0) continue;
    if (d == 0 && target + 60 <= sod) continue;  // 今日のその時刻は過ぎた
    const int64_t fl = day * 86400 + target;
    if (fl == skip_fl) continue;
    return fl;
  }
  return -1;
}

// g_entries + スヌーズから次回発火を計算し直す。
void reschedule(FeatureContext& ctx) {
  const int64_t now_e = ctx.clock.epoch_s();
  const int64_t tz = tz_s(ctx);
  const int64_t local_now = now_e + tz;
  int64_t best = INT64_MAX;
  uint32_t best_id = 0;
  for (uint8_t i = 0; i < g_count; ++i) {
    const AlarmEntry& e = g_entries[i];
    if (!e.enabled) continue;
    const int64_t fl =
        next_fire_local(e, local_now, g_last_fire_epoch + tz);
    if (fl < 0) continue;
    const int64_t fe = fl - tz;
    if (fe < best) {
      best = fe;
      best_id = e.id;
    }
  }
  if (g_snooze_epoch > 0 && g_snooze_epoch != g_last_fire_epoch &&
      g_snooze_epoch < best) {
    best = g_snooze_epoch;
    best_id = g_snooze_id;
  }
  g_fire_epoch = best == INT64_MAX ? 0 : best;
  g_fire_id = best_id;
  g_next_mono = g_fire_epoch > 0
                    ? ctx.clock.now_ms() + (g_fire_epoch - now_e) * 1000
                    : 0;
}

void persist(FeatureContext& ctx) {
  Persist p{};
  p.version = kPersistVersion;
  p.count = g_count;
  p.next_id = g_next_id;
  p.fire_id = g_fire_id;
  p.snooze_id = g_snooze_id;
  p.fire_epoch = g_fire_epoch;
  p.snooze_epoch = g_snooze_epoch;
  p.last_fire_epoch = g_last_fire_epoch;
  std::memcpy(p.entries, g_entries, sizeof(g_entries));
  ctx.storage.set(kKey, &p, sizeof(p));
}

void start_ring(FeatureContext& ctx) {
  g_ringing = true;
  g_ring_id = g_fire_id;
  g_ring_until_ms = ctx.clock.now_ms() + kAlarmRingTimeoutMs;
  if (ctx.power && !g_ring_lease.valid()) {
    g_ring_lease = ctx.power->acquire(Res::CpuMax, "alarm");
  }
  ctx.bus.publish({EventType::AlarmRinging, g_ring_id});
  ctx.bus.publish({EventType::AlarmChanged, 0});
}

void stop_ring(FeatureContext& ctx) {
  if (!g_ringing) return;
  g_ringing = false;
  g_ring_lease.release();
  reschedule(ctx);
  persist(ctx);
  ctx.bus.publish({EventType::AlarmChanged, 0});
}

void snooze(FeatureContext& ctx) {
  if (!g_ringing) return;
  g_ringing = false;
  g_ring_lease.release();
  g_snooze_epoch = ctx.clock.epoch_s() + kAlarmSnoozeS;
  g_snooze_id = g_ring_id;
  reschedule(ctx);
  persist(ctx);
  ctx.bus.publish({EventType::AlarmChanged, 0});
}

bool handle(const Action& a, FeatureContext& ctx) {
  switch (a.type) {
    case ActionType::AlarmStop:
      stop_ring(ctx);
      return true;
    case ActionType::AlarmSnooze:
      snooze(ctx);
      return true;
    case ActionType::AlarmToggle: {
      AlarmEntry e;
      if (!alarm_find(a.arg0, &e)) return true;
      alarm_set(e.id, e.hour, e.min, e.dow, !e.enabled, ctx);
      return true;
    }
    default:
      return false;
  }
}

void tick(int64_t now_ms, FeatureContext& ctx) {
  if (g_ringing) {
    if (now_ms >= g_ring_until_ms) stop_ring(ctx);  // 自動停止
    return;
  }
  const int64_t now_e = ctx.clock.epoch_s();
  if (g_fire_epoch > 0 && now_e >= g_fire_epoch) {
    g_last_fire_epoch = g_fire_epoch;
    if (g_snooze_epoch == g_fire_epoch) g_snooze_epoch = 0;
    start_ring(ctx);
    reschedule(ctx);
    persist(ctx);
  }
}

int64_t next_deadline() { return g_next_mono; }

void save(FeatureContext& ctx) { persist(ctx); }

void restore(FeatureContext& ctx) {
  // 初期化時のみの復元先は静的 (GCC13 -Wdangling-pointer 対策)。
  static Persist p;
  p = Persist{};
  size_t n = 0;
  if (!ctx.storage.get(kKey, &p, sizeof(p), &n) || n < sizeof(p) ||
      p.version != kPersistVersion || p.count > kAlarmMax) {
    return;
  }
  g_count = p.count;
  g_next_id = p.next_id > 0 ? p.next_id : 1;
  g_fire_epoch = p.fire_epoch;
  g_fire_id = p.fire_id;
  g_snooze_epoch = p.snooze_epoch;
  g_snooze_id = p.snooze_id;
  g_last_fire_epoch = p.last_fire_epoch;
  std::memcpy(g_entries, p.entries, sizeof(g_entries));
  // 保存した発火時刻が過ぎていれば次の tick で鳴る (deep sleep 中に来た
  // 時刻 = RTC timer wakeup で起きた直後、が典型)。
  g_next_mono = g_fire_epoch > 0
                    ? ctx.clock.now_ms() +
                          (g_fire_epoch - ctx.clock.epoch_s()) * 1000
                    : 0;
}

}  // namespace

size_t alarm_count() { return g_count; }

bool alarm_at(size_t i, AlarmEntry* out) {
  if (!out || i >= g_count) return false;
  *out = g_entries[i];
  return true;
}

bool alarm_find(uint32_t id, AlarmEntry* out) {
  for (uint8_t i = 0; i < g_count; ++i) {
    if (g_entries[i].id == id) {
      if (out) *out = g_entries[i];
      return true;
    }
  }
  return false;
}

int32_t alarm_set(uint32_t id, uint8_t hour, uint8_t min, uint8_t dow,
                  bool enabled, FeatureContext& ctx) {
  if (hour > 23 || min > 59 || dow > 0x7F) return -1;
  int32_t out_id;
  if (id == 0) {
    if (g_count >= kAlarmMax) return -2;
    g_entries[g_count] = AlarmEntry{};
    g_entries[g_count].id = g_next_id++;
    ++g_count;
    out_id = static_cast<int32_t>(g_entries[g_count - 1].id);
  } else {
    out_id = -3;
    for (uint8_t i = 0; i < g_count; ++i) {
      if (g_entries[i].id == id) {
        out_id = static_cast<int32_t>(id);
        break;
      }
    }
    if (out_id < 0) return -3;
  }
  for (uint8_t i = 0; i < g_count; ++i) {
    if (g_entries[i].id == static_cast<uint32_t>(out_id)) {
      g_entries[i].hour = hour;
      g_entries[i].min = min;
      g_entries[i].dow = dow;
      g_entries[i].enabled = enabled ? 1 : 0;
    }
  }
  reschedule(ctx);
  persist(ctx);
  ctx.bus.publish({EventType::AlarmChanged, 0});
  return out_id;
}

int32_t alarm_delete(uint32_t id, FeatureContext& ctx) {
  for (uint8_t i = 0; i < g_count; ++i) {
    if (g_entries[i].id == id) {
      for (uint8_t j = i; j + 1 < g_count; ++j) g_entries[j] = g_entries[j + 1];
      --g_count;
      g_entries[g_count] = AlarmEntry{};
      if (g_snooze_id == id) g_snooze_epoch = 0;
      reschedule(ctx);
      persist(ctx);
      ctx.bus.publish({EventType::AlarmChanged, 0});
      return 1;
    }
  }
  return 0;
}

bool alarm_ringing() { return g_ringing; }
uint32_t alarm_ringing_id() { return g_ring_id; }

int64_t alarm_next_fire_epoch() { return g_fire_epoch; }

bool alarm_dow_has(uint8_t dow, int wday) {
  return dow == 0 || ((dow >> wday) & 1) != 0;
}

void alarm_reset_state() {
  g_count = 0;
  g_next_id = 1;
  g_fire_epoch = 0;
  g_fire_id = 0;
  g_last_fire_epoch = 0;
  g_snooze_epoch = 0;
  g_snooze_id = 0;
  g_next_mono = 0;
  g_ringing = false;
  g_ring_id = 0;
  g_ring_until_ms = 0;
  g_ring_lease.release();
  for (auto& e : g_entries) e = AlarmEntry{};
}

const FeatureDescriptor kAlarm = {
    /*id*/ "alarm",
    /*capabilities*/ HasScreen | HasQuickTile,
    /*route*/ Route::Alarm,
    /*init*/ nullptr,
    /*handle*/ handle,
    /*tick*/ tick,
    /*next_deadline_ms*/ next_deadline,
    /*save*/ save,
    /*restore*/ restore,
};

}  // namespace features
}  // namespace watch
