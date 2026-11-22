"""Board outline and cutouts from the shapes on Edge.Cuts (pure geometry, kipy shapes converted elsewhere).

Items (coordinates in nm, any consistent unit works): ("line", p0, p1), ("arc", start, mid, end),
("circle", centre, radius), ("rect", p0, p1), ("poly", [points]), ("bezier", p0, c1, c2, p1). Open pieces are
chained end to end into closed loops; the loop of largest area is the board and loops inside it are cutouts
(routed slots, openings).
"""

import math

ARC_STEP_DEG = 5.0
BEZIER_STEPS = 12


def _key(p):
    return (
        round(p[0] / 1000.0),
        round(p[1] / 1000.0),
    )  # pieces meeting within 1 micrometre (in nm) are joined


def arc_points(a, m, b):
    """Points along the arc from a through m to b (inclusive), at most ARC_STEP_DEG apart."""
    (x1, y1), (x2, y2), (x3, y3) = a, m, b
    d = 2 * (x1 * (y2 - y3) + x2 * (y3 - y1) + x3 * (y1 - y2))
    if d == 0:
        return [a, b]
    s1, s2, s3 = x1 * x1 + y1 * y1, x2 * x2 + y2 * y2, x3 * x3 + y3 * y3
    ux = (s1 * (y2 - y3) + s2 * (y3 - y1) + s3 * (y1 - y2)) / d
    uy = (s1 * (x3 - x2) + s2 * (x1 - x3) + s3 * (x2 - x1)) / d
    r = math.hypot(x1 - ux, y1 - uy)
    a1, a2, a3 = (math.atan2(y - uy, x - ux) for x, y in (a, m, b))
    two_pi = 2 * math.pi
    ccw = (a2 - a1) % two_pi < (a3 - a1) % two_pi
    span = (a3 - a1) % two_pi if ccw else -((a1 - a3) % two_pi)
    n = max(2, int(math.ceil(abs(math.degrees(span)) / ARC_STEP_DEG)))
    pts = [(ux + r * math.cos(a1 + span * i / n), uy + r * math.sin(a1 + span * i / n)) for i in range(n + 1)]
    pts[0], pts[-1] = a, b
    return pts


def _pieces(items):
    """(points, closed) for each item."""
    out = []
    for item in items:
        kind = item[0]
        if kind == "line":
            out.append(([item[1], item[2]], False))
        elif kind == "arc":
            out.append((arc_points(item[1], item[2], item[3]), False))
        elif kind == "bezier":
            p0, c1, c2, p1 = item[1:5]
            pts = []
            for i in range(BEZIER_STEPS + 1):
                t = i / BEZIER_STEPS
                w = ((1 - t) ** 3, 3 * t * (1 - t) ** 2, 3 * t * t * (1 - t), t**3)
                pts.append(
                    (
                        sum(c * p[0] for c, p in zip(w, (p0, c1, c2, p1))),
                        sum(c * p[1] for c, p in zip(w, (p0, c1, c2, p1))),
                    )
                )
            out.append((pts, False))
        elif kind == "circle":
            (cx, cy), r = item[1], item[2]
            n = int(360 / ARC_STEP_DEG)
            out.append(
                (
                    [
                        (cx + r * math.cos(2 * math.pi * i / n), cy + r * math.sin(2 * math.pi * i / n))
                        for i in range(n)
                    ],
                    True,
                )
            )
        elif kind == "rect":
            (x0, y0), (x1, y1) = item[1], item[2]
            out.append(([(x0, y0), (x1, y0), (x1, y1), (x0, y1)], True))
        elif kind == "poly":
            out.append((list(item[1]), True))
    return out


