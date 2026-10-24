#!/usr/bin/env python3
"""theme_build.py — theme.json から manifest.cbor + STORED zip を作る。

    python3 tools/theme_build.py themes/<id>
    python3 tools/theme_build.py themes/<id>/theme.json --preview

theme.json は docs/theme-guide.md の形式。画像は参照先の PNG / .bin を
そのまま zip に入れる (端末側が PNG をデコードする)。
"""
import argparse
import json
import os
import re
import struct
import subprocess
import sys
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
PKG_MAX = 4 * 1024 * 1024
ENTRY_MAX = 32
ENTRY_NAME_RE = re.compile(r"^[a-z0-9._-]{1,32}$")
THEME_ID_RE = re.compile(r"^[a-z0-9-]{1,31}$")
ASSET_ID_RE = re.compile(r"^[a-z0-9_.-]{1,31}$")
COLOR_RE = re.compile(r"^(0x|#)?([0-9a-fA-F]{6})$")
SCREENS = ["home", "quick", "more", "timer", "stopwatch", "counter", "steps",
           "memo", "settings", "ota", "powermenu", "alarm", "notifications",
           "media", "agent", "alert"]
SKIN_PARTS = ["card", "list_group", "row", "divider", "button_primary",
              "button_secondary", "button_danger", "back_pill", "header_bar",
              "switch_track", "switch_knob", "slider_track", "slider_fill",
              "slider_knob", "icon_tile", "toast", "bubble", "caption_line"]
SKIN_STATES = ["pressed", "checked", "disabled"]
FONT_SIZES = {20, 26, 56, 96}
FONT_SLOTS = ["body", "title", "digits", "digits_sm"]
FACE_ELEMS = ["time", "date", "steps", "battery", "notify", "bubble", "chara"]
IMAGE_SLOTS = {"home_bg": (410, 502), "stand": (240, 360),
               "timer_done": (410, 320), "face_chara": (240, 410)}
STYLE_KEYS = ["card_radius", "card_opa", "border_w", "border", "glow_color",
              "glow_w", "btn_radius", "header"]
BUBBLE_KEYS = ["morning", "noon", "evening", "night", "steps"]
COLOR_KEYS = ["bg", "surface", "surface2", "line", "primary", "primary2",
              "on_primary", "text", "text_dim", "accent", "danger", "ok",
              "accent2", "accent3", "accent4", "accent5", "bubble_bg",
              "bubble_text", "edge"]
METRIC_KEYS = ["radius_sm", "radius_lg", "space", "anim_ms"]
FONT_KEYS = ["font_body", "font_title", "font_digits", "font_digits_sm"]
TOP_KEYS = ["id", "name", "version", "tokens", "images", "bubble", "screens",
            "style", "icons", "fonts", "mascot", "face_layout", "skin",
            "assets", "palette"]


class ThemeError(Exception):
    pass


def _head(major, n):
    if n < 24:
        return bytes([(major << 5) | n])
    if n < 256:
        return bytes([(major << 5) | 24, n])
    if n < 65536:
        return bytes([(major << 5) | 25]) + struct.pack(">H", n)
    return bytes([(major << 5) | 26]) + struct.pack(">I", n)


def c_uint(n):
    return _head(0, n)


def c_int(n):
    return c_uint(n) if n >= 0 else _head(1, -1 - n)


def c_text(s):
    b = s.encode("utf-8")
    return _head(3, len(b)) + b


def c_map(d):
    out = _head(5, len(d))
    for k, v in d.items():
        out += c_text(k) + v
    return out


def c_arr(items):
    out = _head(4, len(items))
    for it in items:
        out += it
    return out


def fail(errors, msg):
    errors.append(msg)


def color(v, where, errors, palette):
    if not isinstance(v, str):
        fail(errors, f"{where}: 色は文字列 (0xRRGGBB) で書く")
        return None
    if v.startswith("@"):
        if v[1:] not in palette:
            fail(errors, f"{where}: palette に '{v[1:]}' がない")
        return None
    m = COLOR_RE.match(v)
    if not m:
        fail(errors, f"{where}: 色 '{v}' が不正 (0xRRGGBB)")
        return None
    return int(m.group(2), 16)


