import json
import subprocess

import pytest
from pytest_bdd import given, parsers, scenarios, then, when

from emfdtd_plugin.farm.gui import controller as ctl
from farm_gui_fake import DOCTOR_BAD, FakeAgent

scenarios("features/farm_gui_controller.feature")


@pytest.fixture
def ctx():
    fake = FakeAgent()
    return {"fake": fake, "c": ctl.Controller(fake)}


def _valid(**over):
    s = ctl.Settings(solver="/x/emfdtd", device="RTX", turn="coordinator")
    for k, v in over.items():
        setattr(s, k, v)
    return s


# --- status ---------------------------------------------------------------------------------------


@given(parsers.parse("an agent status {text}"))
def _status(ctx, text):
    ctx["status"] = ctl.parse_status(text)


@then(parsers.parse('the panel says "{state}" and "{line}"'))
def _panel(ctx, state, line):
    d = ctl.describe(ctx["status"])
    assert d["state"] == state
    if line != "(nothing more)":
        assert any(line in v for v in d.values() if isinstance(v, str)), d


@then('the panel says "Unknown" and "was not understood"')
def _unknown(ctx):
    d = ctl.describe(ctx["status"])
    assert d["state"] == "Unknown" and "was not understood" in d["problem"]


@then(parsers.parse("the progress is {n:d}"))
def _progress(ctx, n):
    assert ctl.describe(ctx["status"])["percent"] == n


@then(parsers.parse('stopping asks a question that mentions "{a}" and "{b}"'))
def _asks(ctx, a, b):
    q = ctx["c"].stop_needs_confirmation(ctx["status"])
    assert q and a in q and b in q


@then("stopping asks nothing")
def _asks_nothing(ctx):
    assert ctx["c"].stop_needs_confirmation(ctx["status"]) is None


# --- settings -------------------------------------------------------------------------------------


@given("settings with no solver, no GPU and a lock command mode with no lock command")
def _bad_settings(ctx):
    ctx["settings"] = ctl.Settings(solver="", device="", turn="lock", lock_command="")


@given("valid settings")
def _valid_settings(ctx):
    ctx["settings"] = _valid(device="RTX, Radeon", max_minutes="45")


@given(parsers.parse('valid settings with {field} set to "{value}"'))
def _field(ctx, field, value):
    ctx["settings"] = _valid(**{field: value})


@then(parsers.parse('the problems mention "{a}", "{b}" and "{c}"'))
def _problems(ctx, a, b, c):
    text = " ".join(ctl.validate_settings(ctx["settings"]))
    assert a in text and b in text and c in text


@then(parsers.parse("the settings are {verdict}"))
def _verdict(ctx, verdict):
    assert (not ctl.validate_settings(ctx["settings"])) == (verdict == "valid")


@when("the settings are saved")
def _save(ctx):
    ctx["res"] = ctx["c"].save_settings(ctx["settings"])


@then('the command was "agent configure --json-in" with numbers as numbers and the GPU as a list')
def _json_in(ctx):
    assert ctx["fake"].calls[-1] == ["agent", "configure", "--json-in"]
    saved = ctx["fake"].settings
    assert saved["max_minutes"] == 45 and isinstance(saved["max_minutes"], int)
    assert saved["devices"] == ["RTX", "Radeon"]  # the agent's own key, a list
    assert saved["turn"] == {"mode": "coordinator", "lock_command": []}
    assert set(ctx["fake"].saved_documents[-1]) <= {
        "solver",
        "devices",
        "turn",
        "max_minutes",
        "max_steps",
        "max_input_mb",
        "max_tier",
        "watchdog",
        "work",
    }  # nothing the agent would refuse as an unknown setting


@then("nothing was run")
def _nothing(ctx):
    assert ctx["fake"].calls == []


# --- the one-time code ----------------------------------------------------------------------------


@given("a controller with a fake agent")
def _controller(ctx):
    pass


@when(parsers.parse('enrol is run with the code "{code}"'))
def _enrol_code(ctx, code):
    ctx["code"] = code
    ctx["c"]._run(
        ["enroll", "1.2.3.4:7421", "--fingerprint", "ab", "--code", code, "--coordinator", "1.2.3.4:7420"]
    )


@then("the code appears in no recorded command and no history entry")
def _no_code(ctx):
    assert all(ctx["code"] not in " ".join(c) for c in ctx["fake"].calls)
    assert all(ctx["code"] not in " ".join(h[0]) for h in ctx["c"].history)
    assert any("********" in h[0] for h in ctx["c"].history)


# --- wizard ---------------------------------------------------------------------------------------


def _wizard(ctx, tls=True, done=()):
    ctx["w"] = ctl.Wizard(ctx["c"], tls_available=lambda: tls)
    ctx.setdefault("settings", _valid(lock_command="/x/lock nice"))
    for step in done:
        ctx["w"].results[step] = ctl.StepResult(True, "test")


@given("a wizard on a machine with TLS 1.3")
def _w_tls(ctx):
    _wizard(ctx)


