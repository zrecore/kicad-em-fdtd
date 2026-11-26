"""Gerber (RS-274X) and Excellon reader and rasteriser, for turning a vendor's manufacturing files into a model.
Needs numpy. Written for this project from the public format description (the Gerber specification is a published
document); not derived from any other implementation.

Supported: %FSLA..% (leading zeros omitted) coordinate formats, inch or mm (%MOIN%, %MOMM%, G70, G71), apertures
C, R, O and macros built from the primitives 1 (circle), 4 (outline), 20 (vector line), 21 (centre rectangle) and 22
(lower-left rectangle), D01 strokes with round apertures, D03 flashes, G01/G02/G03 with G75 (multi-quadrant) arcs and
G74 (single-quadrant) arcs, G36/G37 regions, %LPD% and %LPC% polarity. Not supported (reported, not guessed):
step-and-repeat, aperture blocks, rotated or mirrored images, strokes with a macro aperture (drawn as a 0.1 mm
round stroke).
All lengths in the results are millimetres, y up.
"""
import math
import re
import struct
import zlib

import numpy as np

MM_PER_INCH = 25.4


class GerberError(Exception):
    pass


def _rot(x, y, deg):
    c, s = math.cos(math.radians(deg)), math.sin(math.radians(deg))
    return x * c - y * s, x * s + y * c


def parse(path):
    """Returns a list of operations: ('stroke', [(x, y)...], aperture, clear), ('flash', x, y, aperture, clear),
    ('region', [(x, y)...], clear); aperture is ('C', d), ('R', w, h), ('O', w, h) or ('M', primitives)."""
    text = open(path, "rb").read().decode("latin1")
    to_mm = MM_PER_INCH  # inch unless %MOMM% / G71 says otherwise
    scale = 1e-4
    macros, apertures = {}, {}
    ops = []
    x = y = 0.0
    cur = None
    mode = "G01"
    multi = True
    clear = False
    region = None
    last_op = 1
    for par, data in re.findall(r"%(.*?)%|([^%]+)", text, re.S):
        if par:
            p = par.strip().replace("\n", "").replace("\r", "")
            m = re.match(r"FSLAX(\d)(\d)Y(\d)(\d)", p)
            if m:
                scale = 10.0 ** -int(m.group(2))
                continue
            if p.startswith("MOIN"):
                to_mm = MM_PER_INCH
                continue
            if p.startswith("MOMM"):
                to_mm = 1.0
                continue
            if p.startswith("LPC"):
                clear = True
                continue
            if p.startswith("LPD"):
                clear = False
                continue
            m = re.match(r"AM([\w.]+)\*(.*)", par.strip(), re.S)
            if m:
                prims = []
                for line in m.group(2).replace("\n", "").replace("\r", "").split("*"):
                    line = line.strip()
                    if line and not line.startswith("0 "):
                        prims.append([float(t) for t in line.split(",") if t.strip() != ""])
                macros[m.group(1)] = prims
                continue
            m = re.match(r"ADD(\d+)([\w.]+)(?:,([\d.]+)(?:X([\d.]+))?)?", p)
            if m:
                code, shape = int(m.group(1)), m.group(2)
                if shape in macros:
                    apertures[code] = ("M", macros[shape])
                elif shape == "C":
                    apertures[code] = ("C", float(m.group(3)))
                elif shape in ("R", "O"):
                    apertures[code] = (shape, float(m.group(3)), float(m.group(4) or m.group(3)))
                else:
                    raise GerberError("aperture shape %s is not supported" % shape)
            continue
        for blk in data.replace("\n", "").replace("\r", "").split("*"):
            blk = blk.strip()
            if not blk:
                continue
            if blk == "G70":
                to_mm = MM_PER_INCH
                continue
            if blk == "G71":
                to_mm = 1.0
                continue
            if blk == "G74":
                multi = False
                continue
            if blk == "G75":
                multi = True
                continue
            m = re.match(r"^(G\d+)?(?:X(-?\d+))?(?:Y(-?\d+))?(?:I(-?\d+))?(?:J(-?\d+))?(?:D(\d+))?$", blk)
            if not m:
                continue
            g, xs, ys, i_, j_, d = m.groups()
            if g in ("G01", "G02", "G03"):
                mode = g
            if g == "G36":
                region = []
                continue
            if g == "G37":
                if region and len(region) >= 3:
                    ops.append(("region", [(a * to_mm * scale, b * to_mm * scale) for a, b in region], clear))
                region = None
                continue
            if d is not None and int(d) >= 10 and xs is None and ys is None:
                cur = int(d)
                continue
            if xs is None and ys is None and d is None:
                continue  # a bare G code
            nx = int(xs) if xs is not None else x
            ny = int(ys) if ys is not None else y
            op = int(d) if d is not None else last_op
            last_op = op if op in (1, 2, 3) else last_op
            pts = [(x, y), (nx, ny)]
            if op == 1 and mode in ("G02", "G03") and i_ is not None:
                ci, cj = int(i_), int(j_ if j_ is not None else 0)
                pts = _arc((x, y), (nx, ny), ci, cj, mode == "G03", multi, scale * to_mm)
            if op == 2:
                if region is not None:
                    region.append((nx, ny))
            elif op == 3:
                ops.append(("flash", nx * scale * to_mm, ny * scale * to_mm, apertures.get(cur), clear))
            elif op == 1:
                if region is not None:
                    region.extend(pts if not region else pts[1:])
                else:
                    ops.append(("stroke", [(a * scale * to_mm, b * scale * to_mm) for a, b in pts], apertures.get(cur), clear))
            x, y = nx, ny
    # apertures were stored in file units: convert once now
    out = []
    for o in ops:
        if o[0] == "stroke":
            out.append(("stroke", o[1], _ap_mm(o[2], to_mm), o[3]))
        elif o[0] == "flash":
            out.append(("flash", o[1], o[2], _ap_mm(o[3], to_mm), o[4]))
        else:
            out.append(o)
    return out


