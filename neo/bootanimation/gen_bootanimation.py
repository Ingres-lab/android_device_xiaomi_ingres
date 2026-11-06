#!/usr/bin/env python3
#
# SPDX-FileCopyrightText: Neo
# SPDX-License-Identifier: Apache-2.0
#
# Neo OS boot animation for the POCO F4 GT (ingres).
#
# Inspired by the two "X-factor" RGB light strips on the camera island: two
# chevrons, > and <, pointing at the middle lens.
#
#   part0  intro (c 1 0)   sparks ignite at the tips, light races outward with
#                          comet tails, the island outline is traced and
#                          NEO OS settles in under it
#   part1  loop  (c 0 0)   HDR breathing, energy comets along the strips,
#                          drifting light motes; seamless, ends where it starts
#   part2  finale (c 1 0)  anticipation, snap-in with overshoot, flash,
#                          shockwave, chromatic aberration; NEO OS glides down
#                          while POCO F4 GT rises letter by letter, one light
#                          sweep, hold
#
# Rendering works like a fragment-shader pipeline, in numpy so it needs no GPU:
# signed-distance-field shapes with exponential light falloff into an HDR float
# buffer, a three-level bloom chain, ACES tone mapping, sRGB encoding and a
# fixed ordered dither against banding. Frames are cropped to the lit area and
# described in trim.txt, so the device decodes small PNGs (smooth playback).
#
# Usage: gen_bootanimation.py OUT.zip FONT.ttf [JOBS]
#   FONT: Roboto Condensed Bold Italic (Apache-2.0) for the shipped zip.
#   Needs only numpy and Pillow.

import io
import math
import multiprocessing
import sys
import zipfile

import numpy as np
from PIL import Image, ImageDraw, ImageFont

W, H, FPS = 1080, 2400, 60

# Geometry (portrait screen pixels)
CX, CY = 540, 1000
ISLAND = (372, 720, 708, 1280)
ISLAND_R = 74
STRIP_H = 214
STRIP_DX = 48
STRIP_GAP = 94
LENSES = ((CX, CY - 168, 64), (CX, CY, 34), (CX, CY + 168, 64))
NEO_Y = 1452            # NEO OS during intro/loop (baseline-ish top)
TITLE_Y = 1430          # POCO F4 GT in the finale
NEO_Y_FINAL = 1590      # NEO OS after it glides down

# Crop rectangles (x0, y0, x1, y1): the device draws the frame there.
CROP_A = (200, 520, 880, 1600)      # intro + loop
CROP_B = (40, 520, 1040, 1760)      # finale (title is wide)

# Linear-light colours (HDR scale)
C_CORE = np.array([0.80, 0.95, 1.00], np.float32)
C_GLOW = np.array([0.00, 0.55, 1.00], np.float32)
C_HALO = np.array([0.02, 0.18, 1.00], np.float32)
C_ISLAND = np.array([0.20, 0.26, 0.34], np.float32)
C_TEXT = np.array([0.95, 0.98, 1.00], np.float32)

FONT_PATH = None


# ---------------------------------------------------------------- math helpers

def clamp01(x):
    return max(0.0, min(1.0, x))


def ease_out_cubic(t):
    t = clamp01(t)
    return 1 - (1 - t) ** 3


def ease_in_out(t):
    t = clamp01(t)
    return t * t * (3 - 2 * t)


def ease_out_back(t, s=1.9):
    t = clamp01(t) - 1
    return t * t * ((s + 1) * t + s) + 1


def hash01(i):
    """Deterministic pseudo random in [0, 1)."""
    x = math.sin(i * 12.9898 + 78.233) * 43758.5453
    return x - math.floor(x)


# ---------------------------------------------------------------- SDF helpers

class Grid:
    def __init__(self, crop):
        self.x0, self.y0, self.x1, self.y1 = crop
        ys, xs = np.mgrid[self.y0:self.y1, self.x0:self.x1].astype(np.float32)
        self.x, self.y = xs + 0.5, ys + 0.5
        self.shape = self.x.shape


def seg_dist(g, a, b, t1=1.0):
    """Distance to segment a->b (only the part 0..t1) and the parameter t."""
    ax, ay = a
    bx, by = b
    dx, dy = bx - ax, by - ay
    L2 = dx * dx + dy * dy
    t = ((g.x - ax) * dx + (g.y - ay) * dy) / L2
    t = np.clip(t, 0.0, t1)
    px, py = ax + t * dx, ay + t * dy
    return np.hypot(g.x - px, g.y - py), t


