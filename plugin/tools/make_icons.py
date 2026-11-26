"""Generate the plugin's toolbar icons with the standard library only. Each action has its own:
  simulate: the viewer's field render (rings from the feed point, blue negative / orange positive) with a
            white "play" badge, meaning run;
  farm:     three machines linked to a hub, meaning the GPU farm.
  window:   a window frame with a title bar showing the same rings, meaning open the solver window.
KiCAD draws an IPC plugin's toolbar button only if the action has icons. Sizes 24 and 48 pixels (a 32 pixel
bitmap renders too large in some setups). The colour constants mirror src/viz/colormap.hpp.
  python tools/make_icons.py
"""

import math
import struct
import zlib
from pathlib import Path

NEUTRAL, POS, NEG = (20, 20, 24), (245, 98, 66), (66, 135, 245)  # src/viz/colormap.hpp
FRAME = {"light": (70, 70, 88), "dark": (205, 205, 220)}  # window frame and title bar per theme


def png(path, size, rows):
    raw = b"".join(b"\x00" + bytes(v for px in row for v in px) for row in rows)

    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)  # 8-bit RGBA
    Path(path).write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", ihdr)
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b"")
    )


def diverging(v):
    t = max(-1.0, min(1.0, v))
    a = abs(t) ** 0.7  # the viewer's gamma
    end = POS if t >= 0 else NEG
    return tuple(NEUTRAL[i] + (end[i] - NEUTRAL[i]) * a for i in range(3))


def rounded(px, py, x0, y0, x1, y1, r):
    """True if (px, py) is inside the rounded rectangle."""
    qx = abs(px - (x0 + x1) / 2) - ((x1 - x0) / 2 - r)
    qy = abs(py - (y0 + y1) / 2) - ((y1 - y0) / 2 - r)
    return math.hypot(max(qx, 0), max(qy, 0)) + min(max(qx, qy), 0) <= r


def rings(px, py, sx, sy, wavelength, decay):
    r = math.hypot(px - sx, py - sy)
    return diverging(math.cos(2 * math.pi * r / wavelength) * 1.15 * math.exp(-r / decay))


def in_triangle(px, py, a, b, c):
    def side(p, q, r):
        return (p[0] - r[0]) * (q[1] - r[1]) - (q[0] - r[0]) * (p[1] - r[1])

    d = [side((px, py), a, b), side((px, py), b, c), side((px, py), c, a)]
    return all(x >= 0 for x in d) or all(x <= 0 for x in d)


def colour_at(kind, theme, px, py, big):
    """RGB or None (transparent) for one supersample of the icon."""
    if kind == "simulate":
        if not rounded(px, py, 0, 0, big, big, 0.22 * big):
            return None
        bx, by, br = 0.72 * big, 0.72 * big, 0.25 * big  # play badge, bottom right
        if math.hypot(px - bx, py - by) <= br:
            tri = in_triangle(
                px,
                py,
                (bx - 0.10 * big, by - 0.14 * big),
                (bx - 0.10 * big, by + 0.14 * big),
                (bx + 0.15 * big, by),
            )
            return (255, 255, 255) if tri else (35, 35, 45)
        c = rings(px, py, 0.27 * big, 0.45 * big, 0.30 * big, 0.85 * big)
        return (255, 255, 255) if math.hypot(px - 0.27 * big, py - 0.45 * big) <= 0.075 * big else c
    if kind == "farm":  # three small machines linked to one hub: the GPU farm
        nodes = [(0.20, 0.22), (0.80, 0.22), (0.50, 0.84)]
        hub = (0.50, 0.46)
        x, y = px / big, py / big
        for nx, ny in nodes:  # links
            dx, dy = hub[0] - nx, hub[1] - ny
            t = max(0.0, min(1.0, ((x - nx) * dx + (y - ny) * dy) / (dx * dx + dy * dy)))
            if math.hypot(x - (nx + t * dx), y - (ny + t * dy)) <= 0.035:
                return FRAME[theme]
        if math.hypot(x - hub[0], y - hub[1]) <= 0.13:
            return (255, 255, 255) if math.hypot(x - hub[0], y - hub[1]) <= 0.05 else (235, 120, 40)
        for nx, ny in nodes:
            if rounded(
                px, py, (nx - 0.15) * big, (ny - 0.11) * big, (nx + 0.15) * big, (ny + 0.11) * big, 0.04 * big
            ):
                return FRAME[theme]
        return None
    # window: frame with a title bar, rings inside
    if not rounded(px, py, 0.02 * big, 0.10 * big, 0.98 * big, 0.92 * big, 0.14 * big):
        return None
    border = 0.07 * big
    inner = rounded(
        px, py, 0.02 * big + border, 0.10 * big + border, 0.98 * big - border, 0.92 * big - border, 0.07 * big
    )
    if not inner or py < 0.10 * big + 0.22 * big:  # frame, or the title bar across the top
        return FRAME[theme]
    c = rings(px, py, 0.30 * big, 0.62 * big, 0.28 * big, 0.80 * big)
    return (255, 255, 255) if math.hypot(px - 0.30 * big, py - 0.62 * big) <= 0.06 * big else c


def render(kind, theme, size, ss=4):
    big = size * ss
    rows = []
    for y in range(size):
        row = []
        for x in range(size):
            acc, n = [0.0, 0.0, 0.0], 0
            for j in range(ss):
                for i in range(ss):
                    c = colour_at(kind, theme, x * ss + i + 0.5, y * ss + j + 0.5, big)
                    if c is not None:
                        n += 1
                        for k in range(3):
                            acc[k] += c[k]
            row.append(
                tuple(round(a / n) for a in acc) + (round(255 * n / (ss * ss)),) if n else (0, 0, 0, 0)
            )
        rows.append(row)
    return rows


def main():
    out = Path(__file__).resolve().parents[1] / "icons"
    out.mkdir(exist_ok=True)
    for old in out.glob("*.png"):
        old.unlink()
    for kind in ("simulate", "window", "farm"):
        for theme in ("light", "dark"):
            for size in (24, 48):
                png(out / f"{kind}_{theme}_{size}.png", size, render(kind, theme, size))
    print("wrote", sorted(p.name for p in out.glob("*.png")))


if __name__ == "__main__":
    main()
