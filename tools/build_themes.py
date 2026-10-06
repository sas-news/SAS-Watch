#!/usr/bin/env python3
"""tools/build_themes.py — テーマパッケージをビルドする。

  python3 tools/build_themes.py

出力:
  sim/themes/<id>/                      展開済みパッケージ (sim が読む)
  android/app/src/main/assets/themes/<id>.zip   アプリ同梱サンプル
  tools/themes/<id>.zip                 手元に置く同じパッケージ
  tools/themes/<id>_preview.png         画像プレビュー (PR 添付用)

依存は Python 標準ライブラリのみ。画像は自作のベクター図形
(楕円・多角形を 2x SSAA で描いて RGB565A8 に落とす)。
他者の著作物は使わない。
"""

import io
import math
import os
import struct
import sys
import zipfile
import zlib

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# ---------------------------------------------------------------- CBOR

def cbor_head(major, v):
    if v < 24:
        return bytes([(major << 5) | v])
    if v < 256:
        return bytes([(major << 5) | 24, v])
    if v < 65536:
        return bytes([(major << 5) | 25]) + struct.pack(">H", v)
    return bytes([(major << 5) | 26]) + struct.pack(">I", v)


def cbor_uint(v):
    return cbor_head(0, v)


def cbor_text(s):
    b = s.encode("utf-8")
    return cbor_head(3, len(b)) + b


def cbor_map(pairs):
    out = cbor_head(5, len(pairs))
    for k, v in pairs:
        out += cbor_text(k) + v
    return out


# ---------------------------------------------------------------- 描画
# キャンバスは RGBA (各 0-255)。2x で描いて最後に box ダウンサンプル。

class Canvas:
    def __init__(self, w, h, ss=2):
        self.w, self.h, self.ss = w, h, ss
        self.W, self.H = w * ss, h * ss
        self.px = bytearray(self.W * self.H * 4)

    def _set(self, x, y, c):
        if 0 <= x < self.W and 0 <= y < self.H:
            i = (y * self.W + x) * 4
            self.px[i:i + 4] = bytes(c)

    def blend(self, x, y, c):
        """c=(r,g,b,a) を通常アルファ合成で置く。"""
        if not (0 <= x < self.W and 0 <= y < self.H):
            return
        i = (y * self.W + x) * 4
        sa = c[3]
        if sa == 255:
            self.px[i:i + 4] = bytes(c)
            return
        da = self.px[i + 3]
        oa = sa + da * (255 - sa) // 255
        if oa == 0:
            return
        for k in range(3):
            d = self.px[i + k]
            v = (c[k] * sa + d * da * (255 - sa) // 255) // oa
            self.px[i + k] = min(255, v)
        self.px[i + 3] = oa

    def ellipse(self, cx, cy, rx, ry, c):
        """塗り楕円 (中心 cx,cy, 半径 rx,ry)。"""
        x0, x1 = int(cx - rx - 1), int(cx + rx + 1)
        y0, y1 = int(cy - ry - 1), int(cy + ry + 1)
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                dx = (x + 0.5 - cx) / rx
                dy = (y + 0.5 - cy) / ry
                d = dx * dx + dy * dy
                if d <= 1.0:
                    self.blend(x, y, c)
                elif d < 1.0 + 2.0 / min(rx, ry):
                    # 端のアンチエイリアス
                    edge = (d - 1.0) / (2.0 / min(rx, ry))
                    a = int(c[3] * max(0.0, 1.0 - edge))
                    if a:
                        self.blend(x, y, (c[0], c[1], c[2], a))

    def circle(self, cx, cy, r, c):
        self.ellipse(cx, cy, r, r, c)

    def rect(self, x0, y0, x1, y1, c):
        for y in range(int(y0), int(y1)):
            for x in range(int(x0), int(x1)):
                self.blend(x, y, c)

    def polygon(self, pts, c):
        """塗り多角形 (スキャンライン)。pts=[(x,y),...]"""
        ys = [p[1] for p in pts]
        y0, y1 = int(min(ys)), int(max(ys)) + 1
        n = len(pts)
        for y in range(y0, y1):
            xs = []
            for i in range(n):
                x1, y_1 = pts[i]
                x2, y_2 = pts[(i + 1) % n]
                if (y_1 <= y + 0.5 < y_2) or (y_2 <= y + 0.5 < y_1):
                    t = (y + 0.5 - y_1) / (y_2 - y_1)
                    xs.append(x1 + t * (x2 - x1))
            xs.sort()
            for i in range(0, len(xs) - 1, 2):
                for x in range(int(xs[i]), int(xs[i + 1]) + 1):
                    self.blend(x, y, c)

    def ring(self, cx, cy, r, thick, c):
        """輪郭 (外径 r, 太さ thick)。"""
        r2o, r2i = r * r, (r - thick) * (r - thick)
        for y in range(int(cy - r - 1), int(cy + r + 1)):
            for x in range(int(cx - r - 1), int(cx + r + 1)):
                d = (x + 0.5 - cx) ** 2 + (y + 0.5 - cy) ** 2
                if r2i <= d <= r2o:
                    self.blend(x, y, c)

    def downsampled(self):
        """ss 倍キャンバス → 実寸 RGBA バイト列。"""
        s = self.ss
        out = bytearray(self.w * self.h * 4)
        for y in range(self.h):
            for x in range(self.w):
                acc = [0, 0, 0, 0]
                for dy in range(s):
                    for dx in range(s):
                        i = ((y * s + dy) * self.W + x * s + dx) * 4
                        for k in range(4):
                            acc[k] += self.px[i + k]
                j = (y * self.w + x) * 4
                for k in range(4):
                    out[j + k] = acc[k] // (s * s)
        return out


def rgb565a8_bin(rgba, w, h):
    """RGBA バイト列 → LVGL .bin (RGB565A8: 色 w*h*2 + α w*h)。"""
    head = struct.pack("<BBHHHHH", 0x19, 0x14, 0, w, h, w * 2, 0)
    color = bytearray(w * h * 2)
    alpha = bytearray(w * h)
    for y in range(h):
        for x in range(w):
            i = (y * w + x) * 4
            r, g, b, a = rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]
            v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            j = (y * w + x) * 2
            color[j] = v & 0xFF
            color[j + 1] = v >> 8
            alpha[y * w + x] = a
    return head + bytes(color) + bytes(alpha)


def write_png(path, rgba, w, h):
    """RGBA → PNG (プレビュー用)。"""
    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(
            ">I", zlib.crc32(c) & 0xFFFFFFFF)

    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw += rgba[y * w * 4:(y + 1) * w * 4]
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
           + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)


# ---------------------------------------------------------------- まめ
# テーマ「まめ」: 豆のキャラクター。配色は落ち着いた暖色系ダーク。