@given("a wizard on a machine without TLS 1.3")
def _w_notls(ctx):
    _wizard(ctx, tls=False)


@given(parsers.parse('a wizard with step "{step}" done'))
def _w_one(ctx, step):
    _wizard(ctx, done=[step])


@given(parsers.parse('a wizard with steps "{a}" and "{b}" done'))
def _w_two(ctx, a, b):
    _wizard(ctx, done=[a, b])


@given(parsers.parse('a wizard with steps "{a}", "{b}" and "{c}" done'))
def _w_three(ctx, a, b, c):
    _wizard(ctx, done=[a, b, c])


@given(parsers.parse('a wizard with every step done up to "{step}"'))
def _w_upto(ctx, step):
    _wizard(ctx, done=ctl.STEPS[: ctl.STEPS.index(step)])


@given(parsers.parse('a wizard with every step done up to "{step}" and a failing check'))
def _w_failing(ctx, step):
    _wizard(ctx, done=ctl.STEPS[: ctl.STEPS.index("doctor")])
    ctx["fake"].doctor_checks = DOCTOR_BAD


@then(parsers.parse('only step "{step}" can run'))
def _only(ctx, step):
    assert [s for s in ctl.STEPS if ctx["w"].can_run(s)] == [step]


@when(parsers.parse('step "{step}" is run'))
def _run_step(ctx, step):
    ctx["res"] = ctx["w"].run(step)


@when(parsers.parse('step "{step}" is run again'))
def _run_again(ctx, step):
    ctx["res"] = ctx["w"].run(
        step, invitation="1.2.3.4:7421", fingerprint="ab", code="x", coordinator="1.2.3.4:7420"
    )


@when(parsers.parse('step "{step}" is run without confirming'))
def _no_confirm(ctx, step):
    ctx["res"] = ctx["w"].run(step, confirmed=False)


@when(parsers.parse('step "{step}" is run confirming'))
def _confirm(ctx, step):
    ctx["res"] = ctx["w"].run(step, confirmed=True)


@then(parsers.parse('step "{a}" can run and step "{b}" cannot'))
def _can(ctx, a, b):
    assert ctx["w"].can_run(a) and not ctx["w"].can_run(b)


@then(parsers.parse('the step failed with "{text}" and step "{b}" cannot run'))
def _failed_and(ctx, text, b):
    assert not ctx["res"].ok and text in ctx["res"].message
    assert not ctx["w"].can_run(b)


@then(parsers.parse('the step failed with "{text}" and nothing was run'))
def _failed_nothing(ctx, text):
    assert not ctx["res"].ok and text in ctx["res"].message
    assert ctx["fake"].calls == []


@then(parsers.parse('the step failed with "{text}"'))
def _failed(ctx, text):
    assert not ctx["res"].ok and text in ctx["res"].message


@then(parsers.parse('the command was "{text}"'))
def _command(ctx, text):
    assert " ".join(ctx["fake"].calls[-1]) == text


@when(parsers.parse('enrol is run with invitation "{inv}" and coordinator "{coord}"'))
def _enrol(ctx, inv, coord):
    ctx["res"] = ctx["w"].run("enroll", invitation=inv, fingerprint="ab-cd", code="XXXX", coordinator=coord)


@then(parsers.parse("the enrol step is {verdict}"))
def _enrol_verdict(ctx, verdict):
    ran = bool(ctx["fake"].calls)
    assert ran == (verdict == "run")
    if verdict == "refused":
        assert not ctx["res"].ok and "host:port" in ctx["res"].message


@when("devices are recorded for the coordinator-held mode without holding the turn")
def _dev_no_hold(ctx):
    ctx["res"] = ctx["w"].run(
        "devices", solver="/x/emfdtd", device="RTX", turn="coordinator", holding_turn=False
    )


@when("devices are recorded for the coordinator-held mode while holding the turn")
def _dev_hold(ctx):
    ctx["res"] = ctx["w"].run(
        "devices", solver="/x/emfdtd", device="RTX", turn="coordinator", holding_turn=True
    )


@when(parsers.parse('devices are recorded in lock mode with the lock command "{lock}"'))
def _dev_lock(ctx, lock):
    ctx["res"] = ctx["w"].run("devices", solver="/x/emfdtd", device="RTX", turn="lock", lock_command=lock)


@then(parsers.parse('steps "{a}" and "{b}" are no longer done'))
def _undone(ctx, a, b):
    assert not ctx["w"].done(a) and not ctx["w"].done(b)


# --- refresher ------------------------------------------------------------------------------------


class Clock:
    def __init__(self):
        self.t = 100.0

    def __call__(self):
        return self.t


@given(parsers.parse("a refresher with a {n:d} second interval"))
def _refresher(ctx, n):
    ctx["clock"] = Clock()
    ctx["r"] = ctl.Refresher(interval=n, clock=ctx["clock"])


