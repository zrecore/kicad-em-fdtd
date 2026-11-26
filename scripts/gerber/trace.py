"""Boolean raster -> polygons (outer rings and holes) by pixel-edge chaining, with collinear merging and
Douglas-Peucker simplification. Pure python + numpy. Pixel (ix, iy) covers [ix, ix+1] x [iy, iy+1] (y up)."""
import math
import numpy as np

def boundary_loops(mask):
    h, w = mask.shape
    m = np.pad(mask, 1)  # False border
    ed = {}
    def add(a, b):
        ed.setdefault(a, []).append(b)
    ys, xs = np.nonzero(mask)
    for iy, ix in zip(ys.tolist(), xs.tolist()):
        py, px = iy + 1, ix + 1
        if not m[py - 1, px]: add((ix, iy), (ix + 1, iy))
        if not m[py, px + 1]: add((ix + 1, iy), (ix + 1, iy + 1))
        if not m[py + 1, px]: add((ix + 1, iy + 1), (ix, iy + 1))
        if not m[py, px - 1]: add((ix, iy + 1), (ix, iy))
    loops = []
    while ed:
        start = next(iter(ed))
        loop = [start]
        cur = start
        while True:
            nxt = ed[cur]
            n = nxt.pop()
            if not nxt: del ed[cur]
            if n == start:
                break
            loop.append(n)
            cur = n
            if cur not in ed:  # should not happen
                break
        loops.append(loop)
    return loops

def merge_collinear(pts):
    out = []
    n = len(pts)
    for i in range(n):
        a, b, c = pts[i - 1], pts[i], pts[(i + 1) % n]
        if (b[0] - a[0]) * (c[1] - b[1]) - (b[1] - a[1]) * (c[0] - b[0]) != 0:
            out.append(b)
    return out

def dp(pts, eps):
    if len(pts) < 5: return pts
    def rdp(p, lo, hi, keep):
        best, idx = 0.0, -1
        ax, ay = p[lo]; bx, by = p[hi]
        dx, dy = bx - ax, by - ay; L = math.hypot(dx, dy) or 1e-12
        for i in range(lo + 1, hi):
            d = abs((p[i][0] - ax) * dy - (p[i][1] - ay) * dx) / L
            if d > best: best, idx = d, i
        if best > eps:
            keep[idx] = True
            rdp(p, lo, idx, keep); rdp(p, idx, hi, keep)
    # split the ring at the two farthest-apart points to keep it closed
    n = len(pts)
    i0 = 0
    i1 = max(range(n), key=lambda i: (pts[i][0] - pts[0][0]) ** 2 + (pts[i][1] - pts[0][1]) ** 2)
    a = pts[i0:i1 + 1]; b = pts[i1:] + [pts[0]]
    out = []
    for seg in (a, b):
        keep = [False] * len(seg); keep[0] = keep[-1] = True
        rdp(seg, 0, len(seg) - 1, keep)
        out += [p for p, k in zip(seg, keep) if k][:-1]
    return out

def area(r):
    return sum(r[i][0] * r[(i + 1) % len(r)][1] - r[(i + 1) % len(r)][0] * r[i][1] for i in range(len(r))) / 2

def inside(r, x, y):
    c = False
    for a in range(len(r)):
        b = a - 1
        if (r[a][1] > y) != (r[b][1] > y) and x < (r[b][0] - r[a][0]) * (y - r[a][1]) / (r[b][1] - r[a][1]) + r[a][0]:
            c = not c
    return c

def polygons(mask, res, x0, y0, eps_px=0.6, min_area_mm2=1.0, min_hole_mm2=0.25):
    """Returns [(outer, [holes])] in mm, outer CCW. Rings are lists of (x, y) mm."""
    loops = boundary_loops(mask)
    outers, holes = [], []
    for lp in loops:
        lp = merge_collinear(lp)
        if len(lp) < 3: continue
        lp = dp(lp, eps_px)
        if len(lp) < 3: continue
        a = area(lp)
        (outers if a > 0 else holes).append(lp)
    out = []
    px2mm2 = res * res
    for o in outers:
        if area(o) * px2mm2 < min_area_mm2: continue
        out.append([o, []])
    for hole in holes:
        if -area(hole) * px2mm2 < min_hole_mm2: continue
        hx, hy = hole[0]
        cand = [e for e in out if inside(e[0], hx + 1e-3, hy + 1e-3)]
        if cand:
            min(cand, key=lambda e: area(e[0]))[1].append(hole)
    conv = lambda r: [(x0 + p[0] * res, y0 + p[1] * res) for p in r]
    return [(conv(o), [conv(h) for h in hs]) for o, hs in out]