MAME_TOKENS = {
    "bg": "0x0C0D11", "surface": "0x1C1812", "surface2": "0x2C2417",
    "primary": "0xFFB84D", "on_primary": "0x2A1A00",
    "text": "0xFFF4E2", "text_dim": "0xBCA98C",
    "accent": "0x8FD98F", "danger": "0xFF6E6E", "ok": "0x7ED4A0",
    "accent2": "0xE8D7B0", "accent3": "0x7ED4A0", "accent4": "0xB08DFF",
    "accent5": "0xFF9DBB",
    "bubble_bg": "0xFFF8EC", "bubble_text": "0x1A0F2E",
    "radius_sm": 12, "radius_lg": 20, "space": 8, "anim_ms": 200,
    "font_body": 20, "font_title": 26, "font_digits": 96,
    "font_digits_sm": 56,
}

# 文字盤 chara_bubble のふきだし文言 (manifest "bubble"、{n} に残り歩数)。
MAME_BUBBLE = {
    "morning": "おはよう！",
    "noon": "こんにちは！",
    "evening": "おつかれさま！",
    "night": "おやすみー",
    "steps": "あと{n}歩だよ",
}

# パレット
BODY = (255, 216, 160, 255)      # 豆の体
BODY_SHADE = (240, 190, 130, 255)
LEAF = (126, 200, 120, 255)      # 芽
LEAF_DARK = (90, 160, 90, 255)
INK = (46, 36, 30, 255)          # 目・口
BLUSH = (245, 160, 130, 140)     # ほっぺ
HILL = (44, 36, 24, 255)         # 丘 (home_bg)
STAR = (255, 200, 120, 255)


def draw_mame(cv, cx, cy, scale, arms_up=False):
    """まめの体を描く。cx,cy は体の中心 (SSAA キャンバス座標)。"""
    s = cv.ss
    rx, ry = 34 * scale * s, 42 * scale * s

    # 影
    cv.ellipse(cx, cy + ry + 4 * s, rx * 0.8, 6 * scale * s,
               (0, 0, 0, 90))
    # 体 (豆形: 上が少し尖った楕円の合成)
    cv.ellipse(cx, cy, rx, ry, BODY)
    cv.ellipse(cx, cy - ry * 0.9, rx * 0.55, ry * 0.35, BODY)
    # 体の陰 (右下に薄く)
    cv.ellipse(cx + rx * 0.3, cy + ry * 0.45, rx * 0.55, ry * 0.4,
               BODY_SHADE)
    cv.ellipse(cx, cy, rx, ry, BODY)  # 上に本体を重ねて陰を縁に留める
    cv.ellipse(cx, cy - ry * 0.9, rx * 0.55, ry * 0.35, BODY)
    # 頭の芽
    cv.ellipse(cx - 6 * scale * s, cy - ry - 6 * scale * s,
               9 * scale * s, 4.5 * scale * s, LEAF)
    cv.ellipse(cx + 6 * scale * s, cy - ry - 7 * scale * s,
               8 * scale * s, 4 * scale * s, LEAF_DARK)

    # 目
    ey = cy - 6 * scale * s
    cv.ellipse(cx - 12 * scale * s, ey, 4.5 * scale * s, 6 * scale * s, INK)
    cv.ellipse(cx + 12 * scale * s, ey, 4.5 * scale * s, 6 * scale * s, INK)
    # 目のハイライト
    cv.circle(cx - 11 * scale * s, ey - 2 * scale * s, 1.6 * scale * s,
              (255, 255, 255, 220))
    cv.circle(cx + 13 * scale * s, ey - 2 * scale * s, 1.6 * scale * s,
              (255, 255, 255, 220))
    # ほっぺ
    cv.ellipse(cx - 20 * scale * s, cy + 6 * scale * s,
               6 * scale * s, 3.5 * scale * s, BLUSH)
    cv.ellipse(cx + 20 * scale * s, cy + 6 * scale * s,
               6 * scale * s, 3.5 * scale * s, BLUSH)
    # 口 (小さな楕円)
    cv.ellipse(cx, cy + 8 * scale * s, 3.5 * scale * s, 2.5 * scale * s, INK)

    # 腕 (体の横の小楕円。arms_up なら持ち上げ)
    if arms_up:
        cv.ellipse(cx - rx - 4 * scale * s, cy - ry * 0.8,
                   6 * scale * s, 10 * scale * s, BODY)
        cv.ellipse(cx + rx + 4 * scale * s, cy - ry * 0.8,
                   6 * scale * s, 10 * scale * s, BODY)
    else:
        cv.ellipse(cx - rx - 3 * scale * s, cy + 4 * scale * s,
                   5 * scale * s, 8 * scale * s, BODY_SHADE)
        cv.ellipse(cx + rx + 3 * scale * s, cy + 4 * scale * s,
                   5 * scale * s, 8 * scale * s, BODY_SHADE)


def gen_home_bg():
    """410x240 の帯 (Home 下部に敷く)。丘 + 星 + 座るまめ。"""
    cv = Canvas(410, 240)
    s = cv.ss
    # 丘 (下半分の大きな楕円弧)
    cv.ellipse(205 * s, 340 * s, 260 * s, 110 * s, HILL)
    # 丘のハイライト
    cv.ellipse(205 * s, 336 * s, 258 * s, 108 * s, (56, 46, 30, 255))
    cv.ellipse(205 * s, 344 * s, 258 * s, 108 * s, HILL)
    # 星 (小さな十字)
    for (sx, sy, r) in [(60, 40, 4), (120, 90, 3), (300, 50, 5),
                        (350, 110, 3), (200, 30, 3), (30, 140, 3)]:
        cv.circle(sx * s, sy * s, r * s, STAR)
        cv.circle(sx * s, sy * s, r * 1.8 * s, (255, 200, 120, 50))
    # キャラ自体は stand スロット側 (home_bg は背景装飾のみ)。
    return cv.downsampled()


def gen_stand():
    """160x200 の立ち絵 (Home 右下)。"""
    cv = Canvas(160, 200)
    s = cv.ss
    draw_mame(cv, 80 * s, 118 * s, 1.15)
    return cv.downsampled()


def gen_timer_done():
    """320x240。両手を上げたまめ + 紙吹雪。"""
    cv = Canvas(320, 240)
    s = cv.ss
    # 紙吹雪 (三角/小四角)
    conf = [
        [(40, 40), (52, 30), (56, 46)], [(260, 36), (272, 44), (258, 54)],
        [(80, 70), (88, 62), (92, 74)], [(230, 80), (240, 72), (244, 84)],
        [(50, 120), (60, 114), (64, 126)], [(268, 130), (278, 122), (282, 134)],
    ]
    conf_col = [STAR, LEAF, (255, 140, 140, 255), (140, 200, 255, 255)]
    for i, tri in enumerate(conf):
        cv.polygon([(x * s, y * s) for x, y in tri],
                   conf_col[i % len(conf_col)])
    for (sx, sy) in [(100, 45), (215, 40), (30, 180), (290, 190)]:
        cv.circle(sx * s, sy * s, 3 * s, STAR)
    draw_mame(cv, 160 * s, 128 * s, 1.35, arms_up=True)
    return cv.downsampled()


