import math
import os
from pathlib import Path

import pytest
from pytest_bdd import given, parsers, scenarios, then, when

from emfdtd_plugin import geometry, launcher, snapshot

scenarios("features/snapshot.feature")


@pytest.fixture
def context():
    return {}


def _snap(stackup, copper):
    return {
        "format": snapshot.FORMAT,
        "version": 1,
        "board": {"name": "t", "kicad_version": "x"},
        "outline": [[0, 0], [1, 0], [1, 1]],
        "stackup": stackup,
        "copper": copper,
        "vias": [],
        "components": [],
    }


CU = {"name": "F.Cu", "kind": "copper"}
DIEL = {"name": "core", "kind": "dielectric", "thickness": 1e-3, "epsilon_r": 4.5, "loss_tangent": 0.02}


@given(parsers.parse("a 10 mm long track of width 0.5 mm"))
def _track(context):
    context["ring"] = lambda: geometry.segment_ring(0, 0, 0.010, 0, 0.0005, cap_steps=48)


@when("it is converted to a polygon")
def _convert(context):
    context["result"] = context["ring"]()


@then("its area is within 0.5 percent of length times width plus a full circle of the track width")
def _area(context):
    w = 0.0005
    expected = 0.010 * w + math.pi * (w / 2) ** 2
    assert geometry.ring_area(context["result"]) == pytest.approx(expected, rel=0.005)


@given("an arc from 1,0 through 0,1 to -1,0 in metres")
def _arc(context):
    context["sample"] = lambda: geometry.arc_points((1, 0), (0, 1), (-1, 0))


@when("it is sampled")
def _sample(context):
    context["result"] = context["sample"]()


@then("every sample lies within 1e-9 of the unit circle and the ends are the given points")
def _on_circle(context):
    pts = context["result"]
    assert all(abs(math.hypot(x, y) - 1) < 1e-9 for x, y in pts)
    assert pts[0] == pytest.approx([1, 0]) and pts[-1] == pytest.approx([-1, 0])


@given("a snapshot whose stackup has only copper")
def _only_copper(context):
    context["snap"] = _snap([CU], [])


@given("a snapshot with copper on layer B.Cu but a stackup with only F.Cu")
def _bad_layer(context):
    context["snap"] = _snap(
        [CU, DIEL], [{"layer": "B.Cu", "owner": "", "net": "", "polygon": [[0, 0], [1, 0], [1, 1]]}]
    )


@when("it is validated")
def _validate(context):
    try:
        snapshot.validate_snapshot(context["snap"])
        context["error"] = None
    except snapshot.SnapshotError as e:
        context["error"] = str(e)


@then("validation fails naming the stackup")
def _names_stackup(context):
    assert "stackup" in context["error"]


@then("validation fails naming layer B.Cu")
def _names_layer(context):
    assert "B.Cu" in context["error"]


@given(
    parsers.parse(
        "pad A spanning x 0 to 1 mm and pad B spanning x {x0:g} to {x1:g} mm, both spanning y 0 to 0.8 mm"
    )
)
def _pads(context, x0, x1):
    context["boxes"] = ((0, 0, 1e-3, 0.8e-3), (x0 * 1e-3, 0, x1 * 1e-3, 0.8e-3))


@when("a port is made from pad A to pad B")
def _make_port(context):
    try:
        context["port"] = geometry.port_between(*context["boxes"], "F.Cu")
        context["error"] = None
    except ValueError as e:
        context["error"] = str(e)


@then("the port runs from x 1.0 to x 1.3 mm at y 0.4 mm and is 0.8 mm wide")
def _port_ok(context):
    p = context["port"]
    assert p["a"] == pytest.approx([1.0e-3, 0.4e-3])
    assert p["b"] == pytest.approx([1.3e-3, 0.4e-3])
    assert p["width"] == pytest.approx(0.8e-3)


@then("port creation fails saying there is no gap")
def _port_fails(context):
    assert "no gap" in context["error"]


@given("a project folder with a solver window running")
def _running(context, tmp_path):
    (tmp_path / "em-fdtd").mkdir()
    (tmp_path / "em-fdtd" / "solver.pid").write_text(f"{os.getpid()}\n")  # this process is alive
    context["project"] = tmp_path


