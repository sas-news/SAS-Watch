// make_sample.mjs — index.html の PURE ブロックを node で実行して
// core/tests/data/theme_studio_sample.zip を生成する。
//   node tools/theme-studio/make_sample.mjs
// ホストテスト (test_theme.cpp) がこの zip を core のパーサで読む。

import { readFileSync, writeFileSync, mkdirSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const repo = join(here, "..", "..");

const html = readFileSync(join(here, "index.html"), "utf8");
const begin = html.indexOf("// ==STUDIO-PURE-BEGIN==");
const end = html.indexOf("// ==STUDIO-PURE-END==");
if (begin < 0 || end < 0) throw new Error("PURE ブロックが見つからない");
// DOM 参照のない範囲だけ評価 (Studio を globalThis に出す)。
new Function(html.slice(begin, end))();
const Studio = globalThis.Studio;

const st = Studio.defaults();
st.id = "studio-test";
st.name = "スタジオ検証";
st.tokens.primary = "0x11AA22";
st.tokens.accent = "0x334455";

// 小さいダミー PNG (1x1 透過。parse には内容は不要)。
const png = new Uint8Array([
  0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
  0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
  0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4, 0x89, 0x00, 0x00, 0x00,
  0x0d, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x62, 0x00, 0x01, 0x00, 0x00,
  0x05, 0x00, 0x01, 0x0d, 0x0a, 0x2d, 0xb4, 0x00, 0x00, 0x00, 0x00, 0x49,
  0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
]);
st.images.home_bg = { name: "home_bg_sample.png", data: png };
st.screens.timer = { name: "timer_bg_sample.png", data: png };
st.icons.timer = { name: "ic_timer_sample.png", data: png };

const files = [{ name: "manifest.cbor", data: Studio.encodeManifest(st) }];
for (const k in st.images) files.push({ name: st.images[k].name, data: st.images[k].data });
for (const k in st.screens) files.push({ name: st.screens[k].name, data: st.screens[k].data });
for (const k in st.icons) files.push({ name: st.icons[k].name, data: st.icons[k].data });

const zip = Studio.buildZip(files);

const outDir = join(repo, "core", "tests", "data");
mkdirSync(outDir, { recursive: true });
const out = join(outDir, "theme_studio_sample.zip");
writeFileSync(out, zip);
console.log("wrote", out, zip.length, "B,", files.length, "entries");

// セルフチェック: parseZip で往復できるか。
const back = Studio.parseZip(zip);
if (!back || back.length !== files.length) throw new Error("parseZip 失敗");
console.log("parseZip ok:", back.map(f => f.name).join(", "));