def rrect_dist(g, rect, r):
    l, t, rr, b = rect
    cx, cy = (l + rr) / 2, (t + b) / 2
    hx, hy = (rr - l) / 2 - r, (b - t) / 2 - r
    qx = np.abs(g.x - cx) - hx
    qy = np.abs(g.y - cy) - hy
    outside = np.hypot(np.maximum(qx, 0), np.maximum(qy, 0))
    inside = np.minimum(np.maximum(qx, qy), 0)
    return np.abs(outside + inside - r)


def strip_segments(gap=STRIP_GAP):
    """Four segments, each from an apex outward to an end."""
    segs = []
    for s in (-1, 1):
        apex = (CX + s * gap, CY)
        for e in (-1, 1):
            end = (CX + s * (gap + STRIP_DX), CY + e * STRIP_H)
            segs.append((apex, end))
    return segs


def light(d, width, intensity):
    """Emission of a lit line at distance d: AA core + two exponential falloffs."""
    core = np.clip((width - d) + 0.5, 0, 1)
    glow = np.exp(-d / 9.0)
    halo = np.exp(-d / 42.0)
    return (core[..., None] * C_CORE * 2.1
            + glow[..., None] * C_GLOW * 1.35
            + halo[..., None] * C_HALO * 0.42) * intensity


def splat(g, buf, x, y, radius, color, amount):
    """Soft additive dot (spark / mote / comet head)."""
    x0 = int(max(g.x0, x - radius * 4)) - g.x0
    x1 = int(min(g.x1, x + radius * 4)) - g.x0
    y0 = int(max(g.y0, y - radius * 4)) - g.y0
    y1 = int(min(g.y1, y + radius * 4)) - g.y0
    if x1 <= x0 or y1 <= y0:
        return
    sx = g.x[y0:y1, x0:x1] - x
    sy = g.y[y0:y1, x0:x1] - y
    w = np.exp(-(sx * sx + sy * sy) / (2 * radius * radius)) * amount
    buf[y0:y1, x0:x1] += w[..., None] * color


# ---------------------------------------------------------------- scene parts

def draw_strips(g, buf, reveal=1.0, intensity=1.0, gap=STRIP_GAP, head=0.0):
    """Chevrons drawn from the apex out to `reveal` (0..1); head = brightness of
    the leading point while drawing."""
    for a, b in strip_segments(gap):
        if reveal <= 0:
            continue
        d, t = seg_dist(g, a, b, min(1.0, reveal))
        # comet tail while drawing: brighter toward the head
        tail = 1.0 if reveal >= 1 else (0.35 + 0.65 * (t / max(reveal, 1e-3)) ** 2)
        buf += light(d, 3.2, intensity) * (tail[..., None] if reveal < 1 else 1.0)
        if head > 0 and reveal < 1:
            hx = a[0] + (b[0] - a[0]) * reveal
            hy = a[1] + (b[1] - a[1]) * reveal
            splat(g, buf, hx, hy, 7, C_CORE * 6 + C_GLOW * 4, head)


def draw_comets(g, buf, phase, strength):
    """Energy pulses running from each end toward the apex."""
    for a, b in strip_segments():
        p = 1.0 - phase
        hx, hy = a[0] + (b[0] - a[0]) * p, a[1] + (b[1] - a[1]) * p
        d, t = seg_dist(g, a, b)
        # tail trails behind the head (toward the end)
        behind = np.clip((t - p) / 0.35, 0, 1)
        trail = np.where(t >= p, (1 - behind) ** 2, 0)
        buf += (np.exp(-d / 6.0) * trail)[..., None] * (C_CORE * 2.2 + C_GLOW * 2.0) * strength
        splat(g, buf, hx, hy, 6, C_CORE * 5 + C_GLOW * 3, strength)


def draw_island(g, buf, amount, trace=1.0, spill=1.0):
    if amount <= 0:
        return
    d = rrect_dist(g, ISLAND, ISLAND_R)
    line = np.clip(1.6 - d, 0, 1)
    if trace < 1:
        # reveal clockwise from the top centre
        ang = (np.arctan2(g.x - CX, -(g.y - CY)) / (2 * math.pi)) % 1.0
        line = line * (ang <= trace)
    # light from the strips spills onto the island edge
    sd = np.full(g.shape, 1e9, np.float32)
    for a, b in strip_segments():
        sd = np.minimum(sd, seg_dist(g, a, b)[0])
    lit = 1.0 + 3.0 * np.exp(-sd / 90.0) * spill
    buf += (line * lit)[..., None] * C_ISLAND * amount
    for lx, ly, lr in LENSES:
        ld = np.abs(np.hypot(g.x - lx, g.y - ly) - lr)
        ring = np.clip(1.6 - ld, 0, 1) * 0.7
        buf += ring[..., None] * C_ISLAND * amount