def gen_face_chara():
    """240x410 の文字盤用立ち絵 (chara_side / chara_bubble)。透過。"""
    cv = Canvas(240, 410)
    s = cv.ss
    # 見本の「右に大きく」に合わせ、体の中心をやや右下に置く。
    draw_mame(cv, 120 * s, 205 * s, 3.2)
    return cv.downsampled()


# ---------------------------------------------------------------- 出力

def emit_assets(id, files, names_map=None):
    """files: {name: (w,h, rgba_or_raw)} を <id>/ 展開と <id>.zip に書く。
    names_map: {name: エントリ名} (png 等、既定は name+".bin")。"""
    sim_dir = os.path.join(REPO, "sim", "themes", id)
    os.makedirs(sim_dir, exist_ok=True)
    entries = {}
    for name, item in files.items():
        w, h, data = item
        ename = (names_map or {}).get(name, name + ".bin")
        blob = data if isinstance(data, (bytes, bytearray)) else \
            rgb565a8_bin(data, w, h)
        entries[ename] = blob
        print(f"  {ename}  {w}x{h}  {len(blob)} B")

    for out_zip in (
        os.path.join(REPO, "android", "app", "src", "main", "assets",
                     "themes", id + ".zip"),
        os.path.join(REPO, "tools", "themes", id + ".zip"),
    ):
        os.makedirs(os.path.dirname(out_zip), exist_ok=True)
        with zipfile.ZipFile(out_zip, "w", zipfile.ZIP_STORED) as z:
            for ename, blob in entries.items():
                z.writestr(ename, blob)
        print(f"  {os.path.relpath(out_zip, REPO)}  "
              f"{os.path.getsize(out_zip)} B (stored)")
    return entries


def build_mame():
    imgs = {
        "home_bg": gen_home_bg(),        # 410x240
        "stand": gen_stand(),            # 160x200
        "timer_done": gen_timer_done(),  # 320x240
        "face_chara": gen_face_chara(),  # 240x410
    }
    dims = {"home_bg": (410, 240), "stand": (160, 200),
            "timer_done": (320, 240), "face_chara": (240, 410)}

    manifest = cbor_map([
        ("id", cbor_text("mame")),
        ("api", cbor_uint(1)),
        ("name", cbor_text("まめ")),
        ("version", cbor_uint(1)),
        ("tokens", cbor_map([(k, cbor_text(v) if isinstance(v, str)
                              else cbor_uint(v))
                             for k, v in MAME_TOKENS.items()])),
        ("images", cbor_map([(k, cbor_text(k + ".bin"))
                             for k in imgs.keys()])),
        ("bubble", cbor_map([(k, cbor_text(v))
                             for k, v in MAME_BUBBLE.items()])),
    ])

    # 展開済み (sim 用) + zip (配布用)
    sim_dir = os.path.join(REPO, "sim", "themes", "mame")
    os.makedirs(sim_dir, exist_ok=True)
    with open(os.path.join(sim_dir, "manifest.cbor"), "wb") as f:
        f.write(manifest)
    total = 0
    for slot, rgba in imgs.items():
        w, h = dims[slot]
        b = rgb565a8_bin(rgba, w, h)
        total += len(b)
        with open(os.path.join(sim_dir, slot + ".bin"), "wb") as f:
            f.write(b)
        print(f"  {slot}.bin  {w}x{h}  {len(b)} B")

    for out_zip in (
        os.path.join(REPO, "android", "app", "src", "main", "assets",
                     "themes", "mame.zip"),
        os.path.join(REPO, "tools", "themes", "mame.zip"),
    ):
        os.makedirs(os.path.dirname(out_zip), exist_ok=True)
        with zipfile.ZipFile(out_zip, "w", zipfile.ZIP_STORED) as z:
            z.writestr("manifest.cbor", manifest)
            for slot, rgba in imgs.items():
                w, h = dims[slot]
                z.writestr(slot + ".bin", rgb565a8_bin(rgba, w, h))
        print(f"  {os.path.relpath(out_zip, REPO)}  "
              f"{os.path.getsize(out_zip)} B (stored)")

    # プレビュー PNG (4 画像を横に並べる。face_chara は縦長なので縮めて置く)
    pw = 410 + 160 + 320 + 240 + 60
    ph = 240
    prev = Canvas(pw, ph, ss=1)
    prev.rect(0, 0, pw, ph, (12, 13, 17, 255))
    x = 0
    for slot in ("home_bg", "stand", "timer_done", "face_chara"):
        rgba, (w, h) = imgs[slot], dims[slot]
        sc = min(1.0, ph / h)  # 高さが ph を超える画像は縮小表示
        ow, oh = int(w * sc), int(h * sc)
        for y in range(oh):
            for xx in range(ow):
                i = (int(y / sc) * w + int(xx / sc)) * 4
                a = rgba[i + 3]
                if a:
                    prev.blend(x + xx, y,
                               (rgba[i], rgba[i + 1], rgba[i + 2], a))
        x += ow + 20
    write_png(os.path.join(REPO, "tools", "themes", "mame_preview.png"),
              prev.px, pw, ph)
    print(f"  tools/themes/mame_preview.png")
    print(f"  pixels total {total} B <= 2.5MiB: {total <= 2621440}")


# ================================================================ cosmos
# テーマ v2 サンプル「cosmos」: 宇宙×HUD。アートは全部このスクリプトが
# Canvas で描く自作 PNG (2x/4x SSAA)。フォントは tools/themes/cosmos/
# の lv_font_conv 生成 .bin を同梱 (JetBrains Mono Bold, SIL OFL)。

COS_TOKENS = {
    "bg": "0x050A18", "surface": "0x0C1628", "surface2": "0x16233C",
    "line": "0x27405E", "edge": "0x3E5578",
    "text": "0xEAF6FF", "text_dim": "0x7E95B8",
    "primary": "0x55C8FF", "primary2": "0x9AE8FF",
    "on_primary": "0x02101F", "danger": "0xFF6E7A", "ok": "0x4BE3A8",
    "accent": "0xFFB45C", "accent2": "0x8FA8FF", "accent3": "0x4BE3A8",
    "accent4": "0xFF8FB8", "accent5": "0x6CE0FF",
    "bubble_bg": "0x10233F", "bubble_text": "0xCFEAFF",
    "radius_sm": 8, "radius_lg": 14, "space": 8, "anim_ms": 160,
    "font_body": 20, "font_title": 26, "font_digits": 96,
    "font_digits_sm": 56,
}

# パレット
SPACE_HI = (14, 22, 46, 255)     # 画面上部の濃紺
SPACE_LO = (4, 7, 18, 255)       # 下部のほぼ黒
NEBULA_P = (96, 60, 160, 26)     # 紫の星雲
NEBULA_C = (40, 120, 180, 22)    # 青い星雲
STAR_W = (220, 240, 255, 255)    # 星
HUD = (85, 200, 255, 120)        # シアンの HUD 線
HUD_DIM = (85, 200, 255, 46)
PLANET = (30, 60, 110, 255)      # 青い惑星
PLANET_HI = (90, 160, 220, 255)
SUN = (255, 150, 70, 255)        # オレンジの恒星
BOT = (210, 235, 255, 255)       # 星ロボ本体
BOT_SH = (120, 170, 220, 255)
BOT_INK = (10, 24, 44, 255)
BOT_EYE = (70, 220, 255, 255)