def uint(v, where, errors, lo=0, hi=65535):
    if not isinstance(v, int) or isinstance(v, bool) or not lo <= v <= hi:
        fail(errors, f"{where}: {lo}..{hi} の整数で書く (got {v!r})")
        return None
    return v


def png_size(path):
    data = path.read_bytes()[:33]
    if len(data) < 24 or data[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    return struct.unpack(">II", data[16:24])


def bin_size(path):
    data = path.read_bytes()[:12]
    if len(data) < 12 or data[0] != 0x19:
        return None
    w, h = struct.unpack("<HH", data[4:8])
    return w, h


def image_size(path):
    return png_size(path) or bin_size(path)


def resolve_file(theme_dir, name, where, errors, files):
    if not isinstance(name, str) or not name:
        fail(errors, f"{where}: ファイル名が不正")
        return None
    if "/" in name or "\\" in name or name.startswith("."):
        fail(errors, f"{where}: ファイル名にディレクトリや先頭ドットは使えない")
        return None
    if not ENTRY_NAME_RE.match(name):
        fail(errors, f"{where}: '{name}' は [a-z0-9._-] 32文字まで")
        return None
    p = theme_dir / name
    if not p.is_file():
        fail(errors, f"{where}: ファイル '{name}' がない")
        return None
    files[name] = p
    return p


def check_image(theme_dir, name, where, errors, files, max_wh=None):
    p = resolve_file(theme_dir, name, where, errors, files)
    if not p:
        return
    size = image_size(p)
    if size is None:
        fail(errors, f"{where}: '{name}' は PNG か LVGL .bin ではない")
        return
    w, h = size
    if max_wh and (w > max_wh[0] or h > max_wh[1]):
        fail(errors, f"{where}: '{name}' は {w}x{h} (上限 "
                     f"{max_wh[0]}x{max_wh[1]})")


def validate(m, theme_dir):
    errors = []
    if not isinstance(m, dict):
        raise ThemeError("theme.json のトップレベルは object")
    for k in m:
        if k not in TOP_KEYS:
            fail(errors, f"未知のキー '{k}' (typo? 使えるキー: {', '.join(TOP_KEYS)})")
    tid = m.get("id")
    if not isinstance(tid, str) or not THEME_ID_RE.match(tid):
        fail(errors, "id: [a-z0-9-]{1,31} で書く")
    if "name" in m and (not isinstance(m["name"], str) or not m["name"]):
        fail(errors, "name: 空でない文字列")
    if "version" in m:
        uint(m["version"], "version", errors, 1, 9999)
    palette = {}
    if "palette" in m:
        pal = m["palette"]
        if not isinstance(pal, dict) or len(pal) > 8:
            fail(errors, "palette: 最大8件の object")
        else:
            for name, v in pal.items():
                if not re.match(r"^[a-z0-9_-]{1,15}$", name):
                    fail(errors, f"palette.{name}: [a-z0-9_-] 15文字まで")
                c = color(v, f"palette.{name}", errors, {})
                if c is not None:
                    palette[name] = c
    files = {}
    tokens = m.get("tokens", {})
    if not isinstance(tokens, dict):
        fail(errors, "tokens: object")
        tokens = {}
    for k, v in tokens.items():
        if k in COLOR_KEYS:
            color(v, f"tokens.{k}", errors, palette)
        elif k in METRIC_KEYS:
            hi = 2000 if k == "anim_ms" else (255 if k.startswith("radius") else 64)
            uint(v, f"tokens.{k}", errors, 0, hi)
        elif k in FONT_KEYS:
            if v not in FONT_SIZES:
                fail(errors, f"tokens.{k}: 20/26/56/96 のどれか (got {v!r})")
        else:
            fail(errors, f"tokens.{k}: 未知のトークン")
    images = m.get("images", {})
    if not isinstance(images, dict):
        fail(errors, "images: object")
        images = {}
    for slot, name in images.items():
        if slot not in IMAGE_SLOTS:
            fail(errors, f"images.{slot}: 未知のスロット")
            continue
        check_image(theme_dir, name, f"images.{slot}", errors, files,
                    IMAGE_SLOTS[slot])
    if "bubble" in m:
        b = m["bubble"]
        if not isinstance(b, dict):
            fail(errors, "bubble: object")
        else:
            for k, v in b.items():
                if k not in BUBBLE_KEYS:
                    fail(errors, f"bubble.{k}: 未知のキー")
                elif not isinstance(v, str) or len(v.encode()) > 48:
                    fail(errors, f"bubble.{k}: 48バイトまでの文字列")
    if "screens" in m:
        sc = m["screens"]
        if not isinstance(sc, dict):
            fail(errors, "screens: object")
        else:
            for name, spec in sc.items():
                if name != "*" and name not in SCREENS:
                    fail(errors, f"screens.{name}: 未知の画面名")
                if not isinstance(spec, dict):
                    fail(errors, f"screens.{name}: object")
                    continue
                for k in spec:
                    if k not in ("bg", "scrim", "tokens", "deco"):
                        fail(errors, f"screens.{name}.{k}: 未知のキー")
                if "bg" in spec:
                    check_image(theme_dir, spec["bg"], f"screens.{name}.bg",
                                errors, files, (410, 502))
                if "scrim" in spec:
                    uint(spec["scrim"], f"screens.{name}.scrim", errors, 0, 255)
                if "tokens" in spec:
                    if not isinstance(spec["tokens"], dict):
                        fail(errors, f"screens.{name}.tokens: object")
                    else:
                        for k, v in spec["tokens"].items():
                            if k not in COLOR_KEYS and k not in METRIC_KEYS:
                                fail(errors, f"screens.{name}.tokens.{k}: 未知")
                            elif k in COLOR_KEYS:
                                color(v, f"screens.{name}.tokens.{k}", errors,
                                      palette)
                if "deco" in spec:
                    deco = spec["deco"]
                    if not isinstance(deco, list) or len(deco) > 4:
                        fail(errors, f"screens.{name}.deco: 最大4件の配列")
                    else:
                        for i, d in enumerate(deco):
                            w = f"screens.{name}.deco[{i}]"
                            if not isinstance(d, dict):
                                fail(errors, f"{w}: object")
                                continue
                            for k in d:
                                if k not in ("img", "x", "y", "z"):
                                    fail(errors, f"{w}.{k}: 未知のキー")
                            check_image(theme_dir, d.get("img"), f"{w}.img",
                                        errors, files, (410, 502))
                            uint(d.get("x", 0), f"{w}.x", errors, -2048, 2048)
                            uint(d.get("y", 0), f"{w}.y", errors, -2048, 2048)
                            if d.get("z", "back") not in ("back", "front"):
                                fail(errors, f"{w}.z: back|front")
    if "style" in m:
        st = m["style"]
        if not isinstance(st, dict):
            fail(errors, "style: object")
        else:
            for k, v in st.items():
                if k not in STYLE_KEYS:
                    fail(errors, f"style.{k}: 未知のキー")
                elif k in ("border", "glow_color"):
                    color(v, f"style.{k}", errors, palette)
                elif k == "header":
                    if v not in ("default", "flat"):
                        fail(errors, "style.header: default|flat")
                else:
                    uint(v, f"style.{k}", errors, 0,
                         255 if k == "card_opa" else 64)
    icons = m.get("icons", {})
    if not isinstance(icons, dict):
        fail(errors, "icons: object")
        icons = {}
    if len(icons) > 12:
        fail(errors, f"icons: 最大12件 (got {len(icons)})")
    for app, name in icons.items():
        if not re.match(r"^[a-z0-9_]{1,15}$", app):
            fail(errors, f"icons.{app}: [a-z0-9_] 15文字まで")
        check_image(theme_dir, name, f"icons.{app}", errors, files)
    fonts = m.get("fonts", {})
    if not isinstance(fonts, dict):
        fail(errors, "fonts: object")
        fonts = {}
    for slot, name in fonts.items():
        if slot not in FONT_SLOTS:
            fail(errors, f"fonts.{slot}: 未知のスロット")
            continue
        p = resolve_file(theme_dir, name, f"fonts.{slot}", errors, files)
        if p and not name.endswith(".bin"):
            fail(errors, f"fonts.{slot}: lv_binfont の .bin を指定する")
    if "mascot" in m:
        mc = m["mascot"]
        if not isinstance(mc, dict):
            fail(errors, "mascot: object")
        else:
            for k in mc:
                if k not in ("x", "y", "expr", "lines", "screens"):
                    fail(errors, f"mascot.{k}: 未知のキー")
            uint(mc.get("x", 0), "mascot.x", errors, -512, 512)
            uint(mc.get("y", 0), "mascot.y", errors, -512, 512)
            expr = mc.get("expr", {})
            if not isinstance(expr, dict) or len(expr) > 4:
                fail(errors, "mascot.expr: 最大4件の object")
            else:
                for name, f in expr.items():
                    check_image(theme_dir, f, f"mascot.expr.{name}", errors,
                                files, (240, 240))
            lines = mc.get("lines", [])
            if not isinstance(lines, list) or len(lines) > 8:
                fail(errors, "mascot.lines: 最大8件の配列")
            else:
                for i, line in enumerate(lines):
                    if not isinstance(line, str) or len(line.encode()) > 48:
                        fail(errors, f"mascot.lines[{i}]: 48バイトまで")
            screens = mc.get("screens", [])
            if not isinstance(screens, list):
                fail(errors, "mascot.screens: 配列")
            else:
                for s in screens:
                    if s != "*" and s not in SCREENS:
                        fail(errors, f"mascot.screens: 未知の画面 '{s}'")
    if "face_layout" in m:
        fl = m["face_layout"]
        if not isinstance(fl, dict):
            fail(errors, "face_layout: object")
        else:
            for elem, spec in fl.items():
                if elem not in FACE_ELEMS:
                    fail(errors, f"face_layout.{elem}: 未知の要素")
                    continue
                if not isinstance(spec, dict):
                    fail(errors, f"face_layout.{elem}: object")
                    continue
                for k in spec:
                    if k not in ("x", "y", "font", "color", "w", "h", "img"):
                        fail(errors, f"face_layout.{elem}.{k}: 未知のキー")
                uint(spec.get("x", 0), f"face_layout.{elem}.x", errors,
                     -512, 512)
                uint(spec.get("y", 0), f"face_layout.{elem}.y", errors,
                     -512, 512)
                if "font" in spec and spec["font"] not in FONT_SIZES:
                    fail(errors, f"face_layout.{elem}.font: 20/26/56/96")
                if "color" in spec:
                    color(spec["color"], f"face_layout.{elem}.color", errors,
                          palette)
                if "img" in spec:
                    check_image(theme_dir, spec["img"],
                                f"face_layout.{elem}.img", errors, files)
    if "skin" in m:
        sk = m["skin"]
        if not isinstance(sk, dict):
            fail(errors, "skin: object")
        else:
            for part, spec in sk.items():
                if part not in SKIN_PARTS:
                    fail(errors, f"skin.{part}: 未知のパーツ")
                    continue
                if not isinstance(spec, dict):
                    fail(errors, f"skin.{part}: object")
                    continue
                for k in spec:
                    if k not in ("img", "slice", "states", "pad", "text",
                                 "text_pressed"):
                        fail(errors, f"skin.{part}.{k}: 未知のキー")
                check_image(theme_dir, spec.get("img"), f"skin.{part}.img",
                            errors, files)
                for k in ("slice", "pad"):
                    if k in spec:
                        v = spec[k]
                        if (not isinstance(v, list) or len(v) != 4 or
                                not all(isinstance(n, int) and 0 <= n <= 200
                                        for n in v)):
                            fail(errors, f"skin.{part}.{k}: 4つの整数 (0..200)")
                if "states" in spec:
                    if not isinstance(spec["states"], dict):
                        fail(errors, f"skin.{part}.states: object")
                    else:
                        for st, f in spec["states"].items():
                            if st not in SKIN_STATES:
                                fail(errors, f"skin.{part}.states.{st}: 未知")
                            check_image(theme_dir, f,
                                        f"skin.{part}.states.{st}", errors,
                                        files)
                for k in ("text", "text_pressed"):
                    if k in spec:
                        color(spec[k], f"skin.{part}.{k}", errors, palette)
    assets = m.get("assets", {})
    if not isinstance(assets, dict):
        fail(errors, "assets: object")
        assets = {}
    for aid, name in assets.items():
        if not ASSET_ID_RE.match(aid):
            fail(errors, f"assets: id '{aid}' は [a-z0-9_.-] 31文字まで")
        check_image(theme_dir, name, f"assets.{aid}", errors, files)
    return errors, files


def encode_manifest(m):
    out = {}
    out["id"] = c_text(m["id"])
    out["api"] = c_uint(1)
    if "name" in m:
        out["name"] = c_text(m["name"])
    if "version" in m:
        out["version"] = c_uint(m["version"])
    tokens = m.get("tokens", {})
    if tokens:
        pairs = {}
        for k in tokens:
            pairs[k] = (c_text(tokens[k]) if k in COLOR_KEYS
                        else c_uint(tokens[k]))
        out["tokens"] = c_map(pairs)
    if m.get("images"):
        out["images"] = c_map({k: c_text(v) for k, v in m["images"].items()})
    if m.get("bubble"):
        out["bubble"] = c_map({k: c_text(v) for k, v in m["bubble"].items()})
    if m.get("screens"):
        screens = {}
        for name, spec in m["screens"].items():
            sm = {}
            if "bg" in spec:
                sm["bg"] = c_text(spec["bg"])
            if "scrim" in spec:
                sm["scrim"] = c_uint(spec["scrim"])
            if spec.get("tokens"):
                sm["tokens"] = c_map(
                    {k: (c_text(v) if k in COLOR_KEYS else c_uint(v))
                     for k, v in spec["tokens"].items()})
            if spec.get("deco"):
                sm["deco"] = c_arr([
                    c_map({"img": c_text(d["img"]),
                           "x": c_int(d.get("x", 0)),
                           "y": c_int(d.get("y", 0)),
                           "z": c_text(d.get("z", "back"))})
                    for d in spec["deco"]])
            screens[name] = c_map(sm)
        out["screens"] = c_map(screens)
    if m.get("style"):
        st = {}
        for k, v in m["style"].items():
            if k in ("border", "glow_color", "header"):
                st[k] = c_text(v)
            else:
                st[k] = c_uint(v)
        out["style"] = c_map(st)
    if m.get("icons"):
        out["icons"] = c_map({k: c_text(v) for k, v in m["icons"].items()})
    if m.get("fonts"):
        out["fonts"] = c_map({k: c_text(v) for k, v in m["fonts"].items()})
    if m.get("mascot"):
        mc = m["mascot"]
        mm = {}
        if "x" in mc:
            mm["x"] = c_int(mc["x"])
        if "y" in mc:
            mm["y"] = c_int(mc["y"])
        if mc.get("expr"):
            mm["expr"] = c_map({k: c_text(v) for k, v in mc["expr"].items()})
        if mc.get("lines"):
            mm["lines"] = c_arr([c_text(v) for v in mc["lines"]])
        if mc.get("screens"):
            mm["screens"] = c_arr([c_text(v) for v in mc["screens"]])
        out["mascot"] = c_map(mm)
    if m.get("face_layout"):
        fl = {}
        for elem, spec in m["face_layout"].items():
            em = {}
            for k, v in spec.items():
                if k in ("color", "img"):
                    em[k] = c_text(v)
                else:
                    em[k] = c_int(v) if k in ("x", "y") else c_uint(v)
            fl[elem] = c_map(em)
        out["face_layout"] = c_map(fl)
    if m.get("skin"):
        sk = {}
        for part, spec in m["skin"].items():
            pm = {"img": c_text(spec["img"])}
            if "slice" in spec:
                pm["slice"] = c_arr([c_uint(v) for v in spec["slice"]])
            if "pad" in spec:
                pm["pad"] = c_arr([c_uint(v) for v in spec["pad"]])
            if spec.get("states"):
                pm["states"] = c_map(
                    {k: c_text(v) for k, v in spec["states"].items()})
            for k in ("text", "text_pressed"):
                if k in spec:
                    pm[k] = c_text(spec[k])
            sk[part] = c_map(pm)
        out["skin"] = c_map(sk)
    if m.get("assets"):
        out["assets"] = c_map({k: c_text(v) for k, v in m["assets"].items()})
    if m.get("palette"):
        out["palette"] = c_map({k: c_text(v) for k, v in m["palette"].items()})
    return c_map(out)


def write_zip(path, entries):
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, "w", zipfile.ZIP_STORED) as z:
        for name, blob in entries:
            z.writestr(name, blob)


