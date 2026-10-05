// テーマ manifest / パッケージ (theme/*.hpp) と SetTheme Action のテスト。
#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "watch/event.hpp"
#include "watch/event_bus.hpp"
#include "watch/feature.hpp"
#include "watch/input_mapper.hpp"
#include "watch/navigation.hpp"
#include "watch/power.hpp"
#include "watch/protocol/cbor.hpp"
#include "watch/runtime.hpp"
#include "watch/settings.hpp"
#include "fakes.hpp"
#include "watch/theme/manifest.hpp"
#include "watch/theme/package.hpp"

using namespace watch;

namespace {

// ---- テスト内ヘルパ: 最小 manifest {id, api:1} ----

std::vector<uint8_t> build_manifest(const char* id) {
  uint8_t buf[128];
  cbor::Writer w(buf, sizeof(buf));
  w.map(2).text("id").text(id).text("api").uint_v(1);
  EXPECT_TRUE(w.ok());
  return {buf, buf + w.size()};
}

// ---- テスト内ヘルパ: stored zip を手で組み立てる ----

struct ZipBuilder {
  std::vector<uint8_t> bytes;
  std::vector<uint8_t> central;

  void push32(std::vector<uint8_t>* v, uint32_t x) {
    v->push_back(x & 0xFF);
    v->push_back((x >> 8) & 0xFF);
    v->push_back((x >> 16) & 0xFF);
    v->push_back((x >> 24) & 0xFF);
  }
  void push16(std::vector<uint8_t>* v, uint16_t x) {
    v->push_back(x & 0xFF);
    v->push_back((x >> 8) & 0xFF);
  }

  void add(const char* name, const uint8_t* data, size_t n) {
    const uint32_t lho = static_cast<uint32_t>(bytes.size());
    const uint32_t crc = theme_crc32(data, n);
    const size_t nl = std::strlen(name);
    // local header
    push32(&bytes, 0x04034b50u);
    push16(&bytes, 20);   // ver needed
    push16(&bytes, 0);    // flags
    push16(&bytes, 0);    // method = stored
    push16(&bytes, 0);
    push16(&bytes, 0);
    push32(&bytes, crc);
    push32(&bytes, static_cast<uint32_t>(n));
    push32(&bytes, static_cast<uint32_t>(n));
    push16(&bytes, static_cast<uint16_t>(nl));
    push16(&bytes, 0);
    bytes.insert(bytes.end(), name, name + nl);
    bytes.insert(bytes.end(), data, data + n);
    // central entry
    push32(&central, 0x02014b50u);
    push16(&central, 20);
    push16(&central, 20);
    push16(&central, 0);
    push16(&central, 0);
    push16(&central, 0);
    push16(&central, 0);
    push32(&central, crc);
    push32(&central, static_cast<uint32_t>(n));
    push32(&central, static_cast<uint32_t>(n));
    push16(&central, static_cast<uint16_t>(nl));
    push16(&central, 0);
    push16(&central, 0);
    push16(&central, 0);
    push16(&central, 0);
    push32(&central, 0);
    push32(&central, lho);
    central.insert(central.end(), name, name + nl);
  }

  // finish(): central dir + EOCD を付けて完成 zip を返す。
  std::vector<uint8_t> finish(int entry_count) {
    const uint32_t cd_off = static_cast<uint32_t>(bytes.size());
    const uint32_t cd_size = static_cast<uint32_t>(central.size());
    bytes.insert(bytes.end(), central.begin(), central.end());
    push32(&bytes, 0x06054b50u);
    push16(&bytes, 0);
    push16(&bytes, 0);
    push16(&bytes, static_cast<uint16_t>(entry_count));
    push16(&bytes, static_cast<uint16_t>(entry_count));
    push32(&bytes, cd_size);
    push32(&bytes, cd_off);
    push16(&bytes, 0);
    return bytes;
  }
};

std::vector<uint8_t> make_rgb565a8_bin(uint16_t w, uint16_t h) {
  std::vector<uint8_t> b(12 + w * h * 3);
  b[0] = 0x19;
  b[1] = 0x14;
  b[4] = w & 0xFF;
  b[5] = w >> 8;
  b[6] = h & 0xFF;
  b[7] = h >> 8;
  b[8] = (w * 2) & 0xFF;
  b[9] = (w * 2) >> 8;
  return b;
}

}  // namespace

// ---------- theme_id_ok ----------

TEST(Theme, IdOk) {
  EXPECT_TRUE(theme_id_ok("standard"));
  EXPECT_TRUE(theme_id_ok("mame-cat2"));
  EXPECT_TRUE(theme_id_ok("a"));
  EXPECT_FALSE(theme_id_ok(""));
  EXPECT_FALSE(theme_id_ok(nullptr));
  EXPECT_FALSE(theme_id_ok("Mame"));        // 大文字不可
  EXPECT_FALSE(theme_id_ok("my_theme"));    // _ 不可
  EXPECT_FALSE(theme_id_ok("../evil"));
  EXPECT_FALSE(theme_id_ok("01234567890123456789012345678901"));  // 32文字
}

