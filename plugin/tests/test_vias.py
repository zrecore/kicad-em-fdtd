import types

import pytest
from pytest_bdd import given, parsers, scenarios, then, when

from emfdtd_plugin import kicad_reader

scenarios("features/vias.feature")

NM = 1_000_000


def _via(drill):
    return types.SimpleNamespace(
        position=types.SimpleNamespace(x=5 * NM, y=5 * NM),
        diameter=300_000,
        drill_diameter=100_000,
        net=types.SimpleNamespace(name="GND"),
        padstack=types.SimpleNamespace(drill=drill),
    )


class _Board:
    def __init__(self, via):
        self._via = via

    def get_vias(self):
        return [self._via]


@pytest.fixture
def context(monkeypatch):
    # the enum values are the layer names themselves in these scenarios
    monkeypatch.setattr(kicad_reader, "layer_name", lambda v: v)
    return {}


@given(parsers.parse("a via from {a} to {b}"))
def _given_span(context, a, b):
    context["board"] = _Board(_via(types.SimpleNamespace(start_layer=a, end_layer=b)))


@given("a via whose drill layers cannot be read")
def _given_nodrill(context):
    context["board"] = _Board(_via(None))


@when("the vias are read")
def _when_read(context):
    context["vias"] = kicad_reader.read_vias(context["board"])


@then("the via has no layer span")
def _then_none(context):
    assert "from" not in context["vias"][0] and "to" not in context["vias"][0]


@then(parsers.parse("the via spans from {a} to {b}"))
def _then_span(context, a, b):
    assert (context["vias"][0]["from"], context["vias"][0]["to"]) == (a, b)
