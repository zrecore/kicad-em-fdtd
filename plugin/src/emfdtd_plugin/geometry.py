"""Plain-Python geometry helpers for the snapshot (metres, no KiCAD imports, so they test anywhere)."""

import math


def segment_ring(x1, y1, x2, y2, width, cap_steps=6):
    """A straight track as a stadium polygon: a rectangle with round end caps, counter-clockwise."""
    r = width / 2
    if x1 == x2 and y1 == y2:
        return circle_ring(x1, y1, r, 4 * cap_steps)
    a = math.atan2(y2 - y1, x2 - x1)
    ring = []
    for i in range(cap_steps + 1):  # cap around the end point, from -90 to +90 degrees about the direction
        t = a - math.pi / 2 + math.pi * i / cap_steps
        ring.append([x2 + r * math.cos(t), y2 + r * math.sin(t)])
    for i in range(cap_steps + 1):
        t = a + math.pi / 2 + math.pi * i / cap_steps
        ring.append([x1 + r * math.cos(t), y1 + r * math.sin(t)])
    return ring


def circle_ring(cx, cy, r, steps=24):
    return [
        [cx + r * math.cos(2 * math.pi * i / steps), cy + r * math.sin(2 * math.pi * i / steps)]
        for i in range(steps)
    ]


def arc_points(start, mid, end, max_step_rad=math.pi / 12):
    """Points along the circular arc through three points (start, mid, end), inclusive."""
    (x1, y1), (x2, y2), (x3, y3) = start, mid, end
    d = 2 * (x1 * (y2 - y3) + x2 * (y3 - y1) + x3 * (y1 - y2))
    if abs(d) < 1e-30:  # collinear: a straight segment
        return [list(start), list(end)]
    ux = ((x1**2 + y1**2) * (y2 - y3) + (x2**2 + y2**2) * (y3 - y1) + (x3**2 + y3**2) * (y1 - y2)) / d
    uy = ((x1**2 + y1**2) * (x3 - x2) + (x2**2 + y2**2) * (x1 - x3) + (x3**2 + y3**2) * (x2 - x1)) / d
    a1, a2, a3 = (math.atan2(y - uy, x - ux) for (x, y) in (start, mid, end))
    # choose the sweep direction that passes through mid
    sweep = (a3 - a1) % (2 * math.pi)
    if ((a2 - a1) % (2 * math.pi)) > sweep:
        sweep -= 2 * math.pi
    n = max(2, int(math.ceil(abs(sweep) / max_step_rad)) + 1)
    r = math.hypot(x1 - ux, y1 - uy)
    return [
        [ux + r * math.cos(a1 + sweep * i / (n - 1)), uy + r * math.sin(a1 + sweep * i / (n - 1))]
        for i in range(n)
    ]


def ring_area(ring):
    """Signed shoelace area (positive for counter-clockwise)."""
    return 0.5 * sum(
        ring[i][0] * ring[(i + 1) % len(ring)][1] - ring[(i + 1) % len(ring)][0] * ring[i][1]
        for i in range(len(ring))
    )


def bbox(ring):
    xs = [p[0] for p in ring]
    ys = [p[1] for p in ring]
    return (min(xs), min(ys), max(xs), max(ys))


def port_between(box_a, box_b, layer, name="P1", impedance=50.0):
    """A port across the gap between two pads on one layer, from pad A (positive) to pad B (negative).

    Boxes are (x0, y0, x1, y1) in metres. The port runs along the axis with the larger centre distance,
    from the facing edge of A to the facing edge of B, as wide as the pads overlap across the gap.
    """
    cax, cay = (box_a[0] + box_a[2]) / 2, (box_a[1] + box_a[3]) / 2
    cbx, cby = (box_b[0] + box_b[2]) / 2, (box_b[1] + box_b[3]) / 2
    along_x = abs(cbx - cax) >= abs(cby - cay)
    i0, i1 = (0, 2) if along_x else (1, 3)  # indices of the along-gap coordinates in a box
    t0, t1 = (1, 3) if along_x else (0, 2)  # and of the across-gap coordinates
    a_first = box_a[i0] <= box_b[i0]
    start = box_a[i1] if a_first else box_a[i0]
    end = box_b[i0] if a_first else box_b[i1]
    if (end - start) * (1 if a_first else -1) <= 0:
        raise ValueError("the two pads overlap or touch along the gap direction: there is no gap for a port")
    lo, hi = max(box_a[t0], box_b[t0]), min(box_a[t1], box_b[t1])
    if hi <= lo:  # not facing each other: use the narrower pad's full width, centred on its own extent
        lo, hi = (
            (box_a[t0], box_a[t1])
            if box_a[t1] - box_a[t0] <= box_b[t1] - box_b[t0]
            else (box_b[t0], box_b[t1])
        )
    centre = (lo + hi) / 2
    a = [start, centre] if along_x else [centre, start]
    b = [end, centre] if along_x else [centre, end]
    return {"name": name, "layer": layer, "a": a, "b": b, "width": hi - lo, "impedance": impedance}