def loops(items):
    """Closed loops (lists of points, no repeated last point) and the number of pieces left open."""
    closed = [pts for pts, is_closed in _pieces(items) if is_closed]
    open_pieces = [pts for pts, is_closed in _pieces(items) if not is_closed]
    used = [False] * len(open_pieces)
    unclosed = 0
    for i in range(len(open_pieces)):
        if used[i]:
            continue
        used[i] = True
        chain = list(open_pieces[i])
        progress = True
        while progress and _key(chain[0]) != _key(chain[-1]):
            progress = False
            for j in range(len(open_pieces)):
                if used[j]:
                    continue
                piece = open_pieces[j]
                if _key(piece[0]) == _key(chain[-1]):
                    chain += piece[1:]
                elif _key(piece[-1]) == _key(chain[-1]):
                    chain += piece[::-1][1:]
                elif _key(piece[-1]) == _key(chain[0]):
                    chain = piece[:-1] + chain
                elif _key(piece[0]) == _key(chain[0]):
                    chain = piece[::-1][:-1] + chain
                else:
                    continue
                used[j] = True
                progress = True
                break
        if _key(chain[0]) == _key(chain[-1]) and len(chain) > 3:
            closed.append(chain[:-1])
        else:
            unclosed += 1
    return closed, unclosed


def area(ring):
    return (
        abs(
            sum(
                ring[i][0] * ring[(i + 1) % len(ring)][1] - ring[(i + 1) % len(ring)][0] * ring[i][1]
                for i in range(len(ring))
            )
        )
        / 2
    )


def inside(ring, p):
    x, y = p
    hit = False
    for i in range(len(ring)):
        (xa, ya), (xb, yb) = ring[i], ring[(i + 1) % len(ring)]
        if (ya > y) != (yb > y) and x < (xb - xa) * (y - ya) / (yb - ya) + xa:
            hit = not hit
    return hit


def board_and_cutouts(items):
    """(outline ring, [cutout rings], notes). Raises ValueError when there is no closed loop at all."""
    rings, unclosed = loops(items)
    notes = []
    if not rings:
        raise ValueError("Edge.Cuts has no closed outline")
    rings.sort(key=area, reverse=True)
    outer = rings[0]
    cuts = [r for r in rings[1:] if inside(outer, r[0])]
    stray = len(rings) - 1 - len(cuts)
    if stray:
        notes.append(f"{stray} other closed outline(s) outside the board are not modelled")
    if unclosed:
        notes.append(f"{unclosed} Edge.Cuts piece(s) do not close into a loop and are ignored")
    return outer, cuts, notes


# Panel frames and breakaway tabs: the board proper is the face, among the slots' rings and the bridges across
# the tabs, that holds the seed point (see "Excluding breakaway tabs" in docs/PLAN.md). Distances in mm.
SLOT_MAX_WIDTH_MM = 3.0
SLOT_MIN_ASPECT = 4.0
TAB_MAX_MM = 6.0
BOARD_MIN_SHARE = 0.2  # a face smaller than this share of the outer loop is not taken for the board


def _slot_axes(ring):
    """(length, width, unit direction along the length, centre) from the covariance of the ring's points."""
    n = len(ring)
    cx, cy = sum(p[0] for p in ring) / n, sum(p[1] for p in ring) / n
    sxx = sum((p[0] - cx) ** 2 for p in ring)
    syy = sum((p[1] - cy) ** 2 for p in ring)
    sxy = sum((p[0] - cx) * (p[1] - cy) for p in ring)
    ang = 0.5 * math.atan2(2 * sxy, sxx - syy)
    d = (math.cos(ang), math.sin(ang))
    along = [(p[0] - cx) * d[0] + (p[1] - cy) * d[1] for p in ring]
    across = [-(p[0] - cx) * d[1] + (p[1] - cy) * d[0] for p in ring]
    return max(along) - min(along), max(across) - min(across), d, (cx, cy)


def _slot_corners(ring, d, centre, seed):
    """The two end corners of a slot on the seed's side: for each end, the extreme point across the slot
    that is nearer the seed. Returns [corner of the first end, corner of the other end]."""
    along = [(p[0] - centre[0]) * d[0] + (p[1] - centre[1]) * d[1] for p in ring]
    across = [-(p[0] - centre[0]) * d[1] + (p[1] - centre[1]) * d[0] for p in ring]
    width = max(across) - min(across)
    corners = []
    for end in (min(along), max(along)):
        cap = [i for i in range(len(ring)) if abs(along[i] - end) <= width]
        lo = min(cap, key=lambda i: across[i])
        hi = max(cap, key=lambda i: across[i])
        near = min((lo, hi), key=lambda i: math.hypot(ring[i][0] - seed[0], ring[i][1] - seed[1]))
        corners.append(ring[near])
    return corners