def cv_gradient(cv, top, bottom):
    """縦方向の線形グラデーションで全面を塗る。"""
    s = cv.ss
    for y in range(cv.H):
        t = y / max(1, cv.H - 1)
        c = tuple(int(top[k] + (bottom[k] - top[k]) * t) for k in range(4))
        cv.rect(0, y, cv.W, y + 1, c)


def cv_line(cv, x0, y0, x1, y1, thick, c):
    """太さ thick の直線 (サンプル刻みで丸を敷く)。"""
    dx, dy = x1 - x0, y1 - y0
    n = int(max(abs(dx), abs(dy))) + 1
    r = max(1, int(thick / 2))
    for i in range(n + 1):
        t = i / n
        cv.circle(x0 + dx * t, y0 + dy * t, r, c)


def cv_sparkle(cv, cx, cy, r, c):
    """十字キラ星。"""
    cv_line(cv, cx - r, cy, cx + r, cy, cv.ss, c)
    cv_line(cv, cx, cy - r, cx, cy + r, cv.ss, c)
    cv.circle(cx, cy, r * 0.35, c)


def cv_ellipse_ring(cv, cx, cy, rx, ry, thick, c):
    """楕円の輪郭を角度サンプリングで描く。"""
    n = int(max(rx, ry) * 6.3) + 1
    for i in range(n + 1):
        a = 2 * math.pi * i / n
        cv.circle(cx + rx * math.cos(a), cy + ry * math.sin(a),
                  thick / 2, c)


