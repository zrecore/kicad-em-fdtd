import math

import pytest
from pytest_bdd import given, parsers, scenarios, then, when

from emfdtd_plugin.farm import jobs, policy

scenarios("features/farm_jobs.feature")

LIST_SETTINGS = """\
profile                        essential  wifi  [Band] The kind of radio
mesh_preset                    essential  medium  [Mesh] Coarse is fast
decay_threshold                advanced  0.001  [Run] stop at
max_steps                      advanced  400000  [Run] cap
lateral_growth_ratio           advanced  1.1  [Mesh] growth
copper_model                   advanced  slab  [Mesh] model
backend                        common  auto  [Run] backend
secret_expert_knob             expert  1  [Run] expert only
"""


@pytest.fixture
def ctx():
    return {"limits": dict(policy.DEFAULT_LIMITS)}


@given("the solver's settings with tiers")
def _tiers(ctx):
    ctx["tiers"] = jobs.parse_list_settings(LIST_SETTINGS)
    assert ctx["tiers"]["secret_expert_knob"] == "expert"


def _normal():
    return {
        "profile": "bluetooth",
        "sets": {"mesh_preset": "medium", "decay_threshold": 0.001},
        "refine": [[53.5, 119.0, 56.5, 124.0, 0.06]],
    }


@when(
    'the job has profile "bluetooth", setting mesh_preset "medium", setting decay_threshold 0.001 '
    "and one refine box"
)
def _job(ctx):
    ctx["spec"] = _normal()
    ctx["job"] = jobs.validate(ctx["spec"], ctx["tiers"], ctx["limits"])


@when("a normal job is prepared")
def _normal_job(ctx):
    ctx["job"] = jobs.validate(_normal(), ctx["tiers"], ctx["limits"])


@when(parsers.parse('the job has the net "{net}" and the ground net "{ground}"'))
def _nets(ctx, net, ground):
    ctx["job"] = jobs.validate({"nets": [net], "ground_nets": [ground]}, ctx["tiers"], ctx["limits"])


ATTACKS = {
    "a setting named backend": {"sets": {"backend": "cpu"}},
    "a setting that does not exist": {"sets": {"no_such_setting": 1}},
    "an expert-tier setting": {"sets": {"secret_expert_knob": 1}},
    "a setting value that starts with a dash": {"sets": {"copper_model": "--help"}},
    "a setting value with a shell metacharacter": {"sets": {"copper_model": "slab;rm"}},
    "a max_steps above the limit": {"sets": {"max_steps": 10**9}},
    "an unknown field named project_dir": {"project_dir": "/etc"},
    "an unknown field named argv": {"argv": ["--open-run", "/"]},
    "a net name that starts with a dash": {"nets": ["--ui"]},
    "a net name containing a comma": {"nets": ["A,B"]},
    "a part reference with a slash": {"parts": {"R1/../x": "open"}},
    "a body treatment that is not a known one": {"bodies": {"U1": "rm -rf"}},
    "a refine box with four numbers": {"refine": [[1, 2, 3, 4]]},
    "a non-finite number": {"sets": {"lateral_growth_ratio": math.inf}},
    "a boolean where a number is wanted": {"sets": {"lateral_growth_ratio": True}},
}


@when(parsers.parse("the job is {attack}"))
def _attack(ctx, attack):
    try:
        jobs.validate(ATTACKS[attack], ctx["tiers"], ctx["limits"])
    except jobs.JobError as e:
        ctx["error"] = str(e)


@when(parsers.parse('the argv is built for device "{device}" in "{project}"'))
def _argv(ctx, device, project):
    device = "" if device == "(empty)" else device
    try:
        ctx["argv"] = jobs.build_argv(
            "/opt/emfdtd/emfdtd", "/jobs/j1/board.json", ctx["job"], project, device
        )
    except jobs.JobError as e:
        ctx["error"] = str(e)


@when(parsers.parse('a sweep of "{key}" over {n:d} values is expanded'))
def _sweep(ctx, key, n):
    base = {"sets": {"mesh_preset": "medium"}, "geoms": {}}
    ctx["sweep_key"] = key
    ctx["points"] = jobs.expand_sweep(base, key, [1.0 + i for i in range(n)])


@then("validation fails")
def _fails(ctx):
    assert ctx.get("error")


@then("building fails")
def _build_fails(ctx):
    assert ctx.get("error")
    assert "argv" not in ctx


@then("the argv starts with the solver and the snapshot")
def _start(ctx):
    assert ctx["argv"][:2] == ["/opt/emfdtd/emfdtd", "/jobs/j1/board.json"]


@then(parsers.parse('it contains "{a}" and "{b}" and "{c}"'))
def _contains3(ctx, a, b, c):
    flat = " ".join(ctx["argv"])
    for part in (a, b, c):
        assert part in flat


@then(parsers.parse('it contains "{a}"'))
def _contains(ctx, a):
    assert a in " ".join(ctx["argv"])


@then(parsers.parse('the argv has "--nets" followed by exactly "{net}"'))
def _nets_argv(ctx, net):
    i = ctx["argv"].index("--nets")
    assert ctx["argv"][i + 1] == net


@then(parsers.parse("there are {n:d} jobs each with one value"))
def _points(ctx, n):
    assert len(ctx["points"]) == n
    assert [p["sets"]["mesh_preset"] for p in ctx["points"]] == [1.0 + i for i in range(n)]


@then("each job carries the value as a geometry stretch")
def _geom(ctx):
    assert all(p["geoms"] == {"ANT.x": 1.0 + i} for i, p in enumerate(ctx["points"]))
    assert all(p["sets"] == {"mesh_preset": "medium"} for p in ctx["points"])
