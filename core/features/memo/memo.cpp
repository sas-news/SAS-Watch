// Memo Feature。最大64件×最大256バイト(UTF-8)のリング。
// 溢れたら最古を消す。各スロットを別 KV キーに書く (NVS blob が小さい前提)。
#include "watch/features/memo.hpp"

#include <cstdio>
#include <cstring>

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
constexpr uint8_t kPersistVersion = 1;
constexpr const char* kHdrKey = "feat.memo.hdr";
constexpr const char* kSlotKeyFmt = "feat.memo.%02u";  // 00..63

struct SlotBlob {
  uint32_t id;
  uint16_t len;
  char text[kMemoMaxText];  // len バイトまで有効。NUL 終端を保つ
};

MemoEntry g_entries[kMemoMaxEntries];
uint16_t g_count = 0;  // 有効件数
uint16_t g_head = 0;   // 最古スロット
uint32_t g_next_id = 1;

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
  SlotBlob b{};
  b.id = g_entries[slot].id;
  b.len = g_entries[slot].len;
  std::memcpy(b.text, g_entries[slot].text, b.len);
  kv.set(key, &b, sizeof(b));
}

bool handle(const Action& a, FeatureContext& ctx) {
  switch (a.type) {
    case ActionType::MemoCreate:
      return memo_create(a.text, std::strlen(a.text), ctx) >= 0;
    case ActionType::MemoRecordStart:
      // 録音は firmware 側 (Audio Lease + codec)。core は握らない。
      // TODO(hw): 実機で確認 - 録音パイプラインは services/audio に。
      return true;
    case ActionType::MemoRecordStop:
      return true;
    default:
      return false;
  }
}

void save(FeatureContext& ctx) {
  // create ごとに永続化しているので、ここは整合用の全書き直し。
  persist_header(ctx.storage);
  for (uint16_t i = 0; i < g_count; ++i) {
    persist_slot(ctx.storage,
                 static_cast<uint16_t>((g_head + i) % kMemoMaxEntries));
  }
}

void restore(FeatureContext& ctx) {
  Header h{};
  size_t n = 0;
  if (!ctx.storage.get(kHdrKey, &h, sizeof(h), &n) || n < sizeof(h) ||
      h.version != kPersistVersion || h.head >= kMemoMaxEntries ||
      h.count > kMemoMaxEntries) {
    return;
  }
  g_head = h.head;
  g_count = h.count;
  g_next_id = h.next_id ? h.next_id : 1;
  for (uint16_t i = 0; i < g_count; ++i) {
    const uint16_t slot =
        static_cast<uint16_t>((g_head + i) % kMemoMaxEntries);
    char key[32];
    slot_key(slot, key, sizeof(key));
    SlotBlob b{};
    size_t bn = 0;
    if (ctx.storage.get(key, &b, sizeof(b), &bn) && bn >= 6 &&
        b.len <= kMemoMaxText) {
      g_entries[slot].id = b.id;
      g_entries[slot].len = b.len;
      std::memcpy(g_entries[slot].text, b.text, b.len);
      g_entries[slot].text[b.len] = '\0';
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
  uint16_t slot;
  if (g_count < kMemoMaxEntries) {
    slot = static_cast<uint16_t>((g_head + g_count) % kMemoMaxEntries);
    ++g_count;
  } else {
    // 満杯 → 最古 (head) を消して回す。
    slot = g_head;
    g_head = static_cast<uint16_t>((g_head + 1) % kMemoMaxEntries);
  }
  MemoEntry& e = g_entries[slot];
  e.id = g_next_id++;
  e.len = static_cast<uint16_t>(len);
  std::memcpy(e.text, text, len);
  e.text[len] = '\0';

  persist_slot(ctx.storage, slot);
  persist_header(ctx.storage);
  ctx.bus.publish({EventType::MemoSaved, e.id});
  return static_cast<int32_t>(e.id);
}

size_t memo_count() { return g_count; }

void memo_reset_state() {
  for (size_t i = 0; i < kMemoMaxEntries; ++i) g_entries[i] = MemoEntry{};
  g_count = 0;
  g_head = 0;
  g_next_id = 1;
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
    /*tick*/ nullptr,
    /*next_deadline_ms*/ nullptr,
    /*save*/ save,
    /*restore*/ restore,
};

}  // namespace features
}  // namespace watch