def scatter_stars(cv, seed, count, x1=None, y1=None, excl=None):
    """決定的な疑似乱数で星を散らす (x1,y1 未満の領域)。

    excl=(x1,y1,x2,y2) は最終ピクセル座標の除外矩形 — テキストや
    UI の領域に飾りを重ねないために使う。"""
    rng = seed
    x1 = x1 or cv.W
    y1 = y1 or cv.H
    ex = None if excl is None else tuple(v * cv.ss for v in excl)

    def blocked(sx, sy):
        return ex is not None and ex[0] <= sx <= ex[2] and ex[1] <= sy <= ex[3]

    for _ in range(count):
        rng = (rng * 1103515245 + 12345) & 0x7FFFFFFF
        sx = rng % x1
        rng = (rng * 1103515245 + 12345) & 0x7FFFFFFF
        sy = rng % y1
        rng = (rng * 1103515245 + 12345) & 0x7FFFFFFF
        rr = 1 + rng % 3
        a = 90 + rng % 160
        if not blocked(sx, sy):
            cv.circle(sx, sy, rr * cv.ss / 2, (220, 240, 255, a))
    # 大きめのキラ星を数個
    for i in range(6):
        rng = (rng * 1103515245 + 12345) & 0x7FFFFFFF
        sx = rng % x1
        rng = (rng * 1103515245 + 12345) & 0x7FFFFFFF
        sy = rng % (y1 * 3 // 4)
        if not blocked(sx, sy):
            cv_sparkle(cv, sx, sy, 6 * cv.ss, (190, 230, 255, 200))


def draw_planet(cv, cx, cy, r, base, hi, ring=False):
    """惑星: 球 + 明暗 + (任意) リング。"""
    cv.circle(cx, cy, r, base)
    # 明暗: 左上に薄いハイライト
    cv.ellipse(cx - r * 0.35, cy - r * 0.4, r * 0.5, r * 0.35,
               (hi[0], hi[1], hi[2], 70))
    # 模様の帯
    cv.ellipse(cx - r * 0.1, cy - r * 0.15, r * 0.95, r * 0.28,
               (hi[0], hi[1], hi[2], 40))
    cv.ellipse(cx + r * 0.2, cy + r * 0.35, r * 0.8, r * 0.2,
               (hi[0], hi[1], hi[2], 28))
    if ring:
        cv_ellipse_ring(cv, cx, cy + r * 0.1, r * 1.6, r * 0.5,
                        2 * cv.ss, (120, 190, 240, 140))


def draw_hud_frame(cv):
    """四隅の HUD ブラケット + 上下の細線。"""
    s = cv.ss
    w, h = cv.W, cv.H
    m = 16 * s   # 丸角にかからない内側オフセット
    ln = 30 * s  # ブラケットの腕長さ
    th = 2 * s
    # 上ブラケットはヘッダ (高さ 76px) と被らないよう下げる。
    top_y = m + 66 * s
    for (bx, by, sx, sy) in (
        (m, top_y, 1, 1), (w - m, top_y, -1, 1),
        (m, h - m - 12 * s, 1, -1), (w - m, h - m - 12 * s, -1, -1),
    ):
        cv_line(cv, bx, by, bx + sx * ln, by, th, HUD)
        cv_line(cv, bx, by, bx, by + sy * ln, th, HUD)
    # 上下中央の薄い水平線
    cv_line(cv, w * 0.35, m + 4 * s, w * 0.65, m + 4 * s, s, HUD_DIM)
    cv_line(cv, w * 0.35, h - m - 4 * s, w * 0.65, h - m - 4 * s, s, HUD_DIM)


def gen_cosmos_bg():
    """410x502: 濃紺グラデ + 星 + 星雲 + 右下の惑星 + HUD ブラケット。"""
    cv = Canvas(410, 502)
    s = cv.ss
    cv_gradient(cv, SPACE_HI, SPACE_LO)
    cv.ellipse(120 * s, 130 * s, 180 * s, 70 * s, NEBULA_P)
    cv.ellipse(300 * s, 300 * s, 160 * s, 60 * s, NEBULA_C)
    scatter_stars(cv, 42, 170)
    draw_planet(cv, 340 * s, 430 * s, 110 * s, PLANET, PLANET_HI, ring=True)
    draw_hud_frame(cv)
    return cv.downsampled()


def gen_cosmos_timer_bg():
    """410x502: 同じ宇宙だが軌道リング中心 + 下部に恒星。"""
    cv = Canvas(410, 502)
    s = cv.ss
    cv_gradient(cv, (10, 16, 40, 255), SPACE_LO)
    cv.ellipse(205 * s, 240 * s, 200 * s, 120 * s, (60, 80, 160, 20))
    scatter_stars(cv, 7, 160)
    # 軌道リング (中心に同心楕円 3 本 + 惑星)
    for k, rx in enumerate((90, 130, 170)):
        cv_ellipse_ring(cv, 205 * s, 240 * s, rx * s, rx * 0.32 * s,
                        s, HUD_DIM)
    draw_planet(cv, 205 * s, 240 * s, 60 * s, (60, 100, 160, 255),
                (140, 200, 255, 255))
    draw_planet(cv, 205 * s + 130 * s, 240 * s - 42 * s, 10 * s,
                SUN, (255, 210, 140, 255))
    draw_hud_frame(cv)
    return cv.downsampled()


def gen_cosmos_alert_bg():
    """410x502: 警戒の赤系。巨大な赤い惑星が昇る。"""
    cv = Canvas(410, 502)
    s = cv.ss
    cv_gradient(cv, (30, 10, 18, 255), (10, 4, 10, 255))
    cv.ellipse(205 * s, 380 * s, 260 * s, 150 * s, (200, 60, 60, 30))
    cv.ellipse(205 * s, 430 * s, 300 * s, 200 * s, (255, 110, 70, 26))
    # 00:00 (~y185-260) と タイマー終了 (~y295-330) の文字領域には星を置かない。
    scatter_stars(cv, 99, 140, excl=(30, 170, 380, 350))
    draw_planet(cv, 205 * s, 560 * s, 190 * s, (120, 30, 40, 255),
                (255, 140, 100, 255))
    draw_hud_frame(cv)
    return cv.downsampled()


def draw_bot(cv, cx, cy, scale, face="normal"):
    """星ロボ。cx,cy は体の中心 (SSAA 座標)。face: normal|smile|wink"""
    s = cv.ss
    r = 36 * scale * s
    # アンテナ
    cv.rect(cx - 1.5 * scale * s, cy - r - 16 * scale * s,
            cx + 1.5 * scale * s, cy - r + 2 * scale * s, BOT_SH)
    cv.circle(cx, cy - r - 18 * scale * s, 5 * scale * s, SUN)
    cv.circle(cx, cy - r - 18 * scale * s, 8 * scale * s,
              (255, 180, 90, 60))
    # 体 (球: 白〜水色)
    cv.circle(cx, cy, r, BOT)
    cv.ellipse(cx - r * 0.3, cy - r * 0.35, r * 0.45, r * 0.3,
               (255, 255, 255, 120))                 # ハイライト
    cv.ellipse(cx + r * 0.25, cy + r * 0.5, r * 0.55, r * 0.35,
               BOT_SH)                               # 右下の陰
    cv.circle(cx, cy, r, BOT)
    # バイザー (上半分の暗い帯)
    cv.ellipse(cx, cy - r * 0.18, r * 0.72, r * 0.4, BOT_INK)
    # 目
    ey = cy - r * 0.2
    if face == "wink":
        cv.circle(cx - r * 0.3, ey, 3.5 * scale * s, BOT_EYE)
        cv_line(cv, cx + r * 0.2, ey + scale * s, cx + r * 0.4,
                ey + scale * s, 2 * scale * s, BOT_EYE)  # ウィンク
    else:
        cv.circle(cx - r * 0.3, ey, 3.5 * scale * s, BOT_EYE)
        cv.circle(cx + r * 0.3, ey, 3.5 * scale * s, BOT_EYE)
        if face == "smile":
            # 目を湾曲させる (円 + 上を消す)
            cv.ellipse(cx - r * 0.3, ey - 1.5 * scale * s,
                       4 * scale * s, 3.5 * scale * s, BOT_INK)
            cv.ellipse(cx + r * 0.3, ey - 1.5 * scale * s,
                       4 * scale * s, 3.5 * scale * s, BOT_INK)
    # 口 (バイザー内)
    if face == "smile" or face == "wink":
        cv.ellipse(cx, cy + r * 0.02, 6 * scale * s, 4 * scale * s, BOT_EYE)
        cv.ellipse(cx, cy - r * 0.02, 6 * scale * s, 4 * scale * s, BOT_INK)
    else:
        cv_line(cv, cx - 3 * scale * s, cy + r * 0.05, cx + 3 * scale * s,
                cy + r * 0.05, 2 * scale * s, BOT_EYE)
    # 下部のパネル線 + 胸のランプ
    cv_line(cv, cx - r * 0.5, cy + r * 0.5, cx + r * 0.5, cy + r * 0.5,
            scale * s, (140, 190, 230, 200))
    cv.circle(cx, cy + r * 0.62, 3 * scale * s, SUN)


def gen_cosmos_mascot(face):
    """96x96 透過のマスコット表情差分。"""
    cv = Canvas(96, 96)
    s = cv.ss
    draw_bot(cv, 48 * s, 54 * s, 1.0, face)
    return cv.downsampled()


def gen_cosmos_face_chara():
    """240x410 透過: 大きめの星ロボ + 軌道リング + キラ星。"""
    cv = Canvas(240, 410)
    s = cv.ss
    cv_ellipse_ring(cv, 120 * s, 240 * s, 100 * s, 34 * s, s,
                    (85, 200, 255, 90))
    draw_bot(cv, 120 * s, 215 * s, 2.6, "smile")
    cv_sparkle(cv, 40 * s, 90 * s, 8 * s, (190, 230, 255, 220))
    cv_sparkle(cv, 200 * s, 140 * s, 6 * s, (190, 230, 255, 180))
    return cv.downsampled()


def gen_cosmos_timer_done():
    """200x140 透過: 手を振る星ロボ + キラ星。"""
    cv = Canvas(200, 140)
    s = cv.ss
    draw_bot(cv, 100 * s, 80 * s, 1.35, "smile")
    cv_sparkle(cv, 30 * s, 30 * s, 7 * s, (190, 230, 255, 220))
    cv_sparkle(cv, 170 * s, 45 * s, 6 * s, (255, 210, 140, 220))
    return cv.downsampled()


# ---- icons (36x36): 中身は白/シアンの記号。各アプリ id 用 ----

def _icon_bg(cv, color):
    """アイコンの下地 (丸角矩形相当を円で近似: 角丸の見た目)。"""
    s = cv.ss
    w = cv.W
    for y in range(w):
        for x in range(w):
            # 中心からの矩形距離で角丸 11px を再現
            rr = 11 * s
            dx = max(abs(x - w / 2 + 0.5) - (w / 2 - rr), 0)
            dy = max(abs(y - w / 2 + 0.5) - (w / 2 - rr), 0)
            if dx * dx + dy * dy <= rr * rr:
                cv.blend(x, y, color)


def gen_icon(app_id):
    """36x36 アイコンを生成。白グリフ + 薄シアン縁。"""
    cv = Canvas(36, 36, ss=4)
    s = cv.ss
    W = 18 * s  # 中心
    INK = (240, 250, 255, 255)
    CY = (120, 220, 255, 255)
    th = 2.4 * s
    g = {
        "timer": lambda: [
            cv.polygon([(x * s, y * s) for x, y in
                        [(10, 8), (26, 8), (18, 18)]], INK),
            cv.polygon([(x * s, y * s) for x, y in
                        [(10, 28), (26, 28), (18, 18)]], INK),
        ],
        "stopwatch": lambda: [
            cv.ring(W, 20 * s, 10 * s, th, INK),
            cv.rect(14 * s, 5 * s, 22 * s, 9 * s, INK),
            cv_line(cv, W, 20 * s, 24 * s, 15 * s, th, INK),
        ],
        "counter": lambda: [
            cv.rect(6 * s, 14 * s, 16 * s, 17 * s, INK),
            cv.rect(10 * s, 11 * s, 13 * s, 20 * s, INK),
            cv.rect(20 * s, 20 * s, 30 * s, 23 * s, INK),
        ],
        "memo": lambda: [
            cv.rect(8 * s, 6 * s, 28 * s, 30 * s,
                    (240, 250, 255, 60)),
            cv.rect(8 * s, 6 * s, 28 * s, 30 * s, (0, 0, 0, 0)),
        ] + [
            cv_line(cv, 12 * s, y * s, 24 * s, y * s, 1.6 * s, INK)
            for y in (12, 17, 22, 27)
        ],
        "alarm": lambda: [
            cv.ellipse(W, 20 * s, 9 * s, 10 * s, INK),
            cv.rect(9 * s, 24 * s, 27 * s, 27 * s, INK),
            cv.circle(W, 25 * s, 2 * s, INK),
            cv_line(cv, 6 * s, 8 * s, 11 * s, 11 * s, th, INK),
            cv_line(cv, 30 * s, 8 * s, 25 * s, 11 * s, th, INK),
        ],
        "notifications": lambda: [
            cv.rect(5 * s, 10 * s, 31 * s, 26 * s, (240, 250, 255, 50)),
            cv.polygon([(x * s, y * s) for x, y in
                        [(5, 10), (31, 10), (18, 19)]], INK),
            cv.polygon([(x * s, y * s) for x, y in
                        [(5, 11), (18, 20), (5, 20)]], INK),
            cv.polygon([(x * s, y * s) for x, y in
                        [(31, 11), (18, 20), (31, 20)]], INK),
        ],
        "media": lambda: [
            cv.polygon([(x * s, y * s) for x, y in
                        [(13, 8), (28, 18), (13, 28)]], INK),
        ],
        "steps": lambda: [
            cv.ellipse(14 * s, 14 * s, 5 * s, 9 * s, INK),
            cv.ellipse(24 * s, 24 * s, 4.5 * s, 7 * s, INK),
            cv.circle(11 * s, 5.5 * s, 1.5 * s, INK),
            cv.circle(15 * s, 4.5 * s, 1.3 * s, INK),
            cv.circle(21 * s, 14 * s, 1.5 * s, INK),
            cv.circle(25 * s, 15 * s, 1.3 * s, INK),
        ],
        "agent": lambda: [
            cv.polygon([(x * s, y * s) for x, y in
                        [(18, 4), (21, 15), (32, 18), (21, 21),
                         (18, 32), (15, 21), (4, 18), (15, 15)]], INK),
            cv.circle(28 * s, 8 * s, 2 * s, CY),
        ],
        "settings": lambda: [
            cv.polygon([(x * s, y * s) for x, y in
                        [(18, 6), (21.4, 7.4), (25.4, 8), (27.4, 10.9),
                         (30, 13.4), (30.8, 16.8), (29.6, 20.2),
                         (27.6, 23), (27.6, 26.6), (24.6, 29.6),
                         (20.8, 30.8), (17.4, 29.6), (14, 30.8),
                         (10.4, 29.6), (7.4, 26.6), (7.4, 23),
                         (5.4, 20.2), (4.2, 16.8), (5, 13.4),
                         (7.6, 10.9), (9.6, 8), (13.6, 7.4)]], INK),
            cv.circle(W, 18 * s, 3.5 * s, (10, 22, 40, 255)),
        ],
    }[app_id]()
    return cv.downsampled()


def build_cosmos():
    """cosmos テーマ: v2 の全キー (screens/style/icons/fonts/mascot/
    face_layout) を行使する。"""
    files = {
        # 画面背景 (PNG: ロード時に RGB565A8 デコード)
        "bg": (410, 502, gen_cosmos_bg()),
        "timer_bg": (410, 502, gen_cosmos_timer_bg()),
        "alert_bg": (410, 502, gen_cosmos_alert_bg()),
        # 文字盤/アラート画像
        "face_chara": (240, 410, gen_cosmos_face_chara()),
        "timer_done": (200, 140, gen_cosmos_timer_done()),
        # マスコット表情
        "m_n": (96, 96, gen_cosmos_mascot("normal")),
        "m_s": (96, 96, gen_cosmos_mascot("smile")),
        "m_w": (96, 96, gen_cosmos_mascot("wink")),
        # アプリアイコン (id → ic_<id>.png)
        **{f"ic_{a}": (36, 36, gen_icon(a)) for a in (
            "timer", "stopwatch", "counter", "memo", "alarm",
            "notifications", "media", "steps", "agent", "settings")},
    }
    # フォント (.bin は tools/themes/cosmos/ にコミット済みの
    # lv_font_conv 生成物を同梱)。
    for fn in ("digits.bin", "digits_sm.bin"):
        p = os.path.join(REPO, "tools", "themes", "cosmos", fn)
        files[fn[:-4]] = (0, 0, open(p, "rb").read())

    # エントリ名 (.png は PNG として書き出す)
    png_names = {n: n + ".png" for n in files if not n.endswith(".bin")
                 and n not in ("digits", "digits_sm")}
    png_names.update({"digits": "digits.bin", "digits_sm": "digits_sm.bin"})
    png_blobs = {}
    for n, (w, h, data) in files.items():
        if not png_names[n].endswith(".png"):
            continue
        # PNG 化 (write_png の中身をバイト列として得る版)
        png_blobs[png_names[n]] = (w, h, png_bytes(data, w, h))
    entries = dict(png_blobs)
    for n in ("digits", "digits_sm"):
        entries[n + ".bin"] = (0, 0, files[n][2])

    # ---- manifest.cbor ----
    def t(x):
        return cbor_text(x)
    def u(x):
        return cbor_uint(x)

    def color_map(pairs):
        return cbor_map([(k, t(v) if isinstance(v, str) else u(v))
                         for k, v in pairs])

    style = cbor_map([
        ("card_radius", u(12)), ("card_opa", u(150)),
        ("border_w", u(1)), ("border", t("0x3A5F8A")),
        ("glow_color", t("0x2A6FC0")), ("glow_w", u(14)),
        ("btn_radius", u(8)), ("header", t("flat")),
    ])
    screens = cbor_map([
        ("*", cbor_map([("bg", t("bg.png")), ("scrim", u(110))])),
        ("timer", cbor_map([("bg", t("timer_bg.png")),
                            ("scrim", u(60))])),
        ("alert", cbor_map([("bg", t("alert_bg.png")),
                            ("scrim", u(70))])),
        ("settings", cbor_map([("scrim", u(150))])),
        ("more", cbor_map([("scrim", u(150))])),
        ("memo", cbor_map([("scrim", u(150))])),
    ])
    icons = cbor_map([(a, t(f"ic_{a}.png")) for a in (
        "timer", "stopwatch", "counter", "memo", "alarm",
        "notifications", "media", "steps", "agent", "settings")])
    fonts = cbor_map([
        ("digits", t("digits.bin")), ("digits_sm", t("digits_sm.bin")),
    ])
    # 負数座標対応の int エンコード (cbor_head(1, -1-v))。
    def ci(v):
        return cbor_head(0, v) if v >= 0 else cbor_head(1, -1 - v)
    mascot = cbor_map([
        ("x", ci(300)), ("y", ci(380)),
        ("expr", cbor_map([("normal", t("m_n.png")),
                            ("smile", t("m_s.png")),
                            ("wink", t("m_w.png"))])),
        ("lines", cbor_head(4, 4) + b"".join(cbor_text(x) for x in (
            "宇宙を見てるよ", "おつかれさま", "きらきら〜",
            "タップありがと"))),
        ("screens", cbor_head(4, 3) + b"".join(cbor_text(x) for x in (
            "more", "memo", "timer"))),
        # home は chara と重なるので外す。alert は固定配置ボタンがあり
        # 退避パディングの効かない画面なので外す。
    ])
    face_layout = cbor_map([
        ("time", cbor_map([("x", ci(30)), ("y", ci(96)),
                           ("font", u(96)), ("color", t("0x9AE8FF"))])),
        ("date", cbor_map([("x", ci(32)), ("y", ci(200)),
                           ("font", u(20)), ("color", t("0x7E95B8"))])),
        ("steps", cbor_map([("x", ci(32)), ("y", ci(228)),
                            ("font", u(20)), ("color", t("0x7E95B8"))])),
        ("battery", cbor_map([("x", ci(-30)), ("y", ci(96)),
                              ("font", u(20)), ("color", t("0x7E95B8"))])),
        ("notify", cbor_map([("x", ci(-30)), ("y", ci(124)),
                             ("font", u(20)), ("color", t("0xFFB45C"))])),
        ("bubble", cbor_map([("x", ci(30)), ("y", ci(32)),
                             ("font", u(20)), ("color", t("0xCFEAFF"))])),
        ("chara", cbor_map([("x", ci(176)), ("y", ci(120)),
                            ("img", t("face_chara.png"))])),
    ])
    images = cbor_map([
        ("timer_done", t("timer_done.png")),
        # chara_side/chara_bubble など既存文字盤でも使えるようスロットにも入れる
        ("face_chara", t("face_chara.png")),
    ])
    bubble = cbor_map([
        ("morning", t("おはよう、宇宙の時間だよ")),
        ("noon", t("こんにちは")),
        ("evening", t("おつかれさま")),
        ("night", t("おやすみなさい")),
        ("steps", t("あと{n}歩")),
    ])
    manifest = cbor_map([
        ("id", t("cosmos")), ("api", u(1)), ("name", t("cosmos")),
        ("version", u(1)),
        ("tokens", cbor_map([(k, t(v) if isinstance(v, str) else u(v))
                             for k, v in COS_TOKENS.items()])),
        ("images", images),
        ("bubble", bubble),
        ("screens", screens),
        ("style", style),
        ("icons", icons),
        ("fonts", fonts),
        ("mascot", mascot),
        ("face_layout", face_layout),
    ])

    # ---- 出力 ----
    sim_dir = os.path.join(REPO, "sim", "themes", "cosmos")
    os.makedirs(sim_dir, exist_ok=True)
    with open(os.path.join(sim_dir, "manifest.cbor"), "wb") as f:
        f.write(manifest)
    total = len(manifest)
    for ename, (w, h, blob) in entries.items():
        total += len(blob)
        with open(os.path.join(sim_dir, ename), "wb") as f:
            f.write(blob)
        print(f"  {ename}  {len(blob)} B")

    for out_zip in (
        os.path.join(REPO, "android", "app", "src", "main", "assets",
                     "themes", "cosmos.zip"),
        os.path.join(REPO, "tools", "themes", "cosmos.zip"),
        os.path.join(REPO, "sim", "themes", "cosmos.zip"),  # zip 直読み検証
    ):
        os.makedirs(os.path.dirname(out_zip), exist_ok=True)
        with zipfile.ZipFile(out_zip, "w", zipfile.ZIP_STORED) as z:
            z.writestr("manifest.cbor", manifest)
            for ename, (w, h, blob) in sorted(entries.items()):
                z.writestr(ename, blob)
        print(f"  {os.path.relpath(out_zip, REPO)}  "
              f"{os.path.getsize(out_zip)} B (stored)")

    # プレビュー: 背景3枚 + chara + mascot + アイコン列
    pw = 410 * 3 + 240 + 96 * 3 + 36 * 10 + 60
    prev = Canvas(pw, 502, ss=1)
    prev.rect(0, 0, pw, 502, (6, 10, 20, 255))
    x = 0
    for n in ("bg", "timer_bg", "alert_bg", "face_chara",
              "m_n", "m_s", "m_w"):
        rgba = files[n][2]
        w, h = files[n][0], files[n][1]
        for y in range(h):
            for xx in range(w):
                i = (y * w + xx) * 4
                a = rgba[i + 3]
                if a:
                    prev.blend(x + xx, y,
                               (rgba[i], rgba[i + 1], rgba[i + 2], a))
        x += w + 10
    xi = 0
    for a in ("timer", "stopwatch", "counter", "memo", "alarm",
              "notifications", "media", "steps", "agent", "settings"):
        rgba = files[f"ic_{a}"][2]
        for y in range(36):
            for xx in range(36):
                i = (y * 36 + xx) * 4
                al = rgba[i + 3]
                if al:
                    prev.blend(x + xi * 46 + xx, 240 + y,
                               (rgba[i], rgba[i + 1], rgba[i + 2], al))
        xi += 1
    write_png(os.path.join(REPO, "tools", "themes",
                           "cosmos_preview.png"), prev.px, pw, 502)
    print(f"  tools/themes/cosmos_preview.png")
    print(f"  package total {total} B <= 4MiB: {total <= 4*1024*1024}")


def png_bytes(rgba, w, h):
    """RGBA バイト列 → PNG バイト列 (ファイルを介さない write_png 版)。"""
    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(
            ">I", zlib.crc32(c) & 0xFFFFFFFF)

    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw += rgba[y * w * 4:(y + 1) * w * 4]
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
            + chunk(b"IEND", b""))


