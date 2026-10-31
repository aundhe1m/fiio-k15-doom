#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Render a DOOM boot animation for the K15 (/usr/project/config/img/start_up/<n>.jpg).

  bootanim.py list
  bootanim.py render <name> <outdir> [--stock <stock start_up dir>] [--quality 85]
  bootanim.py sheet <framesdir> <out.png> [--stock <dir>] [--every 6]

Animations: melt (default: FiiO's own intro, then DOOM's screen-melt wipe to the DOOM title
card; needs --stock), hellfire, demons, terminal.

How v159 mq_ui plays them (static RE, 0x438c40): an LVGL animation runs the value 0..96
linearly over 3000 ms, its exec callback (0x437cb0) loads "<n>.jpg" only for n < 96, and the
ready callback anim_end_cb (0x4389bc) deletes the image and shows the main UI at once. So
frames 0..95 play at ~32 fps (time-based: a slow decode drops frames, never stretches), and
96.jpg is never shown; it is written as a copy of 95 so the file set stays the stock one.
Frames are 1024x240 baseline JPEG, YCbCr 4:2:0.

`render` writes only the frames that differ from stock (melt keeps stock 0..35), so the
rootfs content diff lists exactly the changed files. Output is deterministic (fixed seeds).
Only the public-domain DOOM wordmark (assets/doom-logo.svg) and the public-domain 8x8 font
(engine/src/font8x8_basic.h) are used; everything else is drawn here.
"""
import argparse
import math
import os
import re
import shutil
import sys

import numpy as np
from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from make_icon import load_subpaths  # noqa: E402

W, H = 1024, 240
SHOWN = 96   # frames 0..95 are displayed
FILES = 97   # stock ships 0..96
NAMES = ("melt", "hellfire", "demons", "terminal")
LOGO_TOP, LOGO_BOT = (255, 212, 72), (176, 20, 6)
CAPTION = "FIIO K15 DOOM EDITION"
FONT8 = os.path.join(HERE, "..", "engine", "src", "font8x8_basic.h")
TTF_CANDIDATES = (
    "/usr/share/fonts/truetype/ubuntu/UbuntuSans[wdth,wght].ttf",
    "/usr/share/fonts/truetype/ubuntu/Ubuntu[wdth,wght].ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-ExtraLight.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
)


# ------------------------------------------------------------------ sprites
class Sprite:
    """Straight-alpha RGBA as float32 arrays: rgb (h,w,3) 0..255, a (h,w) 0..1."""

    def __init__(self, rgb, a):
        self.rgb = rgb.astype(np.float32)
        self.a = a.astype(np.float32)
        self.h, self.w = self.a.shape

    @classmethod
    def from_pil(cls, img):
        arr = np.asarray(img.convert("RGBA"), np.float32)
        return cls(arr[..., :3], arr[..., 3] / 255)

    def solid(self, colour, alpha_scale=1.0):
        rgb = np.empty((self.h, self.w, 3), np.float32)
        rgb[:] = colour
        return Sprite(rgb, self.a * alpha_scale)

    def glow(self, radius, colour, strength=1.0):
        """Blurred, padded copy of the alpha in one colour; returns (sprite, pad)."""
        pad = int(radius * 3)
        a = Image.fromarray((self.a * 255).astype(np.uint8), "L")
        big = Image.new("L", (self.w + 2 * pad, self.h + 2 * pad), 0)
        big.paste(a, (pad, pad))
        g = np.asarray(big.filter(ImageFilter.GaussianBlur(radius)), np.float32) / 255
        rgb = np.empty(g.shape + (3,), np.float32)
        rgb[:] = colour
        return Sprite(rgb, np.clip(g * strength, 0, 1)), pad


def blit(dst, spr, x, y, alpha=1.0, add=False, crop_w=None):
    """Composite spr onto dst (H,W,3 float) at integer x,y with clipping."""
    x, y = int(round(x)), int(round(y))
    sw = spr.w if crop_w is None else min(spr.w, crop_w)
    x0, y0 = max(x, 0), max(y, 0)
    x1, y1 = min(x + sw, dst.shape[1]), min(y + spr.h, dst.shape[0])
    if x0 >= x1 or y0 >= y1 or alpha <= 0:
        return
    a = spr.a[y0 - y:y1 - y, x0 - x:x1 - x, None] * alpha
    rgb = spr.rgb[y0 - y:y1 - y, x0 - x:x1 - x]
    region = dst[y0:y1, x0:x1]
    if add:
        region += rgb * a
    else:
        region *= 1 - a
        region += rgb * a


def vgrad(w, h, top, bot):
    t = np.linspace(0, 1, h, dtype=np.float32)[:, None, None]
    col = np.array(top, np.float32) * (1 - t) + np.array(bot, np.float32) * t
    return np.repeat(col, w, axis=1)


_LOGO = {}


def logo(h):
    """The DOOM wordmark h px tall with the menu's gold->red gradient (cached)."""
    if h not in _LOGO:
        subs, lw, lh = load_subpaths()
        s, ss = h / lh, 6
        w = int(round(lw * s)) + 2
        mask = Image.new("1", (w * ss, (h + 2) * ss), 0)
        for sub in subs:
            m = Image.new("1", mask.size, 0)
            ImageDraw.Draw(m).polygon([((px * s + 1) * ss, (py * s + 1) * ss) for px, py in sub],
                                      fill=1)
            mask = ImageChops.logical_xor(mask, m)  # even-odd: letter counters are holes
        a = np.asarray(mask.convert("L").resize((w, h + 2), Image.BOX), np.float32) / 255
        _LOGO[h] = Sprite(vgrad(w, h + 2, LOGO_TOP, LOGO_BOT), a)
    return _LOGO[h]


