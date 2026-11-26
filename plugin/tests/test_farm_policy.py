import pytest
from pytest_bdd import given, parsers, scenarios, then, when

from emfdtd_plugin.farm import policy

scenarios("features/farm_policy.feature")


@pytest.fixture
def ctx():
    return {}


@given("an empty policy")
def _empty(monkeypatch, tmp_path):
    monkeypatch.setenv("EMFDTD_FARM_HOME", str(tmp_path / "farm"))


@when(parsers.parse('the worker "{name}" is enrolled'))
def _worker(name):
    policy.add_device("worker", name)


@when(parsers.parse('the client "{name}" is enrolled'))
def _client(name):
    policy.add_device("client", name)


@when(parsers.parse('the worker "{name}" is removed'))
def _removed(name):
    policy.remove_device("worker", name)


@when(parsers.parse('the worker "{name}" is enrolled with the limit max_steps {n:d}'))
def _limited(name, n):
    policy.add_device("worker", name, limits={"max_steps": n})


@then(parsers.parse('"{role}" "{name}" may "{action}"'))
def _may(role, name, action):
    assert policy.authorize((role, name), action) == (True, "ok")


@then(parsers.parse('"{role}" "{name}" may not "{action}"'))
def _may_not(role, name, action):
    allowed, reason = policy.authorize((role, name), action)
    assert not allowed and reason


@then(parsers.parse('no identity may "{action}"'))
def _anon(action):
    assert policy.authorize(None, action)[0] is False


@then(parsers.parse('the limit {name} of "{role}" "{who}" is {value:d}'))
def _limit(name, role, who, value):
    assert policy.limits_for((role, who))[name] == value