# ================================================================ kit モード
# `--kit` : themes/<id>/kit.html を headless Chromium (Playwright) で開き、
# 各 [data-part] 要素を透過 PNG スクショして skin パーツ画像と
# manifest の `skin` ブロックを自動生成する。
#   data-part="button_primary"  data-state="pressed" (省略=normal)
#   data-slice="l,t,r,b"  data-pad="l,t,r,b"
#   data-text="0xRRGGBB"  data-text-pressed="0xRRGGBB"
# [data-asset="name.png"] はスキンではなく生の PNG アセットとして
# そのまま収録する (画面背景など)。slice 値はスクショ PNG 上の px。

KIT_STATE_NAMES = ("pressed", "checked", "disabled")  # manifest の順


def kit_capture(id_):
    """kit.html をスクショして (parts_meta, {entry名: png bytes}) を返す。"""
    try:
        from playwright.sync_api import sync_playwright
    except ImportError:
        sys.exit("playwright が要ります。まず:\n"
                 "  pip install playwright\n"
                 "  playwright install chromium")
    kit_html = os.path.join(REPO, "themes", id_, "kit.html")
    if not os.path.isfile(kit_html):
        sys.exit(f"{kit_html} がありません")
    parts = {}
    blobs = {}
    with sync_playwright() as pw:
        browser = pw.chromium.launch()
        page = browser.new_page(viewport={"width": 460, "height": 900},
                                device_scale_factor=1)
        page.goto("file://" + kit_html)
        els = page.locator("[data-part]")
        for i in range(els.count()):
            el = els.nth(i)
            part = el.get_attribute("data-part")
            state = el.get_attribute("data-state") or "normal"
            if state not in ("normal",) + KIT_STATE_NAMES:
                sys.exit(f"未知の state: {state} (part={part})")
            ename = part + ("" if state == "normal"
                            else "_" + state) + ".png"
            blobs[ename] = el.screenshot(omit_background=True)
            meta = parts.setdefault(part, {"states": {}})
            if state == "normal":
                meta["img"] = ename
                for attr, key in (("data-slice", "slice"),
                                  ("data-pad", "pad")):
                    v = el.get_attribute(attr)
                    if v:
                        meta[key] = [int(x) for x in v.split(",")]
                for attr, key in (("data-text", "text"),
                                  ("data-text-pressed", "text_pressed")):
                    v = el.get_attribute(attr)
                    if v:
                        meta[key] = v
            else:
                meta["states"][state] = ename
        assets = page.locator("[data-asset]")
        for i in range(assets.count()):
            el = assets.nth(i)
            blobs[el.get_attribute("data-asset")] = \
                el.screenshot(omit_background=True)
        browser.close()
    return parts, blobs