class LogoFx:
    """Logo + drop shadow + coloured glow, drawn together."""

    def __init__(self, h, glow_radius=14, glow_colour=(255, 70, 10), shadow=(5, 6, 0.85)):
        self.spr = logo(h)
        self.glow, self.pad = self.spr.glow(glow_radius, glow_colour)
        self.shadow = shadow
        self.shadow_spr = self.spr.solid((0, 0, 0))

    def draw(self, dst, x, y, alpha=1.0, glow=1.0):
        blit(dst, self.glow, x - self.pad, y - self.pad, min(1.0, alpha * glow), add=True)
        if glow > 1:
            blit(dst, self.glow, x - self.pad, y - self.pad, alpha * (glow - 1), add=True)
        if self.shadow:
            dx, dy, sa = self.shadow
            blit(dst, self.shadow_spr, x + dx, y + dy, alpha * sa)
        blit(dst, self.spr, x, y, alpha)


_GLYPHS = None


def glyphs():
    global _GLYPHS
    if _GLYPHS is None:
        src = open(FONT8, encoding="utf-8").read()
        rows = re.findall(r"\{\s*((?:0x[0-9A-Fa-f]{2}\s*,?\s*){8})\}", src)[:128]
        _GLYPHS = [[int(v, 16) for v in re.findall(r"0x[0-9A-Fa-f]{2}", r)] for r in rows]
    return _GLYPHS


def text8(s, scale, colour, spacing=1):
    """DOOM-menu style 8x8 bitmap text; each glyph cell is (8+spacing)*scale wide."""
    g = glyphs()
    cw = 8 + spacing
    a = np.zeros((8, max(1, len(s) * cw)), np.float32)
    for i, ch in enumerate(s):
        rows = g[ord(ch) & 127]
        for r in range(8):
            for c in range(8):
                if rows[r] >> c & 1:  # LSB is the leftmost pixel
                    a[r, i * cw + c] = 1
    a = np.repeat(np.repeat(a, scale, 0), scale, 1)
    rgb = np.empty(a.shape + (3,), np.float32)
    rgb[:] = colour
    return Sprite(rgb, a)


def ttf_font(size, weight):
    for path in TTF_CANDIDATES:
        if os.path.exists(path):
            f = ImageFont.truetype(path, size)
            try:
                f.set_variation_by_axes([100, weight])
            except Exception:
                pass
            return f
    return ImageFont.load_default()


def text_ttf(s, size, top, bot, tracking=0, weight=300):
    f = ttf_font(size, weight)
    widths = [f.getlength(c) for c in s]
    w = int(sum(widths) + tracking * (len(s) - 1)) + 8
    asc, desc = f.getmetrics()
    m = Image.new("L", (w, asc + desc), 0)
    d = ImageDraw.Draw(m)
    x = 4.0
    for c, cw in zip(s, widths):
        d.text((x, 0), c, font=f, fill=255)
        x += cw + tracking
    m = m.crop(m.getbbox())
    return Sprite(vgrad(m.width, m.height, top, bot), np.asarray(m, np.float32) / 255)


