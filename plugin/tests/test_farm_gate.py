import importlib
import json
import socket
import sys

import pytest
from pytest_bdd import given, parsers, scenarios, then, when

from emfdtd_plugin.farm import config

scenarios("features/farm_gate.feature")


@pytest.fixture
def ctx():
    return {}


@given("a clean farm home")
def _clean(tmp_path, monkeypatch):
    monkeypatch.setenv("EMFDTD_FARM_HOME", str(tmp_path / "farm"))


@given("a switch file that is not JSON")
def _broken(tmp_path):
    home = tmp_path / "farm"
    home.mkdir()
    (home / "farm.json").write_text("{not json")


@given(parsers.parse('a switch file that says the worker is "{value}"'))
def _yes(tmp_path, value):
    home = tmp_path / "farm"
    home.mkdir()
    (home / "farm.json").write_text(json.dumps({"enabled": {"worker": value}}))


@when("the farm package is imported with sockets forbidden")
def _import(monkeypatch):
    def forbidden(*a, **k):
        raise AssertionError("the farm package opened a socket on import")

    monkeypatch.setattr(socket, "socket", forbidden)
    for name in [n for n in sys.modules if n.startswith("emfdtd_plugin.farm")]:
        del sys.modules[name]
    importlib.import_module("emfdtd_plugin.farm")
    importlib.import_module("emfdtd_plugin.farm.config")
    importlib.import_module("emfdtd_plugin.farm.protocol")


@when("the coordinator is required to run")
def _require(ctx):
    from emfdtd_plugin.farm import config as c

    try:
        c.require_enabled("coordinator")
    except c.FarmDisabled as e:
        ctx["error"] = str(e)


@when("the user enables the worker")
def _enable():
    config.set_enabled("worker", True)


@when("the user disables the worker")
def _disable():
    config.set_enabled("worker", False)


@then("no farm file exists")
def _nofile(tmp_path):
    assert not (tmp_path / "farm").exists()


@then("the coordinator is off and the worker is off")
def _both_off():
    assert config.enabled("coordinator") is False
    assert config.enabled("worker") is False


@then("the worker is on and the coordinator is off")
def _worker_on():
    assert config.enabled("worker") is True
    assert config.enabled("coordinator") is False


@then("the worker is off")
def _worker_off():
    assert config.enabled("worker") is False


@then("it is refused and the message says how to enable it")
def _refused(ctx):
    assert "emfdtd-farm enable coordinator" in ctx["error"]
