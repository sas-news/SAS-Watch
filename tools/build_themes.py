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
    "radius_sm": 12, "radius_lg": 20, "space": 8, "anim_ms": 200,
    "font_body": 20, "font_title": 26, "font_digits": 96,
    "font_digits_sm": 56,
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


# ---------------------------------------------------------------- 出力

def build():
    imgs = {
        "home_bg": gen_home_bg(),       # 410x240
        "stand": gen_stand(),           # 160x200
        "timer_done": gen_timer_done(), # 320x240
    }
    dims = {"home_bg": (410, 240), "stand": (160, 200),
            "timer_done": (320, 240)}

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

    # プレビュー PNG (3 画像を横に並べた 900x260)
    pw = sum(d[0] for d in dims.values()) + 40
    ph = 240
    prev = Canvas(pw, ph, ss=1)
    prev.rect(0, 0, pw, ph, (12, 13, 17, 255))
    x = 0
    for slot in ("home_bg", "stand", "timer_done"):
        rgba, (w, h) = imgs[slot], dims[slot]
        for y in range(h):
            for xx in range(w):
                i = (y * w + xx) * 4
                a = rgba[i + 3]
                if a:
                    prev.blend(x + xx, y,
                               (rgba[i], rgba[i + 1], rgba[i + 2], a))
        x += w + 20
    write_png(os.path.join(REPO, "tools", "themes", "mame_preview.png"),
              prev.px, pw, ph)
    print(f"  tools/themes/mame_preview.png")
    print(f"  pixels total {total} B <= 2.5MiB: {total <= 2621440}")


if __name__ == "__main__":
    build()