def build(theme_dir, preview, android):
    jpath = theme_dir if theme_dir.is_file() else theme_dir / "theme.json"
    if not jpath.is_file():
        raise ThemeError(f"{jpath} がない")
    theme_dir = jpath.parent
    m = json.loads(jpath.read_text(encoding="utf-8"))
    errors, files = validate(m, theme_dir)
    if errors:
        raise ThemeError("\n".join("  - " + e for e in errors))
    manifest = encode_manifest(m)
    entries = [("manifest.cbor", manifest)]
    for name in files:
        entries.append((name, files[name].read_bytes()))
    total = sum(len(b) for _, b in entries)
    if total > PKG_MAX:
        raise ThemeError(f"パッケージ {total} B が上限 {PKG_MAX} B を超える")
    tid = m["id"]
    targets = [REPO / "sim" / "themes" / f"{tid}.zip",
               REPO / "tools" / "themes" / f"{tid}.zip"]
    if android:
        targets.append(REPO / "android" / "app" / "src" / "main" / "assets"
                       / "themes" / f"{tid}.zip")
    for t in targets:
        write_zip(t, entries)
    print(f"theme '{tid}': {len(entries)} entries, {total} B")
    for t in targets:
        print(f"  {t.relative_to(REPO)}  {t.stat().st_size} B")
    if preview:
        run_preview(tid)
    return tid


