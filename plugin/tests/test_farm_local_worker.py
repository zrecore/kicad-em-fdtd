import contextlib
import io
import json

from pytest_bdd import given, parsers, scenarios, then, when

from emfdtd_plugin.farm import agent, cli, config, local_worker, pki, policy
from farm_stub.paths import STUB

scenarios("features/farm_local_worker.feature")


def _cli(ctx, argv, stdin=""):
    out, err = io.StringIO(), io.StringIO()
    args = cli._parser().parse_args(argv)
    code = local_worker.run(args, out, io.StringIO(stdin), err)
    ctx.update(code=code, out=out.getvalue(), err=err.getvalue())
    return code


def _ctx():
    return {}


import pytest  # noqa: E402


@pytest.fixture
def ctx():
    return _ctx()


@pytest.fixture(autouse=True)
def _no_agent(monkeypatch):
    monkeypatch.setattr(agent, "start", lambda *a, **k: {"pid": 1})  # the agent has its own tests
    yield
    with contextlib.suppress(Exception):
        agent.stop(1)


@given("the coordinator role is on and this machine knows its coordinator")
def _ready(farm):
    config.set_enabled("coordinator", True)
    (config.home() / "coordinator.json").write_text(
        json.dumps({"host": "127.0.0.1", "port": farm.coord.port})
    )


@when("the user enables the local GPU with the stub solver without starting the agent")
def _enable(ctx):
    assert _cli(ctx, ["local-worker", "enable", "--yes", "--json", "--no-start", "--solver", STUB]) == 0, ctx


@when("the user disables the local GPU")
def _disable(ctx):
    assert _cli(ctx, ["local-worker", "disable", "--json"]) == 0, ctx


@when(parsers.parse('the user runs local-worker enable and types "{text}"'))
def _typed(ctx, text):
    _cli(ctx, ["local-worker", "enable", "--no-start", "--solver", STUB], stdin=text + "\n")


def _status(ctx):
    _cli(ctx, ["local-worker", "status", "--json"])
    return json.loads(ctx["out"])


@then("the local GPU status says available, not enabled, no worker name")
def _idle(ctx):
    st = _status(ctx)
    assert st["available"] and not st["enabled"] and st["name"] == "" and not st["running"], st


@then(parsers.parse('the local GPU status says it is unavailable because "{why}"'))
def _unavailable(ctx, why):
    st = _status(ctx)
    assert not st["available"] and why in st["reason"], st


@then(parsers.parse('enabling it fails saying "{why}"'))
def _fails(ctx, why):
    assert _cli(ctx, ["local-worker", "enable", "--yes", "--json", "--no-start", "--solver", STUB]) == 1
    assert why in json.loads(ctx["out"])["error"]


@then("the worker role is on")
def _worker_on():
    assert config.enabled("worker")


@then("the worker role is off")
def _worker_off():
    assert not config.enabled("worker")


@then("this machine is a member of the farm as a worker that may run jobs")
def _member(ctx):
    name = local_worker._worker_name()
    assert name and f"worker:{name}" in pki.members()
    assert "run" in policy.load()["devices"][f"worker:{name}"]["may"]


@then(parsers.parse('the saved agent settings offer the usable GPUs "{gpus}" and the stub solver'))
def _saved(gpus):
    cfg = agent.load()
    assert cfg["solver"] == STUB and cfg["turn"]["mode"] == "none" and len(cfg["devices"]) == 2
    devs = json.loads((agent.work_dir(cfg) / "devices.json").read_text())
    assert [d["name"] for d in devs] == gpus.split(", "), devs  # the Vulkan 1.1 GPU is left out


@then("the local GPU status says enabled with the same worker name")
def _enabled(ctx):
    st = _status(ctx)
    assert st["enabled"] and st["name"] == local_worker._worker_name() and st["turn"] == "none", st


@then("the farm has exactly one worker identity for this machine")
def _one():
    mine = [k for k in pki.members() if k.startswith("worker:")]
    assert len(mine) == 1, mine


@then("the local GPU status says not enabled but keeps the worker name")
def _kept(ctx):
    st = _status(ctx)
    assert not st["enabled"] and st["name"], st


@then("nothing is enabled")
def _nothing(ctx):
    assert ctx["code"] == 1 and not config.enabled("worker") and local_worker._worker_name() is None
