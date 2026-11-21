import types

import pytest
from pytest_bdd import given, parsers, scenarios, then, when

from emfdtd_plugin import kicad_reader

scenarios("features/components.feature")


def _fp(ref, x_mm, y_mm):
    nm = 1_000_000
    return types.SimpleNamespace(
        reference_field=types.SimpleNamespace(text=types.SimpleNamespace(value=ref)),
        value_field=types.SimpleNamespace(text=types.SimpleNamespace(value="v")),
        layer=0,
        position=types.SimpleNamespace(x=int(x_mm * nm), y=int(y_mm * nm)),
    )


class _Board:
    def __init__(self, fp, box):
        self._fp, self._box = fp, box

    def get_footprints(self):
        return [self._fp]

    def get_item_bounding_box(self, _item):
        if self._box is None:
            raise RuntimeError("no box")
        return self._box


@pytest.fixture
def context(monkeypatch):
    monkeypatch.setattr(kicad_reader, "layer_name", lambda _v: "F.Cu")
    return {"notes": []}


@given(
    parsers.parse(
        "a footprint U1 at 5 mm, 5 mm whose bounding box is 3 mm wide and 2 mm tall starting at 3.5 mm, 4 mm"
    )
)
def _given_box(context):
    nm = 1_000_000
    box = types.SimpleNamespace(
        pos=types.SimpleNamespace(x=int(3.5 * nm), y=4 * nm),
        size=types.SimpleNamespace(x=3 * nm, y=2 * nm),
    )
    context["board"] = _Board(_fp("U1", 5, 5), box)


@given("a footprint U2 whose bounding box cannot be read")
def _given_nobox(context):
    context["board"] = _Board(_fp("U2", 1, 1), None)


@when("the components are read")
def _when_read(context):
    context["out"] = kicad_reader.read_components(context["board"], context["notes"])


@then(parsers.parse("U1 has a body polygon with corners {a} and {b} in metres"))
def _then_polygon(context, a, b):
    poly = context["out"][0]["body"]["polygon"]
    lo = [float(v) for v in a.split(",")]
    hi = [float(v) for v in b.split(",")]
    assert poly[0] == pytest.approx(lo, abs=1e-9)
    assert poly[2] == pytest.approx(hi, abs=1e-9)


@then("the notes say that heights are not available from KiCAD")
def _then_notes(context):
    assert any("heights are not available" in n for n in context["notes"])


@then("U2 is exported without a body")
def _then_nobody(context):
    assert "body" not in context["out"][0]


class _Pad:
    def __init__(self, parent, number, net, x_mm, y_mm):
        nm = 1_000_000
        self.parent = types.SimpleNamespace(value=parent)
        self.number = number
        self.net = types.SimpleNamespace(name=net)
        self.position = types.SimpleNamespace(x=int(x_mm * nm), y=int(y_mm * nm))


class _PadBoard(_Board):
    def __init__(self, fp, pads):
        super().__init__(fp, None)
        self._pads = pads

    def get_pads(self):
        return self._pads


@given(
    parsers.parse(
        "a footprint R5 marked do not populate with pads 1 on net FEED at 2 mm, 3 mm "
        "and 2 on net ANT at 3 mm, 3 mm"
    )
)
def _given_dnp(context):
    fp = _fp("R5", 2.5, 3)
    fp.id = types.SimpleNamespace(value="id-r5")
    fp.attributes = types.SimpleNamespace(do_not_populate=True, mounting_style=2)  # FMS_SMD
    pads = [
        _Pad("id-r5", "1", "FEED", 2, 3),
        _Pad("id-r5", "2", "ANT", 3, 3),
        _Pad("id-other", "1", "GND", 9, 9),
    ]
    context["board"] = _PadBoard(fp, pads)


@when("the components are read with their pads")
def _when_read_pads(context):
    context["out"] = kicad_reader.read_components(context["board"], context["notes"])


@then("R5 is marked dnp and surface-mounted")
def _then_dnp(context):
    a = context["out"][0]["attributes"]
    assert a["dnp"] is True
    assert a["mounting"] == "smd"


@then(parsers.parse("R5 has 2 pads numbered 1 and 2 on nets FEED and ANT, with pad 1 at 0.002,0.003 metres"))
def _then_pads(context):
    pads = context["out"][0]["pads"]
    assert [p["number"] for p in pads] == ["1", "2"]
    assert [p["net"] for p in pads] == ["FEED", "ANT"]
    assert (pads[0]["x"], pads[0]["y"]) == pytest.approx((0.002, 0.003), abs=1e-9)


@then("a pad belongs to the footprint it sits in, not to another one")
def _then_owner(context):
    assert all(p["net"] != "GND" for p in context["out"][0]["pads"])