def board_proper(outer, cuts, seed, mm=1e6):
    """Remove a panel frame: (board ring, remaining cutouts, notes) or None when the loops form no panel.

    A slot is a thin, long cutout. Each slot end is bridged to the nearest end of another slot within a tab's
    reach by the segment between their corners on the seed's side; the board is the face of the slots'
    edges and these bridges that holds `seed` (coordinates in the unit where 1 mm is `mm`)."""
    slots = []
    for i, ring in enumerate(cuts):
        length, width, d, centre = _slot_axes(ring)
        if width <= SLOT_MAX_WIDTH_MM * mm and length >= SLOT_MIN_ASPECT * width:
            slots.append((i, ring, d, centre))
    if len(slots) < 2:
        return None
    ends = []  # (slot index in `slots`, corner)
    for k, (_, ring, d, centre) in enumerate(slots):
        for corner in _slot_corners(ring, d, centre, seed):
            ends.append((k, corner))

    def nearest(a):
        """The closest end on another slot within a tab's reach, or None."""
        ka, pa = ends[a]
        best = None
        for b, (kb, pb) in enumerate(ends):
            dist = math.hypot(pa[0] - pb[0], pa[1] - pb[1])
            if kb != ka and dist <= TAB_MAX_MM * mm and (best is None or dist < best[0]):
                best = (dist, b)
        return best[1] if best else None

    bridges = {tuple(sorted((a, nearest(a)))) for a in range(len(ends)) if nearest(a) is not None}
    bridges = {(a, b) for a, b in bridges if nearest(a) == b and nearest(b) == a}  # mutual nearest only
    # planar graph: nodes are points, edges are slot ring segments and bridges
    index, pts, adj = {}, [], {}

    def node(p):
        key = (round(p[0]), round(p[1]))
        if key not in index:
            index[key] = len(pts)
            pts.append(p)
            adj[index[key]] = set()
        return index[key]

    def link(p, q):
        u, v = node(p), node(q)
        if u != v:
            adj[u].add(v)
            adj[v].add(u)

    for _, ring, _, _ in slots:
        for i in range(len(ring)):
            link(ring[i], ring[(i + 1) % len(ring)])
    for a, b in bridges:
        link(ends[a][1], ends[b][1])
    if len(bridges) < 2:
        return None
    # the first edge the +x ray from the seed meets, oriented upward, has the seed's face on its left
    hit = None
    for u in adj:
        for v in adj[u]:
            (xa, ya), (xb, yb) = pts[u], pts[v]
            if ya < yb and ya <= seed[1] < yb:
                x = xa + (seed[1] - ya) * (xb - xa) / (yb - ya)
                if x > seed[0] and (hit is None or x < hit[0]):
                    hit = (x, u, v)
    if hit is None:
        return None
    start = (hit[1], hit[2])
    walk, edge = [], start
    for _ in range(2 * sum(len(v) for v in adj.values()) + 2):
        walk.append(edge[0])
        u, v = edge
        back = math.atan2(pts[u][1] - pts[v][1], pts[u][0] - pts[v][0])

        def turn(w, v=v, back=back, u=u):
            if w == u:
                return 2 * math.pi
            ang = math.atan2(pts[w][1] - pts[v][1], pts[w][0] - pts[v][0])
            return (back - ang) % (2 * math.pi) or 2 * math.pi

        edge = (v, min(adj[v], key=turn))
        if edge == start:
            break
    else:
        return None
    ring = [pts[i] for i in walk]
    if not inside(ring, seed) or area(ring) < BOARD_MIN_SHARE * area(outer):
        return None
    slot_ids = {id(s[1]) for s in slots}
    rest = [c for c in cuts if id(c) not in slot_ids and inside(ring, c[0])]
    note = (
        f"panel frame excluded: board proper {area(ring) / mm**2:.0f} mm2 of the "
        f"{area(outer) / mm**2:.0f} mm2 drawn, {len(slots)} slots and {len(bridges)} tabs bridged "
        "(slot-end bridging)"
    )
    return ring, rest, [note]


def keeps(ring, polygon):
    """True when the polygon is on the board: a vertex of it is inside the ring, or the ring is inside it."""
    return any(inside(ring, p) for p in polygon) or inside(polygon, ring[0])