def run_preview(tid):
    sim = REPO / "sim" / "build" / "snapshot"
    if not sim.is_file():
        raise ThemeError("sim が未ビルド: cmake -S sim -B sim/build && "
                         "cmake --build sim/build -j")
    out = REPO / "sim" / "screenshots" / f"theme_{tid}"
    out.mkdir(parents=True, exist_ok=True)
    p = subprocess.run([str(sim), "--theme", tid, "--out", str(out)],
                       cwd=REPO, capture_output=True, text=True)
    tail = [l for l in p.stdout.splitlines() if l.startswith("done ->")]
    print("  preview:", " / ".join(tail) if tail else "(no done line)")
    for name in ("90_theme_custom_home.png", "91_theme_custom_settings.png"):
        f = out / name
        if f.is_file():
            print(f"  {f.relative_to(REPO)}")
    parts = sorted(out.glob("*_parts.png"))
    for f in parts[:1]:
        print(f"  {f.relative_to(REPO)}")
    if p.returncode != 0:
        raise ThemeError("プレビュー失敗 (監査違反 or テーマ読込失敗)\n" +
                         "\n".join(p.stdout.splitlines()[-8:]))


def main():
    ap = argparse.ArgumentParser(description="theme.json から zip を作る")
    ap.add_argument("theme", help="themes/<id> か theme.json のパス")
    ap.add_argument("--check", action="store_true", help="検証だけ")
    ap.add_argument("--preview", action="store_true",
                    help="ビルド後に sim で Home/設定/パーツを撮る")
    ap.add_argument("--no-android", action="store_true",
                    help="Android の assets へコピーしない")
    a = ap.parse_args()
    p = Path(a.theme)
    if not p.is_absolute():
        p = REPO / p
    try:
        if a.check:
            jpath = p if p.is_file() else p / "theme.json"
            m = json.loads(jpath.read_text(encoding="utf-8"))
            errors, _ = validate(m, jpath.parent)
            if errors:
                raise ThemeError("\n".join("  - " + e for e in errors))
            print(f"ok: {jpath}")
        else:
            build(p, a.preview, not a.no_android)
    except ThemeError as e:
        print("error:\n" + str(e), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
