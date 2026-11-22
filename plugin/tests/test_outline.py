import math

import pytest
from pytest_bdd import given, parsers, scenarios, then, when

from emfdtd_plugin import outline

scenarios("features/outline.feature")

MM = 1_000_000.0


def _rect_lines(w=20, h=10):
    p = [(0, 0), (w * MM, 0), (w * MM, h * MM), (0, h * MM)]
    return [("line", p[i], p[(i + 1) % 4]) for i in range(4)]


@pytest.fixture
def context():
    return {}


@given("Edge.Cuts with a 20 mm by 10 mm rectangle drawn as four lines")
def _g_rect(context):
    context["items"] = _rect_lines()


@given("Edge.Cuts with a 20 mm by 10 mm rectangle whose top right corner is an arc of radius 3 mm")
def _g_round(context):
    r = 3 * MM
    w, h = 20 * MM, 10 * MM
    c = (w - r, h - r)
    mid = (c[0] + r * math.cos(math.radians(45)), c[1] + r * math.sin(math.radians(45)))
    context["items"] = [
        ("line", (0, 0), (w, 0)),
        ("line", (w, 0), (w, h - r)),
        ("arc", (w, h - r), mid, (w - r, h)),
        ("line", (w - r, h), (0, h)),
        ("line", (0, h), (0, 0)),
    ]


@given("Edge.Cuts with a 20 mm by 10 mm rectangle and a circle of radius 2 mm inside it")
def _g_circle(context):
    context["items"] = _rect_lines() + [("circle", (10 * MM, 5 * MM), 2 * MM)]


@given("Edge.Cuts with a 20 mm by 10 mm rectangle and a stray line")
def _g_stray(context):
    context["items"] = _rect_lines() + [("line", (30 * MM, 0), (31 * MM, 5 * MM))]


@when("the outline is read")
def _w_read(context):
    context["outer"], context["cuts"], context["notes"] = outline.board_and_cutouts(context["items"])


@then(parsers.parse("the board is a ring of {n:d} points with an area of {a:d} square mm"))
def _t_ring(context, n, a):
    assert len(context["outer"]) == n
    assert outline.area(context["outer"]) / MM**2 == pytest.approx(a)


@then("there are no cutouts")
def _t_nocuts(context):
    assert context["cuts"] == []


@then(parsers.parse("the board area is less than 200 square mm by about {d:f} square mm"))
def _t_round(context, d):
    assert 200 - outline.area(context["outer"]) / MM**2 == pytest.approx(d, abs=0.03)


@then("the board is the rectangle")
def _t_rect(context):
    assert outline.area(context["outer"]) / MM**2 == pytest.approx(200)


@then(parsers.parse("there is {n:d} cutout of area about {a:f} square mm"))
def _t_cut(context, n, a):
    assert len(context["cuts"]) == n
    assert outline.area(context["cuts"][0]) / MM**2 == pytest.approx(a, rel=0.01)


@then(parsers.parse("the notes mention {n:d} piece that does not close"))
def _t_notes(context, n):
    assert any(f"{n} Edge.Cuts piece" in note for note in context["notes"])