@given("a project folder with no solver window")
def _not_running(context, tmp_path):
    (tmp_path / "em-fdtd").mkdir()
    (tmp_path / "em-fdtd" / "solver.pid").write_text("999999999\n")  # no such process
    context["project"] = tmp_path


@when("a new board export is published")
def _publish(context, tmp_path):
    context["spawned"] = []
    solver = tmp_path / "emfdtd"
    solver.write_text("")
    os.environ["EMFDTD_BIN"] = str(solver)
    context["result"] = launcher.publish(
        {"x": 1}, context["project"], tmp_path, spawn=lambda cmd, **kw: context["spawned"].append(cmd)
    )


@then("no new solver is started and the snapshot file is rewritten")
def _reused(context):
    assert context["result"] == "reused" and context["spawned"] == []
    assert (context["project"] / "em-fdtd" / "snapshot.json").is_file()


@then("one solver is started on the new snapshot file")
def _started(context):
    assert context["result"] == "started" and len(context["spawned"]) == 1
    assert context["spawned"][0][1].endswith("snapshot.json") and "--ui" in context["spawned"][0]


@when("a run is requested for a new export")
def _publish_run(context, tmp_path):
    context["spawned"] = []
    solver = tmp_path / "emfdtd"
    solver.write_text("")
    os.environ["EMFDTD_BIN"] = str(solver)
    context["result"] = launcher.publish(
        {"x": 1},
        context["project"],
        tmp_path,
        spawn=lambda cmd, **kw: context["spawned"].append(cmd),
        request="run",
    )


@then("the new solver is told to start the run")
def _autorun(context):
    assert "--autorun" in context["spawned"][0]


@then("the open window is asked to run through the request file")
def _request_file(context):
    assert (context["project"] / "em-fdtd" / "request.txt").read_text().strip() == "run"


@given(parsers.parse("a snapshot whose copper layer has thickness {um:g} um"))
def _copper_thickness(context, um):
    context["snap"] = _snap([dict(CU, thickness=um * 1e-6), DIEL], [])


@then("validation passes")
def _passes(context):
    assert context["error"] is None


@then("validation fails naming the copper thickness")
def _names_thickness(context):
    assert "copper thickness" in context["error"]


def test_farm_request_opens_the_farm_tab_in_a_new_window(tmp_path):
    project = tmp_path / "proj"
    project.mkdir()
    solver = tmp_path / "emfdtd"
    solver.write_text("")
    os.environ["EMFDTD_BIN"] = str(solver)
    spawned = []
    launcher.publish({"x": 1}, project, tmp_path, spawn=lambda cmd, **kw: spawned.append(cmd), request="farm")
    assert spawned[0][-2:] == ["--ui-tab", "Farm"] and "--autorun" not in spawned[0]


def test_farm_action_is_declared_with_its_icons_and_entrypoint():
    import json

    root = Path(__file__).resolve().parents[1]
    action = {a["identifier"]: a for a in json.loads((root / "plugin.json").read_text())["actions"]}["farm"]
    assert action["show-button"] is True and (root / action["entrypoint"]).is_file()
    for key in ("icons-light", "icons-dark"):
        assert len(action[key]) == 2 and all((root / f).is_file() for f in action[key])


def test_solver_window_does_not_inherit_kicads_python_variables(tmp_path):
    project = tmp_path / "proj"
    project.mkdir()
    solver = tmp_path / "emfdtd"
    solver.write_text("")
    os.environ["EMFDTD_BIN"] = str(solver)
    os.environ["PYTHONHOME"] = "/kicad/python"
    os.environ["PYTHONPATH"] = "/kicad/site"
    try:
        seen = []
        launcher.publish({"x": 1}, project, tmp_path, spawn=lambda cmd, **kw: seen.append(kw["env"]))
    finally:
        del os.environ["PYTHONHOME"], os.environ["PYTHONPATH"]
    assert "PYTHONHOME" not in seen[0] and "PYTHONPATH" not in seen[0] and "PATH" in seen[0]