def _cbor_quad(vals):
    return cbor_head(4, 4) + b"".join(cbor_uint(v) for v in vals)


def skin_cbor(parts):
    """parts_meta → manifest の `skin` 値 (cbor map)。"""
    pairs = []
    for part in sorted(parts):
        m = parts[part]
        e = [("img", cbor_text(m["img"]))]
        if "slice" in m:
            e.append(("slice", _cbor_quad(m["slice"])))
        if "pad" in m:
            e.append(("pad", _cbor_quad(m["pad"])))
        if "text" in m:
            e.append(("text", cbor_text(m["text"])))
        if "text_pressed" in m:
            e.append(("text_pressed", cbor_text(m["text_pressed"])))
        if m["states"]:
            e.append(("states", cbor_map(
                [(s, cbor_text(m["states"][s])) for s in KIT_STATE_NAMES
                 if s in m["states"]])))
        pairs.append((part, cbor_map(e)))
    return cbor_map(pairs)


def emit_kit_theme(id_, manifest, blobs):
    """manifest.cbor + PNG 群を sim/themes/<id>/ 展開・3 箇所の zip に書く。"""
    sim_dir = os.path.join(REPO, "sim", "themes", id_)
    os.makedirs(sim_dir, exist_ok=True)
    with open(os.path.join(sim_dir, "manifest.cbor"), "wb") as f:
        f.write(manifest)
    total = len(manifest)
    for ename, blob in sorted(blobs.items()):
        with open(os.path.join(sim_dir, ename), "wb") as f:
            f.write(blob)
        total += len(blob)
        print(f"  {ename}  {len(blob)} B")

    for out_zip in (
        os.path.join(REPO, "sim", "themes", id_ + ".zip"),
        os.path.join(REPO, "android", "app", "src", "main", "assets",
                     "themes", id_ + ".zip"),
        os.path.join(REPO, "tools", "themes", id_ + ".zip"),
    ):
        os.makedirs(os.path.dirname(out_zip), exist_ok=True)
        with zipfile.ZipFile(out_zip, "w", zipfile.ZIP_STORED) as z:
            z.writestr("manifest.cbor", manifest)
            for ename, blob in sorted(blobs.items()):
                z.writestr(ename, blob)
        print(f"  {os.path.relpath(out_zip, REPO)}  "
              f"{os.path.getsize(out_zip)} B (stored)")
    print(f"  package total {total} B <= 4MiB: {total <= 4*1024*1024}")