// ---------- manifest ----------

TEST(Theme, ManifestMinimal) {
  auto m = build_manifest("mame");
  ThemeManifest out;
  EXPECT_EQ(theme_manifest_parse(m.data(), m.size(), &out),
            ThemeManifestError::kOk);
  EXPECT_STREQ(out.id, "mame");
  EXPECT_STREQ(out.name, "mame");  // name 省略 → id
  EXPECT_EQ(out.api, 1u);
  EXPECT_EQ(out.version, 1u);
  EXPECT_EQ(out.color_set, 0u);
  EXPECT_EQ(out.image_set, 0u);
}

TEST(Theme, ManifestFull) {
  uint8_t tok[256];
  cbor::Writer tw(tok, sizeof(tok));
  tw.map(4)
      .text("bg").text("0x101010")
      .text("primary").text("0xFF8800")
      .text("radius_sm").uint_v(12)
      .text("font_digits").uint_v(56);
  ASSERT_TRUE(tw.ok());
  uint8_t img[128];
  cbor::Writer iw(img, sizeof(img));
  iw.map(2).text("home_bg").text("bg.bin").text("stand").text("cat.bin");
  ASSERT_TRUE(iw.ok());

  uint8_t buf[1024];
  cbor::Writer w(buf, sizeof(buf));
  w.map(5)
      .text("id").text("mame-cat")
      .text("api").uint_v(1)
      .text("name").text("まめキャット")
      .text("tokens").raw(tok, tw.size())
      .text("images").raw(img, iw.size());
  ASSERT_TRUE(w.ok());

  ThemeManifest out;
  EXPECT_EQ(theme_manifest_parse(buf, w.size(), &out),
            ThemeManifestError::kOk);
  EXPECT_STREQ(out.id, "mame-cat");
  EXPECT_STREQ(out.name, "まめキャット");
  EXPECT_TRUE(out.color_set & 1);          // bg
  EXPECT_EQ(out.color[0], 0x101010u);
  EXPECT_TRUE(out.color_set & (1 << 3));   // primary
  EXPECT_EQ(out.color[3], 0xFF8800u);
  EXPECT_TRUE(out.metric_set & 1);
  EXPECT_EQ(out.radius_sm, 12);
  EXPECT_TRUE(out.font_set & (1 << 2));
  EXPECT_EQ(out.font_px[2], 56);
  EXPECT_TRUE(out.image_set & 1);
  EXPECT_STREQ(out.image[0], "bg.bin");
  EXPECT_TRUE(out.image_set & 2);
  EXPECT_STREQ(out.image[1], "cat.bin");
  EXPECT_FALSE(out.image_set & 4);
}

TEST(Theme, ManifestErrors) {
  ThemeManifest out;
  // id 欠落
  {
    uint8_t buf[64];
    cbor::Writer w(buf, sizeof(buf));
    w.map(1).text("api").uint_v(1);
    EXPECT_EQ(theme_manifest_parse(buf, w.size(), &out),
              ThemeManifestError::kMissingId);
  }
  // api 欠落/違い
  {
    uint8_t buf[64];
    cbor::Writer w(buf, sizeof(buf));
    w.map(2).text("id").text("mame").text("api").uint_v(2);
    EXPECT_EQ(theme_manifest_parse(buf, w.size(), &out),
              ThemeManifestError::kBadApi);
  }
  // id 文字種違反
  {
    auto m = build_manifest("Bad_Id");
    EXPECT_EQ(theme_manifest_parse(m.data(), m.size(), &out),
              ThemeManifestError::kBadId);
  }
  // 色の形式違反
  {
    uint8_t buf[256];
    cbor::Writer w(buf, sizeof(buf));
    w.map(3)
        .text("id").text("mame")
        .text("api").uint_v(1)
        .text("tokens").map(1).text("bg").text("blue");
    EXPECT_EQ(theme_manifest_parse(buf, w.size(), &out),
              ThemeManifestError::kBadValue);
  }
  // 画像名が危険
  {
    uint8_t buf[256];
    cbor::Writer w(buf, sizeof(buf));
    w.map(3)
        .text("id").text("mame")
        .text("api").uint_v(1)
        .text("images").map(1).text("home_bg").text("../etc.bin");
    EXPECT_EQ(theme_manifest_parse(buf, w.size(), &out),
              ThemeManifestError::kBadImageName);
  }
  // CBOR じゃない
  {
    const uint8_t junk[] = {0xFF, 0xFF, 0xFF};
    EXPECT_EQ(theme_manifest_parse(junk, sizeof(junk), &out),
              ThemeManifestError::kBadCbor);
  }
}

// ---------- package (stored zip) ----------

