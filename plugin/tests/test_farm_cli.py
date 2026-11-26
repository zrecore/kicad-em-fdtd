import io
import json

import pytest
from pytest_bdd import given, parsers, scenarios, then, when

from emfdtd_plugin.farm import cli, config

scenarios("features/farm_cli.feature")


@pytest.fixture
def ctx():
    return {}


@given("a clean farm home")
def _clean(monkeypatch, tmp_path):
    monkeypatch.setenv("EMFDTD_FARM_HOME", str(tmp_path / "farm"))


@given("the Python has no TLS 1.3")
def _no_tls13(monkeypatch):
    monkeypatch.setattr(cli.pki.ssl, "HAS_TLSv1_3", False)


@when(parsers.parse('the user runs "{command}" and answers "{answer}"'))
def _run(ctx, command, answer):
    out = io.StringIO()
    ctx["rc"] = cli.main(command.split(), stdin=io.StringIO(answer + "\n"), out=out)
    ctx["out"] = out.getvalue()


@then("the command fails")
def _fails(ctx):
    assert ctx["rc"] != 0


@then(parsers.parse('the command succeeds and the output mentions "{text}"'))
def _ok(ctx, text):
    assert ctx["rc"] == 0 and text in ctx["out"]


@then("the coordinator is off")
def _coord_off():
    assert config.enabled("coordinator") is False


@then("the coordinator is on and the worker is off")
def _coord_on():
    assert config.enabled("coordinator") is True and config.enabled("worker") is False


@then("the worker is on")
def _worker_on():
    assert config.enabled("worker") is True


@then("the status says every role is off")
def _status(ctx):
    assert json.loads(ctx["out"])["enabled"] == {"coordinator": False, "worker": False, "client": False}


@then("starting the worker with only a solver and a device exits with the turn message")
def _worker_needs_turn():
    with pytest.raises(SystemExit) as e:
        cli.main(["worker", "--solver", "emfdtd", "--device", "rtx"])
    assert "exactly one of" in str(e.value) and "--lock-command" in str(e.value)