@then("it is not due before it is started")
def _not_started(ctx):
    assert ctx["r"].due() is False


@when("it is started")
def _start(ctx):
    ctx["r"].start()


@then("it is due at once and not due again until 2 seconds have passed")
def _due(ctx):
    r, clock = ctx["r"], ctx["clock"]
    assert r.due() is True and r.due() is False
    clock.t += 1.9
    assert r.due() is False
    clock.t += 0.2
    assert r.due() is True


@when("it is stopped")
def _stop(ctx):
    ctx["r"].stop()
    ctx["clock"].t += 100


@then("it is never due")
def _never(ctx):
    assert ctx["r"].due() is False


# --- runner ---------------------------------------------------------------------------------------


@given(parsers.parse('a runner for the tool "{tool}" that times out'))
def _runner(ctx, tool, monkeypatch):
    seen = {}

    def fake_run(cmd, **kw):
        seen["cmd"], seen["kw"] = cmd, kw
        raise subprocess.TimeoutExpired(cmd, kw.get("timeout", 0))

    monkeypatch.setattr(ctl.subprocess, "run", fake_run)
    ctx["seen"] = seen
    ctx["runner"] = ctl.make_runner(tool, python="/p/python")


@when(parsers.parse('it runs "{args}"'))
def _runs(ctx, args):
    ctx["res"] = ctx["runner"](args.split(), None, 3)


@then("the child command was the Python, the tool and the arguments as separate items")
def _child(ctx):
    assert ctx["seen"]["cmd"] == ["/p/python", "/p/emfdtd_farm.py", "agent", "status", "--json"]
    assert "shell" not in ctx["seen"]["kw"] or ctx["seen"]["kw"]["shell"] is False


@then("the result says it did not finish")
def _timeout(ctx):
    assert ctx["res"].code == 124 and "did not finish" in ctx["res"].err


# --- texts ----------------------------------------------------------------------------------------


@then(parsers.parse('the autostart text for "{system}" mentions "{thing}"'))
def _autostart_text(system, thing):
    assert thing in ctl.autostart_description(system)


@then("an unreadable doctor answer says it was not understood")
def _doctor_bad():
    assert "not understood" in ctl.parse_doctor("{").error


@then("an unreadable autostart answer says it was not understood")
def _autostart_bad():
    assert "not understood" in ctl.parse_autostart("nope").error


def test_json_helpers_roundtrip():
    s = ctl.Settings.from_json(json.loads(json.dumps(_valid(device="a, b").to_json())))
    assert s.device == "a, b" and s.turn == "coordinator"


def test_gui_without_tkinter_says_so_and_does_not_crash(monkeypatch, capsys):
    import sys

    from emfdtd_plugin.farm import gui

    monkeypatch.setitem(sys.modules, "tkinter", None)  # `import tkinter` now raises ImportError
    monkeypatch.delitem(sys.modules, "emfdtd_plugin.farm.gui.view", raising=False)
    assert gui.main() == 1
    err = capsys.readouterr().err
    assert "needs tkinter" in err and "emfdtd-farm agent status" in err


def test_lock_command_is_split_into_the_list_the_agent_stores():
    doc = _valid(turn="lock", lock_command="/opt/with lock.sh nice -n 10").to_json()
    assert doc["turn"]["mode"] == "lock" and doc["turn"]["lock_command"][-3:] == ["nice", "-n", "10"]
    assert ctl.Settings.from_json(doc).turn == "lock"
    assert _valid(turn="none", lock_command="ignored").to_json()["turn"] == {
        "mode": "none",
        "lock_command": [],
    }


def test_no_saved_configuration_is_normal_on_a_new_machine():
    class Runner:
        def __call__(self, args, stdin=None, timeout=60):
            return ctl.Result(1, json.dumps({"error": "no saved configuration"}), "")

    s, message = ctl.Controller(Runner(), recorded=lambda: ["RTX"]).load_settings()
    assert message == "" and s.recorded_devices == ["RTX"] and s.turn == "lock"


def test_a_real_error_reading_the_settings_is_shown():
    class Runner:
        def __call__(self, args, stdin=None, timeout=60):
            return ctl.Result(1, json.dumps({"error": "the file is damaged"}), "")

    assert ctl.Controller(Runner(), recorded=lambda: []).load_settings()[1] == "the file is damaged"


def test_recorded_devices_come_from_the_file_devices_save_wrote(tmp_path):
    (tmp_path / "devices.json").write_text(
        json.dumps([{"name": "RTX 4060 Ti", "uuid": "99cb"}, {"uuid": "190a"}])
    )
    assert ctl.recorded_device_names(tmp_path) == ["RTX 4060 Ti", "190a"]
    assert ctl.recorded_device_names(tmp_path / "nowhere") == []


def test_an_empty_log_says_so():
    class Runner:
        def __call__(self, args, stdin=None, timeout=60):
            return ctl.Result(0, "", "")

    assert ctl.Controller(Runner(), recorded=lambda: []).log() == "(the log is empty)"
