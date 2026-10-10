// Memo Feature。テキストは最大64件×最大256バイト(UTF-8)のリングで
// 各スロットを別 KV キーに書く (NVS blob が小さい前提)。
// 音声メモは最大8件・合計1MB。実ファイルは AudioPort (littlefs) に
// 置き、core は id/秒数/バイト数のメタだけを持つ。
// 録音中・再生中は Res::Audio の Lease を取って Deep Sleep しない。
#include "watch/features/memo.hpp"

#include <cstdio>
#include <cstring>

#include "watch/power.hpp"
#include "watch/settings.hpp"

namespace watch {
namespace features {

namespace {

struct Header {
  uint8_t version;
  uint8_t reserved;
  uint16_t count;
  uint16_t head;
  uint16_t reserved2;
  uint32_t next_id;
};
constexpr uint8_t kPersistVersion = 2;
constexpr const char* kHdrKey = "feat.memo.hdr";
constexpr const char* kSlotKeyFmt = "feat.memo.%02u";  // 00..63

// v1 はテキストだけ。v2 で kind/sec/size を追加した。
struct SlotBlobV1 {
  uint32_t id;
  uint16_t len;
  char text[kMemoMaxText];
};
struct SlotBlobV2 {
  uint32_t id;
  uint8_t kind;
  uint8_t reserved;
  uint16_t len;
  uint32_t sec;
  uint32_t size;
  char text[kMemoMaxText];
};
using SlotBlob = SlotBlobV2;

MemoEntry g_entries[kMemoMaxEntries];
uint16_t g_count = 0;  // 有効件数
uint16_t g_head = 0;   // 最古スロット
uint32_t g_next_id = 1;

// 録音/再生の実行時状態 (永続化しない)。
bool s_recording = false;
uint32_t s_rec_id = 0;
PowerPolicy::Lease s_rec_lease;
bool s_playing = false;
uint32_t s_play_id = 0;
PowerPolicy::Lease s_play_lease;

void slot_key(uint16_t slot, char* out, size_t cap) {
  std::snprintf(out, cap, kSlotKeyFmt, static_cast<unsigned>(slot));
}

void persist_header(KeyValueStore& kv) {
  Header h{};
  h.version = kPersistVersion;
  h.count = g_count;
  h.head = g_head;
  h.next_id = g_next_id;
  kv.set(kHdrKey, &h, sizeof(h));
}

void persist_slot(KeyValueStore& kv, uint16_t slot) {
  char key[32];
  slot_key(slot, key, sizeof(key));
  const MemoEntry& e = g_entries[slot];
  SlotBlob b{};
  b.id = e.id;
  b.kind = static_cast<uint8_t>(e.kind);
  b.len = e.len;
  b.sec = e.sec;
  b.size = e.size;
  std::memcpy(b.text, e.text, e.len);
  kv.set(key, &b, sizeof(b));
}

// リングへ1件挿入して永続化 + MemoSaved。e.id は呼び出し側で確定済み。
void push_entry(FeatureContext& ctx, MemoEntry& e) {
  uint16_t slot;
  if (g_count < kMemoMaxEntries) {
    slot = static_cast<uint16_t>((g_head + g_count) % kMemoMaxEntries);
    ++g_count;
  } else {
    // 満杯 → 最古 (head) を消して回す。
    slot = g_head;
    g_head = static_cast<uint16_t>((g_head + 1) % kMemoMaxEntries);
  }
  g_entries[slot] = e;
  g_entries[slot].text[e.len] = '\0';
  persist_slot(ctx.storage, slot);
  persist_header(ctx.storage);
  ctx.bus.publish({EventType::MemoSaved, e.id});
}

size_t voice_count() {
  size_t n = 0;
  for (uint16_t i = 0; i < g_count; ++i) {
    if (g_entries[(g_head + i) % kMemoMaxEntries].kind == MemoKind::Voice) {
      ++n;
    }
  }
  return n;
}

uint32_t voice_bytes() {
  uint32_t n = 0;
  for (uint16_t i = 0; i < g_count; ++i) {
    const MemoEntry& e = g_entries[(g_head + i) % kMemoMaxEntries];
    if (e.kind == MemoKind::Voice) n += e.size;
  }
  return n;
}

// 音声メモが件数/容量を超えていたら古いものから消す。
void evict_voice(FeatureContext& ctx) {
  while (voice_count() > kMemoMaxVoice ||
         voice_bytes() > kMemoVoiceBudgetBytes) {
    bool found = false;
    for (uint16_t i = 0; i < g_count; ++i) {
      const MemoEntry& e = g_entries[(g_head + i) % kMemoMaxEntries];
      if (e.kind == MemoKind::Voice) {
        memo_delete(e.id, ctx);
        found = true;
        break;
      }
    }
    if (!found) break;
  }
}

// 録音を終わらせる。commit なら MemoEntry として登録する。
void finish_record(FeatureContext& ctx, bool commit) {
  const uint32_t id = s_rec_id;
  uint32_t sec = 0, size = 0;
  const bool ok =
      ctx.audio && ctx.audio->record_end(commit, &sec, &size);
  s_recording = false;
  s_rec_id = 0;
  s_rec_lease.release();
  if (!ok || !commit || size == 0 || sec == 0) {
    if (ctx.audio && id) ctx.audio->memo_audio_erase(id);
    return;
  }
  MemoEntry e{};
  e.id = id;
  e.kind = MemoKind::Voice;
  e.sec = sec;
  e.size = size;
  push_entry(ctx, e);
  evict_voice(ctx);
}

bool handle(const Action& a, FeatureContext& ctx) {
  switch (a.type) {
    case ActionType::MemoCreate:
      return memo_create(a.text, std::strlen(a.text), ctx) >= 0;
    case ActionType::MemoDelete:
      return memo_delete(a.arg0, ctx);
    case ActionType::MemoRecordStart:
      // トグル: 録音中なら停止して確定、止まっていれば開始。
      return memo_record_toggle(ctx) >= 0;
    case ActionType::MemoRecordStop:
      if (s_recording) finish_record(ctx, true);
      return true;
    case ActionType::MemoPlay:
      return memo_play(a.arg0, ctx);
    case ActionType::MemoStopPlay:
      memo_stop_play(ctx);
      return true;
    default:
      return false;
  }
}

void tick(int64_t, FeatureContext& ctx) {
  // 上限/外部停止に追従して録音を確定する。
  if (s_recording && ctx.audio) {
    if (!ctx.audio->recording() ||
        ctx.audio->record_elapsed_s() >= kMemoVoiceMaxSec) {
      finish_record(ctx, true);
    }
  }
  // 再生が自然終了したら状態をクリアする。
  if (s_playing && ctx.audio && !ctx.audio->playing()) {
    s_playing = false;
    s_play_id = 0;
    s_play_lease.release();
  }
}

void save(FeatureContext& ctx) {
  // create/確定 ごとに永続化しているので、ここは整合用の全書き直し。
  persist_header(ctx.storage);
  for (uint16_t i = 0; i < g_count; ++i) {
    persist_slot(ctx.storage,
                 static_cast<uint16_t>((g_head + i) % kMemoMaxEntries));
  }
}

void restore(FeatureContext& ctx) {
  // スタック変数のアドレスを KV 読み出しに渡すと GCC13 の
  // -Wdangling-pointer を踏むので、初期化時のみの復元先は静的にする。
  static Header h;
  h = Header{};
  size_t n = 0;
  if (!ctx.storage.get(kHdrKey, &h, sizeof(h), &n) || n < sizeof(h) ||
      h.head >= kMemoMaxEntries || h.count > kMemoMaxEntries) {
    return;
  }
  if (h.version != 1 && h.version != kPersistVersion) return;
  const bool v2 = h.version == kPersistVersion;
  g_head = h.head;
  g_count = h.count;
  g_next_id = h.next_id ? h.next_id : 1;
  for (uint16_t i = 0; i < g_count; ++i) {
    const uint16_t slot =
        static_cast<uint16_t>((g_head + i) % kMemoMaxEntries);
    char key[32];
    slot_key(slot, key, sizeof(key));
    static SlotBlob b;
    static SlotBlobV1 b1;
    b = SlotBlob{};
    b1 = SlotBlobV1{};
    size_t bn = 0;
    bool ok;
    if (v2) {
      ok = ctx.storage.get(key, &b, sizeof(b), &bn) && bn >= 16 &&
           b.len <= kMemoMaxText;
    } else {
      ok = ctx.storage.get(key, &b1, sizeof(b1), &bn) && bn >= 6 &&
           b1.len <= kMemoMaxText;
      if (ok) {
        b.id = b1.id;
        b.kind = static_cast<uint8_t>(MemoKind::Text);
        b.len = b1.len;
        std::memcpy(b.text, b1.text, b1.len);
      }
    }
    if (ok) {
      MemoEntry& e = g_entries[slot];
      e.id = b.id;
      e.kind = static_cast<MemoKind>(b.kind);
      e.len = b.len;
      e.sec = b.sec;
      e.size = b.size;
      std::memcpy(e.text, b.text, b.len);
      e.text[b.len] = '\0';
    } else {
      // 壊れていたらそのスロットは空扱い。
      g_entries[slot] = MemoEntry{};
    }
  }
}

}  // namespace

// 1件追加して id を返す。-1 = 失敗 (テキストが長すぎ)。
int32_t memo_create(const char* text, size_t len, FeatureContext& ctx) {
  if (!text || len == 0 || len > kMemoMaxText) return -1;
  MemoEntry e{};
  e.id = g_next_id++;
  e.kind = MemoKind::Text;
  e.len = static_cast<uint16_t>(len);
  std::memcpy(e.text, text, len);
  push_entry(ctx, e);
  return static_cast<int32_t>(e.id);
}

// id のエントリを消す。リング順を保ったまま後続を前に詰め、
// 末尾スロットの KV を消す。無ければ false。
bool memo_delete(uint32_t id, FeatureContext& ctx) {
  for (size_t i = 0; i < g_count; ++i) {
    const uint16_t slot =
        static_cast<uint16_t>((g_head + i) % kMemoMaxEntries);
    if (g_entries[slot].id != id) continue;
    if (s_playing && s_play_id == id) memo_stop_play(ctx);
    if (g_entries[slot].kind == MemoKind::Voice && ctx.audio) {
      ctx.audio->memo_audio_erase(id);
    }
    for (size_t j = i; j + 1 < g_count; ++j) {
      const uint16_t dst =
          static_cast<uint16_t>((g_head + j) % kMemoMaxEntries);
      const uint16_t src =
          static_cast<uint16_t>((g_head + j + 1) % kMemoMaxEntries);
      g_entries[dst] = g_entries[src];
      persist_slot(ctx.storage, dst);
    }
    --g_count;
    const uint16_t last =
        static_cast<uint16_t>((g_head + g_count) % kMemoMaxEntries);
    g_entries[last] = MemoEntry{};
    char key[32];
    slot_key(last, key, sizeof(key));
    ctx.storage.erase(key);
    persist_header(ctx.storage);
    ctx.bus.publish({EventType::MemoDeleted, id});
    return true;
  }
  return false;
}

int32_t memo_record_toggle(FeatureContext& ctx) {
  if (s_recording) {
    const uint32_t id = s_rec_id;
    finish_record(ctx, true);
    return static_cast<int32_t>(id);
  }
  if (s_playing) memo_stop_play(ctx);
  if (!ctx.audio) return -1;
  const uint32_t id = g_next_id++;
  if (!ctx.audio->record_begin(id, kMemoVoiceMaxSec)) return -1;
  s_recording = true;
  s_rec_id = id;
  if (ctx.power) s_rec_lease = ctx.power->acquire(Res::Audio, "memo");
  return static_cast<int32_t>(id);
}

bool memo_recording() { return s_recording; }
uint32_t memo_record_id() { return s_recording ? s_rec_id : 0; }
uint32_t memo_record_elapsed_s(FeatureContext& ctx) {
  return (s_recording && ctx.audio) ? ctx.audio->record_elapsed_s() : 0;
}
uint8_t memo_record_level(FeatureContext& ctx) {
  return (s_recording && ctx.audio) ? ctx.audio->record_level() : 0;
}

uint32_t memo_playing_id() { return s_playing ? s_play_id : 0; }

bool memo_play(uint32_t id, FeatureContext& ctx) {
  MemoEntry e;
  if (!memo_find(id, &e) || e.kind != MemoKind::Voice || !ctx.audio ||
      s_recording) {
    return false;
  }
  if (s_playing) memo_stop_play(ctx);
  uint8_t vol = 70;
  if (ctx.settings) {
    vol = static_cast<uint8_t>(
        ctx.settings->audio_volume > 100 ? 100 : ctx.settings->audio_volume);
  }
  if (!ctx.audio->play_begin(id, vol)) return false;
  s_playing = true;
  s_play_id = id;
  if (ctx.power) s_play_lease = ctx.power->acquire(Res::Audio, "memo.play");
  return true;
}

void memo_stop_play(FeatureContext& ctx) {
  if (!s_playing) return;
  if (ctx.audio) ctx.audio->play_stop();
  s_playing = false;
  s_play_id = 0;
  s_play_lease.release();
}

size_t memo_count() { return g_count; }

void memo_reset_state() {
  for (size_t i = 0; i < kMemoMaxEntries; ++i) g_entries[i] = MemoEntry{};
  g_count = 0;
  g_head = 0;
  g_next_id = 1;
  s_recording = false;
  s_rec_id = 0;
  s_rec_lease.release();
  s_playing = false;
  s_play_id = 0;
  s_play_lease.release();
}

bool memo_at(size_t i, MemoEntry* out) {
  if (!out || i >= g_count) return false;
  const uint16_t slot =
      static_cast<uint16_t>((g_head + i) % kMemoMaxEntries);
  *out = g_entries[slot];
  return true;
}

bool memo_find(uint32_t id, MemoEntry* out) {
  for (uint16_t i = 0; i < g_count; ++i) {
    const uint16_t slot =
        static_cast<uint16_t>((g_head + i) % kMemoMaxEntries);
    if (g_entries[slot].id == id) {
      if (out) *out = g_entries[slot];
      return true;
    }
  }
  return false;
}

const FeatureDescriptor kMemo = {
    /*id*/ "memo",
    /*capabilities*/ HasScreen | HasQuickTile | NeedsAudio,
    /*route*/ Route::Memo,
    /*init*/ nullptr,
    /*handle*/ handle,
    /*tick*/ tick,
    /*next_deadline_ms*/ nullptr,
    /*save*/ save,
    /*restore*/ restore,
};

}  // namespace features
}  // namespace watch
