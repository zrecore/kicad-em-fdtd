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