def smoothstep(x):
    x = np.clip(x, 0, 1)
    return x * x * (3 - 2 * x)


def ramp(f, a, b):
    """0 before frame a, 1 after frame b, linear in between."""
    return float(np.clip((f - a) / max(1e-6, b - a), 0, 1))


def noise(w, h, cells_w, cells_h, blur, seed):
    r = np.random.default_rng(seed).random((cells_h, cells_w))
    im = Image.fromarray((r * 255).astype(np.uint8), "L").resize((w, h), Image.BICUBIC)
    return np.asarray(im.filter(ImageFilter.GaussianBlur(blur)), np.float32) / 255


def splat(points_xy, values, blur=1.2, core=0.8, halo=2.0):
    """Additive point sprites: returns an (H,W) float intensity map (0..~255+)."""
    x = np.round(points_xy[0]).astype(int)
    y = np.round(points_xy[1]).astype(int)
    ok = (x >= 0) & (x < W) & (y >= 0) & (y < H)
    acc = np.zeros((H, W), np.float32)
    np.add.at(acc, (y[ok], x[ok]), values[ok])
    im = Image.fromarray(np.clip(acc, 0, 255).astype(np.uint8), "L")
    soft = np.asarray(im.filter(ImageFilter.GaussianBlur(blur)), np.float32)
    return acc * core + soft * halo