TEST(Theme, PackageListAndData) {
  ZipBuilder z;
  const uint8_t manifest[] = {0xA2, 0x62, 'i', 'd', 0x64, 'm', 'a', 'm', 'e',
                              0x63, 'a', 'p', 'i', 0x01};
  auto img = make_rgb565a8_bin(4, 4);
  z.add("manifest.cbor", manifest, sizeof(manifest));
  z.add("bg.bin", img.data(), img.size());
  auto zip = z.finish(2);

  ThemePackageEntry list[16];
  const int cnt =
      theme_package_list(zip.data(), zip.size(), list, 16);
  ASSERT_EQ(cnt, 2);
  EXPECT_STREQ(list[0].name, "manifest.cbor");
  EXPECT_STREQ(list[1].name, "bg.bin");

  const ThemePackageEntry* e =
      theme_package_find(list, cnt, "manifest.cbor");
  ASSERT_NE(e, nullptr);
  const uint8_t* d = theme_package_data(zip.data(), zip.size(), e);
  ASSERT_NE(d, nullptr);
  EXPECT_EQ(std::memcmp(d, manifest, sizeof(manifest)), 0);
  EXPECT_EQ(theme_crc32(d, e->size), e->crc32);

  EXPECT_EQ(theme_package_find(list, cnt, "nope.bin"), nullptr);
}

TEST(Theme, PackageRejectsBad) {
  ThemePackageEntry list[16];
  // 空/小さすぎ
  EXPECT_EQ(theme_package_list(nullptr, 0, list, 16), -1);
  // EOCD 無し
  {
    const uint8_t junk[64] = {};
    EXPECT_EQ(theme_package_list(junk, sizeof(junk), list, 16), -1);
  }
  // 危険な名前
  {
    ZipBuilder z;
    const uint8_t d[4] = {1, 2, 3, 4};
    z.add("../evil.bin", d, sizeof(d));
    auto zip = z.finish(1);
    EXPECT_EQ(theme_package_list(zip.data(), zip.size(), list, 16), -1);
  }
}

// ---------- .bin 画像ヘッダ ----------

TEST(Theme, ImageCheck) {
  auto ok = make_rgb565a8_bin(160, 200);
  EXPECT_TRUE(theme_image_check(ok.data(), ok.size(), 240, 360));
  EXPECT_FALSE(theme_image_check(ok.data(), ok.size(), 150, 360));  // w 超過
  // サイズ不足
  EXPECT_FALSE(theme_image_check(ok.data(), ok.size() - 2, 240, 360));
  // magic 違い
  ok[0] = 0x18;
  EXPECT_FALSE(theme_image_check(ok.data(), ok.size(), 240, 360));
  ok[0] = 0x19;
  // cf 違い (RGB888)
  ok[1] = 0x0F;
  EXPECT_FALSE(theme_image_check(ok.data(), ok.size(), 240, 360));

  uint16_t w, h;
  theme_image_size(make_rgb565a8_bin(160, 200).data(), &w, &h);
  EXPECT_EQ(w, 160);
  EXPECT_EQ(h, 200);
}

// ---------- SetTheme Action ----------

namespace {
struct ThemeRt {
  EventBus bus;
  Navigator nav{&bus};
  PowerPolicy power{&bus};
  InputMapper input;
  FeatureRegistry registry{builtin_features(), builtin_features_count()};
  Settings settings;
  test::FakeClock clock;
  test::MemoryKeyValueStore kv;
  FeatureContext ctx{bus, kv, clock, &nav, &power};
  Runtime runtime{bus, nav, power, input, registry, settings};
  int theme_changed = 0;

  ThemeRt() {
    power.kick_activity(0);
    bus.subscribe(
        EventType::ThemeChanged,
        [](const Event&, void* c) {
          ++*static_cast<int*>(c);
        },
        &theme_changed);
  }
};
}  // namespace

TEST(Theme, SetThemeActionStoresAndNotifies) {
  ThemeRt f;
  Action a{};
  a.type = ActionType::SetTheme;
  a.source = ActionSource::System;
  a.set_text("mame");
  f.runtime.queue().push(a);
  EXPECT_TRUE(f.runtime.step(10, f.ctx));
  EXPECT_STREQ(f.settings.theme, "mame");
  EXPECT_EQ(f.theme_changed, 1);

  // 設定キーとしても読める (settings.get と同じ値)。
  const SettingKey* k = settings_find("theme");
  ASSERT_NE(k, nullptr);
  EXPECT_STREQ(settings_get_str(f.settings, *k), "mame");
}

TEST(Theme, SetThemeActionRejectsBadId) {
  ThemeRt f;
  Action a{};
  a.type = ActionType::SetTheme;
  a.source = ActionSource::System;
  a.set_text("Bad Theme");
  f.runtime.queue().push(a);
  // step は「キューを消費した」だけ返す。拒否は settings/イベント不変で見る。
  EXPECT_TRUE(f.runtime.step(10, f.ctx));
  EXPECT_STREQ(f.settings.theme, "standard");
  EXPECT_EQ(f.theme_changed, 0);
}
