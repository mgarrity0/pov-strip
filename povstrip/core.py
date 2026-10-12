"""Design math and the image resampler, Python edition of studio/core.js.

Kept in step with core.js (tools/check_resampler cross-checks all three
implementations: this, the JS, and the firmware's integer version).
"""
from __future__ import annotations

import math
from dataclasses import dataclass

import numpy as np

CHIPS = {
    "APA102": dict(clocked=True, maxMHz=None, order="BGR"),
    "SK9822": dict(clocked=True, maxMHz=30, order="BGR"),
    "HD107S": dict(clocked=True, maxMHz=30, order="BGR"),
    "WS2812B": dict(clocked=False, bitNs=1250, resetUs=280, order="GRB"),
    "WS2812B_V5": dict(clocked=False, bitNs=1225, resetUs=280, order="GRB"),
    "SK6812": dict(clocked=False, bitNs=1200, resetUs=80, order="GRB"),
    "WS2811": dict(clocked=False, bitNs=1250, resetUs=280, order="RGB"),
    "WS2813": dict(clocked=False, bitNs=1280, resetUs=300, order="GRB"),
    "WS2815": dict(clocked=False, bitNs=1890, resetUs=300, order="GRB"),
}


def led_count(length_m: float, density: int, n_max: int = 10**9) -> int:
    return min(n_max, max(1, round(length_m * density)))


def frame_bytes(n: int) -> int:
    b = 4 + 4 * n + 4 + ((n + 15) // 16 + 1)
    return (b + 3) & ~3


def actual_spi_hz(mhz: float) -> float:
    return 80e6 / max(1, round(80 / max(0.01, mhz)))


def chip_min_col_us(chip: str, n: int, spi_mhz: float) -> int:
    c = CHIPS[chip]
    if c["clocked"]:
        return math.ceil(frame_bytes(n) * 8 * 1e6 / actual_spi_hz(spi_mhz)) + 25
    return n * 24 * c["bitNs"] // 1000 + c["resetUs"] + 25


@dataclass
class Design:
    n: int
    pitch_m: float
    row_deg: float
    strip_deg: float
    want_us: int
    chip_us: int
    col_us: int
    col_deg: float
    glance_ms: float

    @property
    def chip_limited(self) -> bool:
        return self.want_us < self.chip_us


def design(length_m: float, density: int, dist_m: float, sweep_dps: float, glance_deg: float,
           chip: str, spi_mhz: float = 16.0, col_us_manual: int = 0) -> Design:
    n = led_count(length_m, density)
    pitch = 1.0 / density
    row_deg = math.degrees(2 * math.atan(pitch / (2 * dist_m)))
    strip_deg = math.degrees(2 * math.atan(length_m / (2 * dist_m)))
    sq_us = row_deg / sweep_dps * 1e6
    want = max(1, col_us_manual) if col_us_manual else round(sq_us)
    chip_us = chip_min_col_us(chip, n, spi_mhz)
    col_us = max(want, chip_us)
    return Design(n, pitch, row_deg, strip_deg, want, chip_us, col_us, sweep_dps * col_us * 1e-6,
                  glance_deg / sweep_dps * 1e3)


def columns_for(d: Design, aspect: float, scale: float) -> int:
    return max(1, min(8192, round(aspect * scale * d.n * d.row_deg / d.col_deg)))


def recommended_gap_ms(glance_ms: float, pass_ms: float) -> float:
    return max(5.0, glance_ms - pass_ms)


def area_matrix(S: int, T: int) -> np.ndarray:
    """T x S weights: target cell k covers source [k*S/T, (k+1)*S/T). Rows sum to 1.
    Same integer-overlap scheme as core.js areaSpans and the firmware."""
    W = np.zeros((T, S), dtype=np.float64)
    for k in range(T):
        a, b = k * S, (k + 1) * S
        i0, i1 = a // T, min(S - 1, (b - 1) // T)
        for i in range(i0, i1 + 1):
            W[k, i] = (min(b, (i + 1) * T) - max(a, i * T)) / S
    return W


def area_resample(src_rgb: np.ndarray, dw: int, dh: int, top: int, dh2: int, gamma: float,
                  nearest: bool = False) -> np.ndarray:
    """Area-average resample in linear light (through the LED output gamma).

    src_rgb: (sh, sw, 3) uint8, row-major. Returns column-major (dw, dh, 3) uint8 with
    the source in rows [top, top+dh2) of each column, black elsewhere.
    """
    sh, sw = src_rgb.shape[:2]
    lin = (src_rgb.astype(np.float64) / 255.0) ** gamma  # (sh, sw, 3)
    if nearest:
        xi = np.minimum(sw - 1, ((np.arange(dw) + 0.5) * sw / dw).astype(int))
        yi = np.minimum(sh - 1, ((np.arange(dh2) + 0.5) * sh / dh2).astype(int))
        res = lin[yi][:, xi]  # (dh2, dw, 3)
    else:
        Wx, Wy = area_matrix(sw, dw), area_matrix(sh, dh2)
        res = np.stack([Wy @ lin[:, :, c] @ Wx.T for c in range(3)], axis=-1)  # (dh2, dw, 3)
    enc = np.round(255.0 * np.clip(res, 0, 1) ** (1.0 / gamma)).astype(np.uint8)
    out = np.zeros((dw, dh, 3), dtype=np.uint8)
    out[:, top:top + dh2, :] = enc.transpose(1, 0, 2)
    return out


def render_image_file(path: str, d: Design, scale: float = 1.0, gamma: float = 2.4, invert: bool = False,
                      pixel_art: bool = False, max_src: int = 1200) -> tuple[np.ndarray, int]:
    """Loads an image with Pillow and resamples it to the strip grid.
    Returns (column-major (cols, rows, 3) uint8, cols)."""
    from PIL import Image

    im = Image.open(path).convert("RGB")
    k = min(1.0, max_src / max(im.width, im.height))
    if k < 1.0:
        im = im.resize((max(1, round(im.width * k)), max(1, round(im.height * k))),
                       Image.NEAREST if pixel_art else Image.LANCZOS)
    src = np.asarray(im)
    if invert:
        src = 255 - src
    rows = d.n
    cols = columns_for(d, im.width / im.height, scale)
    dh2 = max(1, min(rows, round(scale * rows)))
    top = (rows - dh2) // 2
    return area_resample(src, cols, rows, top, dh2, gamma, nearest=pixel_art and src.shape[1] <= cols), cols