# kit テーマの tokens (kit.html の CSS と同じ色を manifest 側にも定義)。
CYBER_TOKENS = {
    "bg": "0x04070E", "surface": "0x0A1428", "surface2": "0x122038",
    "line": "0x1E4B66", "text": "0xD8F4FF", "text_dim": "0x6E93AD",
    "primary": "0x3EE6FF", "primary2": "0x9AF0FF",
    "on_primary": "0x02101F", "danger": "0xFF4D6E", "ok": "0x4BE3A8",
    "accent": "0x3EE6FF", "accent2": "0x8FA8FF", "accent3": "0x4BE3A8",
    "accent4": "0xFF8FB8", "accent5": "0xFFB45C",
    "bubble_bg": "0x0B1B30", "bubble_text": "0xD8F4FF",
    "radius_sm": 8, "radius_lg": 12, "space": 8, "anim_ms": 160,
}

CUTE_TOKENS = {
    "bg": "0xFFF3F7", "surface": "0xFFFDFE", "surface2": "0xFFF0F5",
    "line": "0xF0D8E4", "text": "0x6B5570", "text_dim": "0xA48FA8",
    "primary": "0xFF8FB4", "primary2": "0xFFB9D2",
    "on_primary": "0xFFFFFF", "danger": "0xE0556E", "ok": "0x4EC89B",
    "accent": "0xFF9BBE", "accent2": "0xB79CFF", "accent3": "0x7FDFC8",
    "accent4": "0xFFC96E", "accent5": "0x8FD4F0",
    "bubble_bg": "0xFFFDFE", "bubble_text": "0x6B5570",
    "radius_sm": 12, "radius_lg": 24, "space": 8, "anim_ms": 160,
}

KIT_THEMES = {
    "cyber": {"name": "サイバー", "tokens": CYBER_TOKENS, "scrim": 30},
    "cute": {"name": "キュート", "tokens": CUTE_TOKENS, "scrim": 20},
}


def build_kit_theme(id_):
    spec = KIT_THEMES[id_]
    print(f"== {id_} ==")
    parts, blobs = kit_capture(id_)
    fields = [
        ("id", cbor_text(id_)), ("api", cbor_uint(1)),
        ("name", cbor_text(spec["name"])), ("version", cbor_uint(1)),
        ("tokens", cbor_map([(k, cbor_text(v) if isinstance(v, str)
                              else cbor_uint(v))
                             for k, v in spec["tokens"].items()])),
        ("screens", cbor_map([
            ("*", cbor_map([("bg", cbor_text("bg.png")),
                            ("scrim", cbor_uint(spec["scrim"]))])),
        ])),
        ("skin", skin_cbor(parts)),
    ]
    emit_kit_theme(id_, cbor_map(fields), blobs)


if __name__ == "__main__":
    args = sys.argv[1:]
    if args and args[0] == "--kit":
        for i in (args[1:] or list(KIT_THEMES)):
            build_kit_theme(i)
    else:
        build_mame()
        build_cosmos()