def _macro_mm(prims, to_mm):
    """Macro primitive parameters are lengths in file units except the code, exposure, vertex count and rotation."""
    out = []
    for p in prims:
        code = int(p[0])
        q = list(p)
        if code == 1:
            lengths = range(2, min(len(q), 5))
        elif code == 4:
            n = int(q[2])
            lengths = range(3, min(len(q), 3 + 2 * (n + 1)))
        elif code == 20:
            lengths = range(2, min(len(q), 7))
        elif code in (21, 22):
            lengths = range(2, min(len(q), 6))
        else:
            raise GerberError("macro primitive %d is not supported" % code)
        for k in lengths:
            q[k] = q[k] * to_mm
        out.append(q)
    return out


def _ap_mm(ap, to_mm):
    if ap is None:
        return None
    if ap[0] == "C":
        return ("C", ap[1] * to_mm)
    if ap[0] in ("R", "O"):
        return (ap[0], ap[1] * to_mm, ap[2] * to_mm)
    return ("M", _macro_mm(ap[1], to_mm))


def _arc(start, end, i, j, ccw, multi, unit):
    """Points of an arc in integer coordinates; i and j are centre offsets in the same integer units."""
    sx, sy = start
    ex, ey = end
    cands = [(sx + i, sy + j)] if multi else [(sx + a * i, sy + b * j) for a in (1, -1) for b in (1, -1)]
    best = None
    for cx, cy in cands:
        r0, r1 = math.hypot(sx - cx, sy - cy), math.hypot(ex - cx, ey - cy)
        a0, a1 = math.atan2(sy - cy, sx - cx), math.atan2(ey - cy, ex - cx)
        if ccw:
            while a1 <= a0:
                a1 += 2 * math.pi
        else:
            while a1 >= a0:
                a1 -= 2 * math.pi
        sweep = abs(a1 - a0)
        score = abs(r0 - r1) + (0 if multi or sweep <= math.pi / 2 + 1e-6 else 1e9)
        if best is None or score < best[0]:
            best = (score, cx, cy, r0, a0, a1)
    _, cx, cy, r, a0, a1 = best
    n = max(4, int(abs(a1 - a0) * r * unit / 0.05))
    return [(cx + r * math.cos(a0 + (a1 - a0) * k / n), cy + r * math.sin(a0 + (a1 - a0) * k / n)) for k in range(n + 1)]