def draw_motes(g, buf, t, amount):
    """Slow rising light motes; periodic in t (0..1)."""
    if amount <= 0:
        return
    for i in range(26):
        bx = CX + (hash01(i) - 0.5) * 520
        span = 520 + 200 * hash01(i + 50)
        y = CY + 300 - span * ((t + hash01(i + 100)) % 1.0)
        x = bx + 18 * math.sin(2 * math.pi * (t * 2 + hash01(i + 7)))
        tw = 0.5 + 0.5 * math.sin(2 * math.pi * (t * 3 + hash01(i + 9)))
        fade = math.sin(math.pi * ((t + hash01(i + 100)) % 1.0))
        splat(g, buf, x, y, 2.2 + 1.5 * hash01(i + 3), C_GLOW * 2 + C_CORE,
              amount * 0.55 * tw * fade)


def draw_sparks(g, buf, t, amount):
    """Sparks shed from the racing heads during the intro (t 0..1 of the run)."""
    if amount <= 0:
        return
    for i in range(40):
        seg = i % 4
        a, b = strip_segments()[seg]
        born = hash01(i + 200) * 0.8
        age = t - born
        if age < 0 or age > 0.5:
            continue
        hx = a[0] + (b[0] - a[0]) * ease_in_out(born / 0.8)
        hy = a[1] + (b[1] - a[1]) * ease_in_out(born / 0.8)
        ang = hash01(i + 300) * 2 * math.pi
        sp = 120 + 260 * hash01(i + 400)
        x = hx + math.cos(ang) * sp * age
        y = hy + math.sin(ang) * sp * age + 180 * age * age   # a little gravity
        splat(g, buf, x, y, 1.8, C_CORE * 4 + C_GLOW * 3, amount * (1 - age / 0.5) ** 2)


def text_mask(text, font, spacing, size_wh, origin, letter_fx=None):
    """Grayscale float mask (crop coords). letter_fx(i) -> (dy, alpha)."""
    im = Image.new("L", size_wh, 0)
    d = ImageDraw.Draw(im)
    widths = [d.textlength(ch, font=font) for ch in text]
    total = sum(widths) + spacing * (len(text) - 1)
    x = W / 2 - total / 2 - origin[0]
    for i, (ch, w) in enumerate(zip(text, widths)):
        dy, alpha = letter_fx(i) if letter_fx else (0, 1)
        if alpha > 0:
            layer = Image.new("L", size_wh, 0)
            ImageDraw.Draw(layer).text((x, origin[1] + dy), ch, font=font, fill=255)
            im = Image.fromarray(np.maximum(np.asarray(im),
                                            (np.asarray(layer) * alpha).astype(np.uint8)))
        x += w + spacing
    return np.asarray(im, np.float32) / 255


# ---------------------------------------------------------------- post

def box_blur(a, r):
    if r < 1:
        return a
    k = 2 * r + 1
    for axis in (0, 1):
        pad = [(0, 0)] * a.ndim
        pad[axis] = (r + 1, r)
        c = np.cumsum(np.pad(a, pad, mode="edge"), axis=axis, dtype=np.float32)
        hi = np.take(c, np.arange(k, c.shape[axis]), axis=axis)
        lo = np.take(c, np.arange(0, c.shape[axis] - k), axis=axis)
        a = (hi - lo) / k
    return a


def soft(a, r):
    """Three box passes: close to a Gaussian, no square halo."""
    return box_blur(box_blur(box_blur(a, r), r), r)


