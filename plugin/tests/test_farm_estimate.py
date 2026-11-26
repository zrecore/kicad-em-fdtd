import json
import stat
from pathlib import Path

import pytest
from pytest_bdd import given, parsers, scenarios, then, when

from emfdtd_plugin.farm import estimate

scenarios("features/farm_estimate.feature")


@pytest.fixture
def ctx(tmp_path, monkeypatch):
    monkeypatch.setenv("EMFDTD_FARM_HOME", str(tmp_path / "home"))
    return {"tmp": tmp_path}


def _solver(ctx, body):
    path = ctx["tmp"] / "solver.py"
    path.write_text("#!/usr/bin/env python3\nimport json, sys\n" + body)
    path.chmod(path.stat().st_mode | stat.S_IXUSR)
    ctx["solver"] = str(path)
    ctx["calls"] = ctx["tmp"] / "calls.json"


@given(parsers.parse("a solver whose dry run says {cells:d} cells and an upper bound of {steps:d} steps"))
def _good(ctx, cells, steps):
    _solver(
        ctx,
        f"open({str(ctx['tmp'] / 'calls.json')!r}, 'w').write(json.dumps(sys.argv[1:]))\n"
        f"print(json.dumps({{'schema': 1, 'mode': 'dry_run', 'grid': {{'cell_count': {cells}}},"
        f" 'estimate': {{'steps': {steps}}}, 'backend': 'cpu'}}))\n",
    )


@given("a solver that fails its dry run")
def _bad(ctx):
    _solver(ctx, "print('boom', file=sys.stderr); sys.exit(3)\n")


@when("the client estimates a job with the default stop threshold")
def _default(ctx):
    ctx["result"] = estimate.estimate_job(ctx["solver"], str(ctx["tmp"] / "board.json"), {"sets": {}})


@when(parsers.parse("the client estimates a job with the stop threshold {threshold}"))
def _threshold(ctx, threshold):
    spec = {"sets": {"decay_threshold": float(threshold)}}
    ctx["result"] = estimate.estimate_job(ctx["solver"], str(ctx["tmp"] / "board.json"), spec)


@then(parsers.parse("the estimate has {cells:d} cells and about {steps:d} steps"))
def _has(ctx, cells, steps):
    got, why = ctx["result"]
    assert got["cells"] == cells and abs(got["steps"] - steps) <= 1000, (got, why)


@then(parsers.parse("the estimate has about {steps:d} steps"))
def _steps(ctx, steps):
    got, why = ctx["result"]
    assert got and abs(got["steps"] - steps) <= 1, (got, why)


@then(parsers.parse('there is no estimate and the reason mentions "{text}"'))
def _none(ctx, text):
    got, why = ctx["result"]
    assert got is None and text in why, (got, why)


@then('the dry run was called with "--cpu", "--dry-run" and "--json" and a project folder of its own')
def _argv(ctx):
    argv = json.loads(Path(ctx["calls"]).read_text())
    assert {"--cpu", "--dry-run", "--json"} <= set(argv)
    folder = argv[argv.index("--project-dir") + 1]
    assert Path(folder).parent.name == "estimate" and Path(folder) != ctx["tmp"] and "--gpu" not in argv