class Canvas:
    def __init__(self, x0, y0, x1, y1, res):
        self.x0, self.y0, self.res = x0, y0, res
        self.w = int(math.ceil((x1 - x0) / res))
        self.h = int(math.ceil((y1 - y0) / res))
        self.img = np.zeros((self.h, self.w), bool)

    def _set(self, ix0, ix1, iy0, iy1, value):
        return slice(max(iy0, 0), min(iy1, self.h)), slice(max(ix0, 0), min(ix1, self.w))

    def disc(self, cx, cy, d, on=True):
        r = d / 2
        ix0, ix1 = int((cx - r - self.x0) / self.res) - 1, int((cx + r - self.x0) / self.res) + 2
        iy0, iy1 = int((cy - r - self.y0) / self.res) - 1, int((cy + r - self.y0) / self.res) + 2
        sy, sx = self._set(ix0, ix1, iy0, iy1, on)
        if sx.start >= sx.stop or sy.start >= sy.stop:
            return
        X = self.x0 + (np.arange(sx.start, sx.stop) + 0.5) * self.res
        Y = self.y0 + (np.arange(sy.start, sy.stop) + 0.5) * self.res
        hit = ((X[None, :] - cx) ** 2 + (Y[:, None] - cy) ** 2) <= r * r
        self.img[sy, sx] = (self.img[sy, sx] | hit) if on else (self.img[sy, sx] & ~hit)

    def rect(self, cx, cy, w, h, on=True):
        sy, sx = self._set(int((cx - w / 2 - self.x0) / self.res), int(math.ceil((cx + w / 2 - self.x0) / self.res)),
                           int((cy - h / 2 - self.y0) / self.res), int(math.ceil((cy + h / 2 - self.y0) / self.res)), on)
        if sx.start < sx.stop and sy.start < sy.stop:
            self.img[sy, sx] = on if on else False

    def capsule(self, ax, ay, bx, by, d, on=True):
        r = d / 2
        ix0, ix1 = int((min(ax, bx) - r - self.x0) / self.res) - 1, int((max(ax, bx) + r - self.x0) / self.res) + 2
        iy0, iy1 = int((min(ay, by) - r - self.y0) / self.res) - 1, int((max(ay, by) + r - self.y0) / self.res) + 2
        sy, sx = self._set(ix0, ix1, iy0, iy1, on)
        if sx.start >= sx.stop or sy.start >= sy.stop:
            return
        X = self.x0 + (np.arange(sx.start, sx.stop) + 0.5) * self.res
        Y = self.y0 + (np.arange(sy.start, sy.stop) + 0.5) * self.res
        PX, PY = np.meshgrid(X, Y)
        dx, dy = bx - ax, by - ay
        l2 = dx * dx + dy * dy
        t = np.clip(((PX - ax) * dx + (PY - ay) * dy) / l2, 0, 1) if l2 > 0 else np.zeros_like(PX)
        hit = ((PX - (ax + t * dx)) ** 2 + (PY - (ay + t * dy)) ** 2) <= r * r
        self.img[sy, sx] = (self.img[sy, sx] | hit) if on else (self.img[sy, sx] & ~hit)

    def polygon(self, pts, on=True):
        ys = [p[1] for p in pts]
        iy0 = max(int((min(ys) - self.y0) / self.res), 0)
        iy1 = min(int(math.ceil((max(ys) - self.y0) / self.res)), self.h)
        n = len(pts)
        for iy in range(iy0, iy1):
            yc = self.y0 + (iy + 0.5) * self.res
            xs = []
            for k in range(n):
                a, b = pts[k], pts[(k + 1) % n]
                if (a[1] > yc) != (b[1] > yc):
                    xs.append(a[0] + (yc - a[1]) / (b[1] - a[1]) * (b[0] - a[0]))
            xs.sort()
            for k in range(0, len(xs) - 1, 2):
                i0 = max(int(math.ceil((xs[k] - self.x0) / self.res - 0.5)), 0)
                i1 = min(int((xs[k + 1] - self.x0) / self.res - 0.5) + 1, self.w)
                if i1 > i0:
                    self.img[iy, i0:i1] = on

    def _aperture(self, x, y, ap, on):
        if ap is None:
            return
        if ap[0] == "C":
            self.disc(x, y, ap[1], on)
        elif ap[0] == "R":
            self.rect(x, y, ap[1], ap[2], on)
        elif ap[0] == "O":
            w, h = ap[1], ap[2]
            if w >= h:
                self.capsule(x - (w - h) / 2, y, x + (w - h) / 2, y, h, on)
            else:
                self.capsule(x, y - (h - w) / 2, x, y + (h - w) / 2, w, on)
        elif ap[0] == "M":
            for prim in ap[1]:
                code = int(prim[0])
                exposure = prim[1] == 1
                e = on if exposure else not on
                if code == 1:  # circle: exposure, diameter, cx, cy, [rotation]
                    rot = prim[5] if len(prim) > 5 else 0.0
                    cx, cy = _rot(prim[3], prim[4], rot)
                    self.disc(x + cx, y + cy, prim[2], e)
                elif code == 4:  # outline: exposure, n, x0, y0, ..., xn, yn, rotation
                    n = int(prim[2])
                    rot = prim[3 + 2 * (n + 1)] if len(prim) > 3 + 2 * (n + 1) else 0.0
                    pts = [_rot(prim[3 + 2 * k], prim[4 + 2 * k], rot) for k in range(n)]
                    self.polygon([(x + px, y + py) for px, py in pts], e)
                elif code == 20:  # vector line: exposure, width, x1, y1, x2, y2, rotation
                    rot = prim[7] if len(prim) > 7 else 0.0
                    ax, ay = _rot(prim[3], prim[4], rot)
                    bx, by = _rot(prim[5], prim[6], rot)
                    self.capsule(x + ax, y + ay, x + bx, y + by, prim[2], e)
                elif code in (21, 22):  # centre / lower-left rectangle: exposure, w, h, x, y, rotation
                    w, h, px, py = prim[2], prim[3], prim[4], prim[5]
                    rot = prim[6] if len(prim) > 6 else 0.0
                    if code == 22:
                        px, py = px + w / 2, py + h / 2
                    corners = [(px + sx * w / 2, py + sy * h / 2) for sx, sy in ((-1, -1), (1, -1), (1, 1), (-1, 1))]
                    self.polygon([(x + _rot(cx, cy, rot)[0], y + _rot(cx, cy, rot)[1]) for cx, cy in corners], e)

    def render(self, ops):
        for op in ops:
            if op[0] == "region":
                self.polygon(op[1], not op[2])
            elif op[0] == "stroke":
                ap = op[2]
                for a, b in zip(op[1][:-1], op[1][1:]):
                    if ap and ap[0] in ("R", "O"):  # a non-round brush: stamp it along the segment
                        n = max(1, int(math.hypot(b[0] - a[0], b[1] - a[1]) / (self.res / 2)))
                        for k in range(n + 1):
                            self._aperture(a[0] + (b[0] - a[0]) * k / n, a[1] + (b[1] - a[1]) * k / n, ap, not op[3])
                    else:
                        self.capsule(a[0], a[1], b[0], b[1], ap[1] if ap and ap[0] == "C" else 0.1, not op[3])
            elif op[0] == "flash":
                self._aperture(op[1], op[2], op[3], not op[4])
        return self.img

    def fill_box(self, box, on=True):
        x0, y0, x1, y1 = box
        sy, sx = self._set(int((x0 - self.x0) / self.res), int((x1 - self.x0) / self.res) + 1,
                           int((y0 - self.y0) / self.res), int((y1 - self.y0) / self.res) + 1, on)
        self.img[sy, sx] = on