def downsample(a, f):
    h, w = a.shape[0] // f * f, a.shape[1] // f * f
    return a[:h, :w].reshape(h // f, f, w // f, f, -1).mean(axis=(1, 3))


def upsample(a, shape):
    im = [Image.fromarray(a[..., c]).resize((shape[1], shape[0]), Image.BILINEAR)
          for c in range(a.shape[2])]
    return np.stack([np.asarray(i, np.float32) for i in im], axis=-1)


def bloom(hdr):
    bright = np.maximum(hdr - 0.6, 0)
    out = np.zeros_like(hdr)
    for f, r, w in ((2, 3, 0.55), (4, 4, 0.35), (8, 5, 0.25)):
        small = box_blur(box_blur(downsample(bright, f), r), r)
        out += upsample(small, hdr.shape) * w
    return hdr + out


def aces(x):
    a, b, c, d, e = 2.51, 0.03, 2.43, 0.59, 0.14
    return np.clip((x * (a * x + b)) / (x * (c * x + d) + e), 0, 1)


BAYER = (np.array([[0, 32, 8, 40, 2, 34, 10, 42], [48, 16, 56, 24, 50, 18, 58, 26],
                   [12, 44, 4, 36, 14, 46, 6, 38], [60, 28, 52, 20, 62, 30, 54, 22],
                   [3, 35, 11, 43, 1, 33, 9, 41], [51, 19, 59, 27, 49, 17, 57, 25],
                   [15, 47, 7, 39, 13, 45, 5, 37], [63, 31, 55, 23, 61, 29, 53, 21]],
                  np.float32) + 0.5) / 64 - 0.5


def finish(hdr, aberration=0.0):
    img = aces(bloom(hdr) * 1.1)
    if aberration > 0.05:
        s = int(round(aberration))
        img[..., 0] = np.roll(img[..., 0], s, axis=1)
        img[..., 2] = np.roll(img[..., 2], -s, axis=1)
    srgb = np.where(img <= 0.0031308, img * 12.92, 1.055 * np.power(img, 1 / 2.4) - 0.055)
    h, w = srgb.shape[:2]
    v = srgb * 255
    # dither only real gradients: noise in pure black would bloat every PNG
    dither = np.tile(BAYER, (h // 8 + 1, w // 8 + 1))[:h, :w, None] * (v > 2.0)
    return Image.fromarray(np.clip(v + dither + 0.5, 0, 255).astype(np.uint8), "RGB")


# ---------------------------------------------------------------- frames

def neo_os(g, buf, y, alpha, spacing, font):
    if alpha <= 0:
        return
    m = text_mask("NEO OS", font, spacing, (g.x1 - g.x0, g.y1 - g.y0), (g.x0, y - g.y0))
    glow = soft(m, 5)
    buf += (m[..., None] * (C_GLOW * 1.5 + C_CORE * 0.6)
            + glow[..., None] * C_HALO * 1.2) * alpha


def frame_intro(i, fonts):
    g = Grid(CROP_A)
    buf = np.zeros(g.shape + (3,), np.float32)
    s = i / FPS
    # sparks ignite at the apexes with a flicker (anticipation)
    if s < 0.32:
        flick = (0.4 + 0.6 * hash01(i * 3)) * ease_out_cubic(s / 0.32)
        for a, _ in strip_segments()[::2]:
            splat(g, buf, a[0], a[1], 8, C_CORE * 6 + C_GLOW * 4, flick)
    run = (s - 0.25) / 0.75
    reveal = ease_in_out(run)
    draw_strips(g, buf, reveal, 0.35 + 0.65 * ease_out_cubic(run), head=1.0 if run < 1 else 0)
    draw_sparks(g, buf, clamp01(run), 1.0)
    draw_island(g, buf, ease_out_cubic((s - 0.55) / 0.6), trace=ease_in_out((s - 0.55) / 0.7))
    nt = (s - 0.9) / 0.7
    neo_os(g, buf, NEO_Y, ease_out_cubic(nt), 46 - 22 * ease_out_cubic(nt), fonts["small"])
    return finish(buf)


def frame_loop(i, fonts, n):
    g = Grid(CROP_A)
    buf = np.zeros(g.shape + (3,), np.float32)
    t = i / n
    breathe = 0.72 + 0.28 * math.cos(2 * math.pi * t)
    draw_strips(g, buf, 1.0, breathe)
    for k in range(2):
        ph = (t * 2 + k * 0.5) % 1.0
        draw_comets(g, buf, ph, math.sin(math.pi * ph) * 0.8)
    draw_island(g, buf, 1.0, spill=breathe)
    draw_motes(g, buf, t, 1.0)
    neo_os(g, buf, NEO_Y, 0.85 + 0.15 * math.cos(2 * math.pi * t), 24, fonts["small"])
    return finish(buf)


def frame_finale(i, fonts):
    g = Grid(CROP_B)
    buf = np.zeros(g.shape + (3,), np.float32)
    s = i / FPS
    # anticipation: dim and pull apart, then snap in past the rest pose
    if s < 0.18:
        a = ease_in_out(s / 0.18)
        intensity, gap = 1.0 - 0.4 * a, STRIP_GAP + 10 * a
    else:
        k = ease_out_back((s - 0.18) / 0.32)
        gap = STRIP_GAP + 10 - 10 * k
        flash = math.exp(-(s - 0.18) / 0.22)
        intensity = 1.0 + 2.4 * flash
    draw_strips(g, buf, 1.0, intensity, gap=gap)
    draw_island(g, buf, 1.0, spill=min(intensity, 2.0))
    # shockwave ring
    if 0.18 < s < 0.9:
        # stays inside the finale crop (480 px above/left/right of the centre)
        r = 40 + 400 * ease_out_cubic((s - 0.18) / 0.72)
        rd = np.abs(np.hypot(g.x - CX, g.y - CY) - r)
        fade = (1 - clamp01((s - 0.18) / 0.72)) ** 1.5
        buf += (np.exp(-rd / 7.0) * fade)[..., None] * (C_GLOW * 1.4 + C_CORE * 0.4)
    # NEO OS glides down to make room for the title
    gy = NEO_Y + (NEO_Y_FINAL - NEO_Y) * ease_in_out((s - 0.35) / 0.55)
    neo_os(g, buf, gy, 1.0, 24, fonts["small"])

    # POCO F4 GT: letters rise in with overshoot, staggered
    def fx(li):
        lt = (s - 0.5 - li * 0.045) / 0.42
        return (34 * (1 - ease_out_back(lt)), clamp01(lt * 1.6))
    m = text_mask("POCO F4 GT", fonts["big"], 14, (g.x1 - g.x0, g.y1 - g.y0),
                  (g.x0, TITLE_Y - g.y0), fx)
    if m.any():
        sweep = 0.0
        if 1.25 < s < 1.95:
            # diagonal light band crossing the title once
            p = ease_in_out((s - 1.25) / 0.7)
            band = (g.x - (60 + 1000 * p)) + (g.y - TITLE_Y) * 0.45
            sweep = np.exp(-(band / 26.0) ** 2) * 2.4
        glow = soft(m, 6)
        buf += m[..., None] * C_TEXT * (1.25 + sweep if np.isscalar(sweep) else 1.25 + sweep[..., None])
        buf += glow[..., None] * C_HALO * 0.9
    aberr = 6.0 * math.exp(-max(0.0, s - 0.18) / 0.12) if s > 0.18 else 0.0
    return finish(buf, aberr)


def render(job):
    part, i, n = job
    fonts = {"big": ImageFont.truetype(FONT_PATH, 120),
             "small": ImageFont.truetype(FONT_PATH, 52)}
    if part == "part0":
        im = frame_intro(i, fonts)
    elif part == "part1":
        im = frame_loop(i, fonts, n)
    else:
        im = frame_finale(i, fonts)
    # 256-colour palette (pngquant-style): the scene is almost all blues, so
    # this is visually lossless and makes frames ~3x smaller to store and decode
    im = im.quantize(256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.FLOYDSTEINBERG)
    b = io.BytesIO()
    im.save(b, "PNG", optimize=True)
    return part, i, b.getvalue()


def init(font):
    global FONT_PATH
    FONT_PATH = font


def main():
    out, font = sys.argv[1], sys.argv[2]
    jobs_n = int(sys.argv[3]) if len(sys.argv) > 3 else multiprocessing.cpu_count()
    counts = {"part0": 96, "part1": 144, "part2": 168}
    crops = {"part0": CROP_A, "part1": CROP_A, "part2": CROP_B}
    jobs = [(p, i, n) for p, n in counts.items() for i in range(n)]
    init(font)
    with multiprocessing.Pool(jobs_n, initializer=init, initargs=(font,)) as pool:
        results = pool.map(render, jobs, chunksize=4)
    frames = {p: [None] * n for p, n in counts.items()}
    for p, i, data in results:
        frames[p][i] = data

    desc = f"{W} {H} {FPS}\nc 1 0 part0\nc 0 0 part1\nc 1 0 part2\n"
    with zipfile.ZipFile(out, "w", zipfile.ZIP_STORED) as z:
        z.writestr("desc.txt", desc)
        for p, data in frames.items():
            x0, y0, x1, y1 = crops[p]
            trim = f"{x1 - x0}x{y1 - y0}+{x0}+{y0}\n" * len(data)
            z.writestr(f"{p}/trim.txt", trim)
            for i, d in enumerate(data):
                z.writestr(f"{p}/{i:03d}.png", d)
    print(f"wrote {out}: " + ", ".join(f"{p} {len(v)}" for p, v in frames.items()))


if __name__ == "__main__":
    main()
