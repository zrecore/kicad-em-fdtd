import json

import pytest
from pytest_bdd import given, parsers, scenarios, then, when

from emfdtd_plugin.farm.worker import Worker
from farm_stub.paths import STUB

scenarios("features/farm_speed.feature")

RTX = "u-rtx"


@pytest.fixture
def ctx(tmp_path):
    return {"root": tmp_path / "w"}


def _worker(ctx):
    ctx["root"].mkdir(parents=True, exist_ok=True)
    w = Worker({}, "127.0.0.1", 1, STUB, ctx["root"], [], no_lock=True)
    w._load_speed()
    ctx["w"] = w
    return w


@given(parsers.parse("a worker whose RTX has a recorded speed of {m:d} Mcells/s"))
def _known(ctx, m):
    w = _worker(ctx)
    w._note_speed(RTX, float(m), 1900.0)


@given("a worker with no recorded speed")
def _none(ctx):
    _worker(ctx)
    ctx["w"]._speed.clear()
    ctx["w"]._rates.clear()


@given(parsers.parse("a speed file in the old format with {m:d} Mcells/s for the RTX"))
def _old(ctx, m):
    ctx["root"].mkdir(parents=True, exist_ok=True)
    (ctx["root"] / "speed.json").write_text(json.dumps({RTX: float(m)}))
    _worker(ctx)


@when(parsers.parse("a resumed run reports {m:d} Mcells/s after {s:d} seconds"))
def _resumed(ctx, m, s):
    ctx["w"]._note_speed(RTX, float(m), float(s), resumed=True)


@when(parsers.parse("a run reports {m:d} Mcells/s after {s:d} seconds"))
def _run(ctx, m, s):
    ctx["w"]._note_speed(RTX, float(m), float(s))


@then(parsers.parse("the recorded speed is within {pct:g} percent of {m:d} Mcells/s"))
def _speed(ctx, pct, m):
    got = ctx["w"]._speed[RTX]
    assert abs(got - m) <= m * pct / 100.0, f"{got} is not within {pct} % of {m}"