def first_closed_loop(ops):
    """The first run of strokes that returns to its start (a board outline drawn as a polyline), as points in mm."""
    pts = []
    for op in ops:
        if op[0] != "stroke":
            continue
        seg = op[1]
        if pts and math.hypot(seg[0][0] - pts[-1][0], seg[0][1] - pts[-1][1]) > 1e-6:
            pts = []
        if not pts:
            pts = [seg[0]]
        pts.extend(seg[1:])
        if len(pts) > 3 and math.hypot(pts[-1][0] - pts[0][0], pts[-1][1] - pts[0][1]) < 1e-6:
            return pts[:-1]
    raise GerberError("no closed loop of strokes found")


def excellon(path, int_digits=2, dec_digits=3, units="inch", zeros="trailing"):
    """Hole positions (tool, x_mm, y_mm) and tool diameters in mm (empty when the file defines none).
    zeros: 'trailing' (trailing zeros omitted: the digits are padded on the right) or 'leading'."""
    to_mm = MM_PER_INCH if units == "inch" else 1.0
    holes, diameters = [], {}
    tool = None
    x = y = 0.0
    total = int_digits + dec_digits

    def value(s):
        neg = s.startswith("-")
        s = s.lstrip("-+")
        if "." in s:
            return (-1 if neg else 1) * float(s)
        s = s.ljust(total, "0") if zeros == "trailing" else s.rjust(total, "0")
        return (-1 if neg else 1) * int(s) / 10.0 ** dec_digits

    for line in open(path, "rb").read().decode("latin1").splitlines():
        line = line.strip()
        m = re.match(r"^T(\d+)C([\d.]+)$", line)
        if m:
            diameters[int(m.group(1))] = float(m.group(2)) * to_mm
            continue
        m = re.match(r"^T(\d+)$", line)
        if m:
            tool = int(m.group(1))
            continue
        if line.startswith(("X", "Y")):
            mx, my = re.search(r"X(-?[\d.]+)", line), re.search(r"Y(-?[\d.]+)", line)
            if mx:
                x = value(mx.group(1))
            if my:
                y = value(my.group(1))
            holes.append((tool, x * to_mm, y * to_mm))
    return holes, diameters


def write_png(path, mask):
    """A black-on-white PNG of a boolean raster (y up), for looking at a layer."""
    h, w = mask.shape
    raw = b"".join(b"\x00" + (255 - 255 * mask[h - 1 - y]).astype(np.uint8).tobytes() for y in range(h))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 0, 0, 0, 0))
                           + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