def doom_card(dst, f, lfx):
    """The DOOM title card: dark-red gradient, wordmark, caption (melt reveals this)."""
    dst[:] = vgrad(W, H, (48, 6, 0), (0, 0, 0))
    breathe = 0.9 * (1 + 0.1 * math.sin(f * 0.45))
    lfx.draw(dst, (W - lfx.spr.w) // 2, 20, 1.0, breathe)
    cap = text8(CAPTION, 2, (168, 148, 132))
    blit(dst, cap.solid((0, 0, 0)), (W - cap.w) // 2 + 2, 186)
    blit(dst, cap, (W - cap.w) // 2, 184)


# ------------------------------------------------------------------ melt
def anim_melt(stock):
    """Stock frames 0..35 unchanged, then DOOM's f_wipe.c melt of frozen stock frame 35
    onto the title card (~42 frames), then the card holds with drifting sparks."""
    if not stock:
        raise SystemExit("melt needs --stock <stock start_up dir>")
    start = 36
    frozen = np.asarray(Image.open(os.path.join(stock, f"{start - 1}.jpg")).convert("RGB"),
                        np.float32)
    lfx = LogoFx(132, glow_radius=14)
    rng = np.random.default_rng(1993)
    cols = 160  # DOOM melts 2-px columns of a 320-px screen
    colx = np.arange(W) * cols // W
    # wipe_initMelt(): y[0] = -(rand%16); y[i] = y[i-1] + rand(-1..1), clamped to [-15, 0]
    y = np.zeros(cols)
    y[0] = -rng.integers(0, 16)
    for i in range(1, cols):
        y[i] = np.clip(y[i - 1] + rng.integers(-1, 2), -15, 0)
    # sparks rising off the card
    n = 70
    sx, sy0 = rng.uniform(0, W, n), rng.uniform(0, H + 120, n)
    svy, sb = rng.uniform(1.0, 2.6, n), rng.uniform(0.3, 1.0, n)
    rows = np.arange(H)[:, None]
    for f in range(SHOWN):
        if f < start:
            yield f, None
            continue
        # wipe_doMelt(), one tic per frame, in DOOM's 200-line units
        live = y < 200
        dy = np.where(y < 0, 1, np.where(y < 16, y + 1, 8))
        y = np.where(live, np.minimum(y + dy, 200), y)
        shift = np.clip(np.round(np.maximum(y, 0) * H / 200), 0, H).astype(int)
        card = np.empty((H, W, 3), np.float32)
        doom_card(card, f, lfx)
        t = f - start
        py = (sy0 - svy * t * 2) % (H + 120) - 60
        glow = splat((sx, py), sb * 140 * (0.6 + 0.4 * np.sin(t * 0.7 + sx)), blur=1.5)
        card += glow[..., None] * np.array([1.0, 0.42, 0.1], np.float32) * 0.6
        src = rows - shift[colx][None, :]
        old = src >= 0
        srcc = np.clip(src, 0, H - 1)
        oldpix = np.take_along_axis(frozen, srcc[..., None].repeat(3, axis=2), axis=0)
        out = np.where(old[..., None], oldpix, card)
        yield f, out


# ------------------------------------------------------------------ hellfire
def fire_palette(n=37):
    stops = [(0, (7, 7, 7)), (0.15, (71, 15, 7)), (0.35, (159, 47, 7)), (0.55, (207, 95, 7)),
             (0.75, (223, 135, 23)), (0.9, (207, 191, 79)), (1, (255, 255, 255))]
    pal = np.zeros((n, 3), np.float32)
    for i in range(n):
        t = i / (n - 1)
        for (t0, c0), (t1, c1) in zip(stops, stops[1:]):
            if t <= t1:
                k = (t - t0) / (t1 - t0)
                pal[i] = [c0[j] + (c1[j] - c0[j]) * k for j in range(3)]
                break
    pal[0] = 0
    return pal


def anim_hellfire(stock):
    """PSX DOOM fire ignites from black; the logo rises out of it; the caption types on."""
    cell = 4
    cols, vis = W // cell, H // cell
    rows = vis + 3  # hidden source rows below the screen
    fire = np.zeros((rows, cols), int)
    pal = fire_palette()
    rng = np.random.default_rng(666)
    idx = np.arange(cols)
    lfx = LogoFx(132, glow_radius=16, glow_colour=(255, 90, 10))
    lx = (W - lfx.spr.w) // 2
    cap = text8(CAPTION, 2, (255, 255, 255))
    cap_sh = cap.solid((0, 0, 0))
    cell_w = 9 * 2
    cx = (W - cap.w) // 2
    plate = Sprite(np.zeros((cap.h + 14, cap.w + 40, 3)), np.full((cap.h + 14, cap.w + 40), 0.6))
    cursor = Sprite(np.full((16, 16, 3), 255.0), np.ones((16, 16)))
    for f in range(SHOWN):
        fire[-1] = round(36 * min(1.0, (f + 1) / 5))
        # spreadFire(), top-down so each step climbs exactly one row
        for _ in range(3 if f < 12 else 1):
            for yy in range(1, rows):
                rnd = rng.integers(0, 4, cols)
                dec = (rng.random(cols) < 0.8) * rnd
                fire[yy - 1, (idx - rnd + 1) % cols] = np.maximum(fire[yy] - dec, 0)
        rgb = pal[fire[:vis]] * 0.85
        out = np.repeat(np.repeat(rgb, cell, 0), cell, 1).astype(np.float32)
        heat = float(fire[vis - 12:vis].mean()) / 36
        p = ramp(f, 12, 36)
        ly = 10 + 80 * (1 - p) ** 3
        alpha = ramp(f, 12, 26)
        flick = 1.15 + 0.12 * math.sin(f * 0.9) + 0.08 * math.sin(f * 2.3) + 0.2 * (heat - 0.5)
        lfx.draw(out, lx, ly, alpha, flick)
        pa = ramp(f, 34, 40)
        blit(out, plate, (W - plate.w) // 2, 158, pa)
        n = int(np.clip(f - 38, 0, len(CAPTION)))
        if n:
            blit(out, cap_sh, cx + 2, 167, crop_w=n * cell_w)
            blit(out, cap, cx, 165, crop_w=n * cell_w)
        if 36 <= f < 80 and (f // 5) % 2 == 0:
            blit(out, cursor, cx + n * cell_w, 165)
        yield f, out


# ------------------------------------------------------------------ demons
def anim_demons(stock):
    """Homage to FiiO's own intro: red dust and embers drift, embers condense into the
    wordmark, then the thin tracked caption closes in."""
    rng = np.random.default_rng(666)
    span = W + 420
    smoke1 = noise(span, H, 52, 10, 18, 3)
    smoke2 = noise(span, H, 156, 30, 6, 4)
    band = np.exp(-((np.arange(H) - 120) / 85.0) ** 2)[:, None].astype(np.float32)
    xs = np.linspace(0, 5.5, W, dtype=np.float32)[None, :]
    ember = np.array([1.0, 0.45, 0.12], np.float32)
    # ambient embers
    n = 3200
    ax, ay = rng.uniform(0, W, n), rng.normal(120, 55, n)
    avx, avy = rng.uniform(0.8, 3.5, n), rng.normal(0, 0.3, n)
    ab, aph = rng.random(n) ** 3, rng.uniform(0, 6.3, n)
    # logo particles: targets sampled from the wordmark's coverage
    lh = 78
    lfx = LogoFx(lh, glow_radius=10, glow_colour=(255, 60, 0), shadow=None)
    lx, ly = (W - lfx.spr.w) // 2, 62
    inside = lfx.spr.a > 0.5
    edge = inside & ~(np.roll(inside, 1, 0) & np.roll(inside, -1, 0) &
                      np.roll(inside, 1, 1) & np.roll(inside, -1, 1))
    ey, ex = np.nonzero(edge)  # mostly outline points so the letters read while forming
    fy, fx = np.nonzero(inside)
    m = 4200
    ne = int(m * 0.7)
    pe, pf = rng.integers(0, len(ex), ne), rng.integers(0, len(fx), m - ne)
    tx = np.concatenate([ex[pe], fx[pf]]) + lx
    ty = np.concatenate([ey[pe], fy[pf]]) + ly
    px0, py0 = rng.uniform(-120, W + 120, m), rng.normal(120, 70, m)
    t0, dur = rng.uniform(6, 28, m), rng.uniform(12, 18, m)
    curl = rng.uniform(-30, 30, m)
    pb = 0.5 + 0.5 * rng.random(m)
    cap_cache = {}
    for f in range(SHOWN):
        bright = smoothstep(f / 14)
        o1, o2 = int(2 * f), int(3.5 * f)
        s1 = smoke1[:, span - W - o1:span - o1]
        s2 = smoke2[:, span - W - o2:span - o2]
        wave = 0.5 + 0.3 * np.sin(xs - f * 0.03)
        dens = np.clip((s1 * 0.6 + s2 * 0.4) * band * (0.6 + wave) - 0.15, 0, 1) ** 1.4
        out = dens[..., None] * np.array([175, 34, 10], np.float32)
        # ambient
        px = (ax + avx * f) % W
        py = ay + avy * f
        di = dens[np.clip(py.astype(int), 0, H - 1), px.astype(int) % W]
        vals = ab * 255 * (di + 0.15) * (0.7 + 0.3 * np.sin(aph + f * 0.4))
        # converging
        p = smoothstep((f - t0) / dur)
        lp = smoothstep((f - 30) / 14)
        swirl = (1 - p) ** 2 * np.sin(p * math.pi * 1.5)
        cx = px0 + (tx - px0) * p + curl * swirl
        cy = py0 + (ty - py0) * p + curl * 0.4 * swirl
        cv = pb * 255 * (0.25 + 0.75 * p) * (1 - 0.85 * lp)
        pts = splat((np.concatenate([px, cx]), np.concatenate([py, cy])),
                    np.concatenate([vals, cv]))
        out += pts[..., None] * ember
        out *= bright
        breathe = 0.9 * (1 + 0.08 * math.sin(f * 0.35))
        lfx.draw(out, lx, ly, lp, breathe)
        ca = smoothstep((f - 44) / 16)
        if ca > 0:
            track = int(round(4 + 12 * (1 - ca)))
            if track not in cap_cache:
                cap_cache[track] = text_ttf("FIIO K15  DOOM EDITION", 22, (235, 190, 120),
                                            (170, 90, 50), tracking=track)
            cap = cap_cache[track]
            blit(out, cap, (W - cap.w) // 2, 158, ca)
        yield f, out


# ------------------------------------------------------------------ terminal
TERM_HEAD = [("FIIO K15 // INGENIC X2000  MIPS32R2", (255, 212, 72)),
             ("DOOM EDITION  -  DOOMGENERIC/K15", (255, 212, 72)),
             ("", None)]
TERM_LOG = [("Z_Init: Init zone memory allocation", (200, 190, 180)),
            ("W_Init: Init WADfiles.", (200, 190, 180)),
            ("        adding doom1.wad", (160, 150, 140)),
            ("I_Init: Setting up machine state.", (200, 190, 180)),
            ("S_Init: Setting up sound.", (200, 190, 180))]
GREY, GREEN, RED = (200, 190, 180), (120, 230, 120), (255, 58, 36)


def terminal_script():
    """Per-frame list of (text, colour) lines for the log, printed like a real console."""
    snaps, cur = [], []

    def emit(lines, hold=1):
        snaps.extend([list(lines)] * hold)

    emit([], 6)  # CRT power-on
    for s, c in TERM_HEAD[:2]:
        for k in range(6, len(s) + 6, 6):
            emit(cur + [(s[:k], c)])
        cur.append((s, c))
    cur.append(TERM_HEAD[2])
    emit(cur)
    for s, c in TERM_LOG:
        cur.append((s, c))
        emit(cur, 3)
    base = "R_Init: Init DOOM refresh ["
    for k in range(8):
        emit(cur + [(base + "." * k + " " * (7 - k) + "]", GREY)])
    cur.append((base + ".......]", GREY))
    cur.append(("ST_Init: Init status bar.", GREY))
    emit(cur, 3)
    emit(cur + [("K15: knob, touch, usb-hid ...", GREEN)], 4)
    cur.append(("K15: knob, touch, usb-hid ... OK", GREEN))
    emit(cur, 1)
    return snaps, cur


def anim_terminal(stock):
    """CRT power-on, DOOM's startup log prints, the logo slams in, RIP AND TEAR."""
    snaps, final = terminal_script()
    slam = len(snaps) + 3  # first slam frame
    impact = slam + 4
    rng = np.random.default_rng(42)
    lh = 120
    cxp = 640 + (W - 640) // 2
    scales = [1.9, 1.6, 1.35, 1.15, 1.0]
    fx = {h: LogoFx(h, glow_radius=12) for h in {int(lh * s) for s in scales}}
    k15 = "K15 EDITION"
    rip = "> RIP AND TEAR"
    lines_y0, lh_px = 14, 18
    cur_spr = Sprite(np.full((14, 14, 3), 255.0), np.ones((14, 14)))
    bg = np.array([4, 2, 2], np.float32)
    for f in range(SHOWN):
        lines = snaps[min(f, len(snaps) - 1)] if f < len(snaps) else list(final)
        if f >= impact + 8:
            n = int(np.clip(f - (impact + 8), 0, len(rip)))
            lines = lines + [(rip[:n], RED)]
        txt = np.zeros((H, W, 3), np.float32)
        y = lines_y0
        endx, endy = 22, lines_y0
        for s, c in lines:
            if s:
                spr = text8(s, 2, c, spacing=0)
                blit(txt, spr, 22, y)
                endx, endy = 22 + spr.w + 2, y
            else:
                endx, endy = 22, y
            y += lh_px
        if f >= 3 and (f // 5) % 2 == 0:
            blit(txt, cur_spr.solid(RED if f >= impact else GREY), endx, endy + 1)
        glow = np.asarray(Image.fromarray(np.clip(txt, 0, 255).astype(np.uint8))
                          .filter(ImageFilter.GaussianBlur(2.5)), np.float32)
        out = np.empty((H, W, 3), np.float32)
        out[:] = bg
        out += txt + glow * 0.45
        out[1::3, :640] *= 0.55  # scanlines over the console
        # divider grows from the centre before the slam
        d = ramp(f, slam - 8, slam - 2)
        if d > 0:
            half = int(96 * d)
            out[120 - half:120 + half, 639:641] = (120, 20, 8)
        # logo slam
        if f >= slam:
            k = min(f - slam, len(scales) - 1)
            h = int(lh * scales[k])
            lf = fx[h]
            a = [0.35, 0.55, 0.75, 0.9, 1.0][k]
            lf.draw(out, cxp - lf.spr.w // 2, 40 + lh // 2 - h // 2, a, 0.8 + 0.6 * (f == impact))
        if f >= impact + 2:
            n = int(np.clip((f - impact - 2) * 2, 0, len(k15)))
            if n:
                cap = text8(k15, 2, (168, 148, 132))
                full = text8(k15, 2, (0, 0, 0))
                x = cxp - full.w // 2
                blit(out, cap.solid((0, 0, 0)), x + 2, 178, crop_w=n * 18)
                blit(out, cap, x, 176, crop_w=n * 18)
        if f >= impact:
            decay = 0.6 ** (f - impact)
            out = out * (1 - 0.55 * decay) + np.array([255, 40, 10], np.float32) * 0.55 * decay
            amp = int(round(7 * decay))
            if amp:
                dx, dy = rng.integers(-amp, amp + 1, 2)
                out = np.roll(out, (int(dy), int(dx)), axis=(0, 1))
        # CRT power-on: a bright line opens vertically
        if f < 6:
            open_h = [2, 2, 24, 90, 240, 240][f]
            flash = [1.0, 0.9, 0.7, 0.45, 0.25, 0.1][f]
            mask = np.zeros((H, 1, 1), np.float32)
            mask[H // 2 - open_h // 2:H // 2 + max(1, open_h // 2)] = 1
            line_w = W * (0.35 if f == 0 else 1.0)
            xm = (np.abs(np.arange(W) - W / 2) < line_w / 2).astype(np.float32)[None, :, None]
            out = out * mask + 255 * flash * mask * xm
        yield f, out


# ------------------------------------------------------------------ driver
ANIMS = {"melt": anim_melt, "hellfire": anim_hellfire, "demons": anim_demons,
         "terminal": anim_terminal}


MAX_BYTES = 32 * 1024  # stock frames are 5-20 KB; keep decode time near stock


def save_jpeg(arr, path, quality):
    """Baseline 4:2:0 JPEG; steps the quality down until the frame fits MAX_BYTES."""
    img = Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8), "RGB")
    q = quality
    while True:
        img.save(path, "JPEG", quality=q, subsampling=2, optimize=False, progressive=False)
        if os.path.getsize(path) <= MAX_BYTES or q <= 50:
            return q
        q -= 5


def check_stock(stock):
    missing = [i for i in range(FILES) if not os.path.isfile(os.path.join(stock, f"{i}.jpg"))]
    if missing:
        raise SystemExit(f"{stock}: missing stock frames {missing[:5]}…")


def render(name, outdir, stock=None, quality=85):
    if stock:
        check_stock(stock)
    os.makedirs(outdir, exist_ok=True)
    written, last, qs = [], None, []
    for f, arr in ANIMS[name](stock):
        if arr is None:
            continue
        path = os.path.join(outdir, f"{f}.jpg")
        qs.append(save_jpeg(arr, path, quality))
        written.append(f)
        last = path
    if last and SHOWN - 1 in written:
        shutil.copyfile(last, os.path.join(outdir, f"{FILES - 1}.jpg"))
        written.append(FILES - 1)
    size = sum(os.path.getsize(os.path.join(outdir, f"{i}.jpg")) for i in written)
    print(f"bootanim {name}: {len(written)} frames ({written[0]}..{written[-1]}), "
          f"{size // 1024} KiB, quality {min(qs)}..{max(qs)} -> {outdir}")
    return written


def sheet(framesdir, out, stock=None, every=6):
    tiles = []
    for i in range(0, SHOWN, every):
        p = os.path.join(framesdir, f"{i}.jpg")
        if not os.path.exists(p) and stock:
            p = os.path.join(stock, f"{i}.jpg")
        tiles.append((i, Image.open(p).convert("RGB").resize((W // 2, H // 2))))
    cols = 2
    rows = (len(tiles) + cols - 1) // cols
    img = Image.new("RGB", (cols * (W // 2 + 6), rows * (H // 2 + 20)), (40, 40, 44))
    d = ImageDraw.Draw(img)
    for k, (i, t) in enumerate(tiles):
        x, y = (k % cols) * (W // 2 + 6), (k // cols) * (H // 2 + 20)
        d.text((x + 4, y + 3), f"frame {i}  t={i / 32:.2f}s", fill=(220, 220, 220))
        img.paste(t, (x, y + 18))
    img.save(out)
    print(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list")
    r = sub.add_parser("render")
    r.add_argument("name", choices=NAMES)
    r.add_argument("outdir")
    r.add_argument("--stock")
    r.add_argument("--quality", type=int, default=85)
    s = sub.add_parser("sheet")
    s.add_argument("framesdir")
    s.add_argument("out")
    s.add_argument("--stock")
    s.add_argument("--every", type=int, default=6)
    a = ap.parse_args()
    if a.cmd == "list":
        for n in NAMES:
            print(n)
    elif a.cmd == "render":
        render(a.name, a.outdir, a.stock, a.quality)
    else:
        sheet(a.framesdir, a.out, a.stock, a.every)


if __name__ == "__main__":
    main()
