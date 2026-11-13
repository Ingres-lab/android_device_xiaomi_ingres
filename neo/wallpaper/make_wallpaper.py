# Neo OS default wallpaper: abstract, soft light glows. Deterministic (fixed seed).
import numpy as np
from PIL import Image
W, H = 1440, 3200
rng = np.random.default_rng(7)
y, x = np.mgrid[0:H, 0:W].astype(np.float32)
u, v = x / W, y / H

def srgb_to_lin(c): c = np.asarray(c, np.float32) / 255.0; return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)
def lin_to_srgb(c): return np.where(c <= 0.0031308, c * 12.92, 1.055 * np.power(np.clip(c, 0, None), 1 / 2.4) - 0.055)

# base: deep navy at the top fading to deep indigo at the bottom
top, bot = srgb_to_lin([6, 10, 28]), srgb_to_lin([16, 9, 40])
img = top[None, None, :] * (1 - v[..., None]) + bot[None, None, :] * v[..., None]

def glow(cx, cy, rx, ry, color, strength, angle=0.0):
    ca, sa = np.cos(angle), np.sin(angle)
    dx, dy = (u - cx), (v - cy) * (H / W)
    xr, yr = dx * ca + dy * sa, -dx * sa + dy * ca
    g = np.exp(-((xr / rx) ** 2 + (yr / ry) ** 2))
    return g[..., None] * srgb_to_lin(color)[None, None, :] * strength

light = np.zeros_like(img)
light += glow(0.20, 0.28, 0.42, 0.30, [0, 180, 255], 0.55)          # Neo blue, upper left
light += glow(0.88, 0.50, 0.36, 0.34, [140, 90, 255], 0.42)         # violet, right
light += glow(0.30, 0.82, 0.44, 0.26, [255, 70, 180], 0.30)         # magenta, lower
light += glow(0.66, 0.16, 0.20, 0.14, [0, 235, 220], 0.26)          # small cyan bloom
light += glow(0.50, 0.55, 1.10, 0.09, [120, 200, 255], 0.16, angle=-0.55)  # long diagonal light streak

# soft bloom tone map, then a gentle vignette
img = img + (1 - np.exp(-light * 1.3)) * 0.70
vig = 1 - 0.35 * (((u - 0.5) * 1.1) ** 2 + ((v - 0.5) * 0.9) ** 2)
img = np.clip(img * vig[..., None], 0, 1)

out = lin_to_srgb(img) * 255.0
out += rng.normal(0, 0.9, out.shape)  # fine grain, prevents banding in the smooth gradients
Image.fromarray(np.clip(out, 0, 255).astype(np.uint8), "RGB").save(__import__("sys").argv[1], quality=94, subsampling=0, optimize=True)
