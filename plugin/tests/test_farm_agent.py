import contextlib
import io
import json
import os
import shlex
import subprocess
import sys
import time
from pathlib import Path

import pytest
from pytest_bdd import given, parsers, scenarios, then, when

from emfdtd_plugin.farm import agent, autostart, cli, config, enroll, pki, policy
from emfdtd_plugin.farm.worker import parse_devices
from farm_stub.paths import STUB

scenarios("features/farm_agent.feature", "features/farm_agent_run.feature", "features/farm_setup.feature")

TOOL = str(Path(__file__).resolve().parent.parent / "tools" / "emfdtd_farm.py")
SRC = str(Path(__file__).resolve().parent.parent / "src")


@pytest.fixture
def ctx():
    return {"pids": [], "procs": []}


@pytest.fixture(autouse=True)
def _no_orphans(monkeypatch, ctx):
    """Whatever a scenario started, it must not leave running."""
    yield
    with contextlib.suppress(Exception):
        agent.stop(5)
    for env in ctx.get("other_homes", []):
        subprocess.run([sys.executable, TOOL, "agent", "stop"], env=env, capture_output=True, timeout=60)
    for p in ctx["procs"]:
        with contextlib.suppress(Exception):
            p.kill()


def _cli(ctx, command, stdin=""):
    """Run `emfdtd_farm.py COMMAND` in this process; keep its exit code, stdout and stderr."""
    out, err = io.StringIO(), io.StringIO()
    code = 0
    try:
        with contextlib.redirect_stderr(err):
            args = cli._parser().parse_args(shlex.split(command))
        code = agent.run(args, out, io.StringIO(stdin), err)
    except SystemExit as e:  # argparse refusing the flags
        code = e.code if isinstance(e.code, int) else 2
    ctx.update(code=code, out=out.getvalue(), err=err.getvalue())
    return code


def _json(text):
    return json.loads(text.strip().splitlines()[-1])


def _stub_devices(work):
    out = subprocess.run([STUB, "--list-devices"], capture_output=True, text=True, check=True).stdout
    work.mkdir(parents=True, exist_ok=True)
    devs = [{**d, "vulkan": list(d["vulkan"])} for d in parse_devices(out)]
    (work / "devices.json").write_text(json.dumps(devs))


def _wait(check, seconds=20.0, what="the condition"):
    end = time.time() + seconds
    while time.time() < end:
        got = check()
        if got:
            return got
        time.sleep(0.1)
    raise AssertionError(f"timed out waiting for {what}")


# --- clean-home scenarios ----------------------------------------------------------------------------


@given("a clean farm home")
def _clean(monkeypatch, tmp_path):
    monkeypatch.setenv("EMFDTD_FARM_HOME", str(tmp_path / "home"))
    monkeypatch.setenv("HOME", str(tmp_path / "userhome"))  # autostart paths: never the real ones
    monkeypatch.setattr(autostart, "_run", lambda argv: (0, ""))  # nothing external is ever run


DEVICE = '--device "stub gpu a"'
CONFIGURE = {
    'the stub solver, the device "stub gpu a" and no lock': f"--solver {STUB} {DEVICE} --no-lock",
    "no way to take the GPU's turn": f'--solver {STUB} --device "stub gpu a"',
    "two ways to take the GPU's turn": f"--solver {STUB} {DEVICE} --no-lock --turn-from-coordinator",
    "a solver that does not exist": '--solver /no/such/emfdtd --device "stub gpu a" --no-lock',
    "no device": f"--solver {STUB} --no-lock",
    "a tier that is not one of the tiers": f"--solver {STUB} {DEVICE} --no-lock --max-tier nonsense",
    "a watchdog of zero seconds": f'--solver {STUB} --device "stub gpu a" --no-lock --watchdog 0',
    "a ceiling of minus five minutes": f'--solver {STUB} --device "stub gpu a" --no-lock --max-minutes=-5',
}


@when(parsers.parse("the user configures the agent with {what}"))
def _configure(ctx, what):
    if what == "a lock command that is empty":
        body = {"solver": STUB, "devices": ["stub gpu a"], "turn": {"mode": "lock", "lock_command": []}}
        _cli(ctx, "agent configure --json-in --json", json.dumps(body))
    else:
        _cli(ctx, "agent configure " + CONFIGURE[what])


@when(parsers.parse('the user runs "{command}"'))
def _run(ctx, command):
    _cli(ctx, command)


@then("the saved configuration is exactly those settings")
def _saved_exact(ctx):
    assert ctx["code"] == 0, ctx
    assert agent.load() == {
        "solver": STUB,
        "devices": ["stub gpu a"],
        "turn": {"mode": "none", "lock_command": []},
        "max_minutes": None,
        "max_steps": None,
        "max_input_mb": None,
        "max_tier": None,
        "watchdog": 20.0,
        "work": None,
        "log": None,
    }


@then("the configuration file can be read only by its owner")
def _private():
    if sys.platform == "win32":
        pytest.skip("Windows ACLs are not mode bits (config.restrict uses icacls)")
    assert agent.config_file().stat().st_mode & 0o077 == 0


@then("the configuration holds no key, token or certificate")
def _no_secret():
    text = agent.config_file().read_text().lower()
    for word in ("key", "token", "cert", "password", "secret", "begin "):
        assert word not in text, word


@then(parsers.parse('the command fails saying "{message}"'))
def _fails_saying(ctx, message):
    assert ctx["code"] != 0
    assert message in ctx["err"] + ctx["out"], (ctx["err"], ctx["out"])


@then("nothing is saved")
def _nothing_saved():
    assert not agent.config_file().exists()


@then("the output is the saved configuration as JSON")
def _show_json(ctx):
    assert ctx["code"] == 0
    assert _json(ctx["out"]) == agent.load()
    assert _json(ctx["out"])["solver"] == STUB


@when(parsers.parse('the user saves this JSON on stdin with "{command}": {what}'))
def _json_in(ctx, command, what):
    body = {"solver": STUB, "devices": ["stub gpu b"], "turn": {"mode": "none", "lock_command": []}}
    if "45 minute" in what:
        body["max_minutes"] = 45
    if "unknown key" in what:
        body["password"] = "x"
    _cli(ctx, command, json.dumps(body))


@then(parsers.parse('the saved configuration has the device "{device}" and a ceiling of {n:d} minutes'))
def _replaced(ctx, device, n):
    assert ctx["code"] == 0, ctx
    cfg = agent.load()
    assert cfg["devices"] == [device] and cfg["max_minutes"] == n
    assert _json(ctx["out"]) == cfg


@then(parsers.parse('the command fails and prints a JSON error naming "{text}"'))
def _json_error(ctx, text):
    assert ctx["code"] == 1
    assert text in _json(ctx["out"])["error"]


@then("the command fails")
def _fails(ctx):
    assert ctx["code"] != 0


@then(
    "the report has the checks tls13, role, identity, coordinator, certificate, solver, settings, devices, "
    "workdir, config in that order"
)
def _ids(ctx):
    report = _json(ctx["out"])
    assert set(report) == {"ok", "checks"}
    assert report["ok"] is False
    assert [c["id"] for c in report["checks"]] == [
        "tls13",
        "role",
        "identity",
        "coordinator",
        "certificate",
        "solver",
        "settings",
        "devices",
        "workdir",
        "config",
    ]


@then("every failing check has a title, a detail and a one-line fix")
def _fixes(ctx):
    for c in _json(ctx["out"])["checks"]:
        assert set(c) == {"id", "title", "ok", "detail", "fix"}
        assert isinstance(c["ok"], bool) and c["title"] and c["detail"]
        if not c["ok"]:
            assert c["fix"] and "\n" not in c["fix"], c


@then(
    "the status says not running and has exactly the keys running, pid, started, uptime_s, connected, "
    "coordinator, job, device, last_error and log"
)
def _status_shape(ctx):
    st = _json(ctx["out"])
    assert set(st) == {
        "running",
        "pid",
        "started",
        "uptime_s",
        "connected",
        "coordinator",
        "job",
        "device",
        "last_error",
        "log",
    }
    assert st["running"] is False and st["pid"] is None and st["connected"] is False
    assert st["started"] is None and st["uptime_s"] is None and st["job"] is None
    assert st["log"].endswith("agent.log")


@given(parsers.parse("an agent log of {n:d} lines"))
def _log(n):
    config.ensure_home()
    agent.log_path().write_text("".join(f"line {i}\n" for i in range(1, n + 1)))


@then(parsers.parse("the output is the last {n:d} lines of the log"))
def _tail(ctx, n):
    assert ctx["out"].splitlines() == [f"line {i}" for i in range(31 - n, 31)]


@then("autostart is off and names a mechanism")
def _autostart_off(ctx):
    st = _json(ctx["out"])
    assert set(st) == {"enabled", "mechanism", "detail"}
    assert st["enabled"] is False and st["mechanism"] in ("launchd", "systemd", "schtasks")


@then(
    parsers.parse(
        "the autostart for {platform} uses the mechanism {mechanism} and the text or command in {what}"
    )
)
def _generators(platform, mechanism, what):
    assert autostart.mechanism(platform) == mechanism
    if platform == "darwin":
        plist = autostart.launchd_plist("/usr/bin/python3", "/opt/f/emfdtd_farm.py", "/tmp/a.log")
        assert "<key>Label</key>\n  <string>org.kicad-em-fdtd.farm-agent</string>" in plist
        assert "<key>RunAtLoad</key>\n  <true/>" in plist
        assert "<key>KeepAlive</key>\n  <false/>" in plist  # a stop is a stop
        order = ["/usr/bin/python3", "/opt/f/emfdtd_farm.py", "agent", "start"]
        positions = [plist.index(f"<string>{a}</string>") for a in order]
        assert positions == sorted(positions)
    elif platform == "linux":
        unit = autostart.systemd_unit("/usr/bin/python3", "/opt/f/emfdtd_farm.py")
        assert "Type=oneshot" in unit and "RemainAfterExit=yes" in unit
        assert "ExecStart=/usr/bin/python3 /opt/f/emfdtd_farm.py agent start" in unit
        assert "ExecStop=/usr/bin/python3 /opt/f/emfdtd_farm.py agent stop" in unit
        assert "WantedBy=default.target" in unit
    else:
        assert autostart.schtasks_create_argv("C:\\py\\python.exe", "C:\\f\\emfdtd_farm.py") == [
            "schtasks",
            "/Create",
            "/SC",
            "ONLOGON",
            "/TN",
            "EmfdtdFarmAgent",
            "/TR",
            '"C:\\py\\python.exe" "C:\\f\\emfdtd_farm.py" "agent" "start"',
            "/RL",
            "LIMITED",
            "/F",
        ]


@then(
    parsers.parse(
        "on {platform} autostart on writes its file, runs the install commands, and off removes them again"
    )
)
def _install(platform, monkeypatch, tmp_path):
    ran = []
    monkeypatch.setattr(autostart, "_run", lambda argv: ran.append(list(argv)) or (0, ""))
    home = tmp_path / "fakehome"
    py, tool, log = "/usr/bin/python3", "/opt/f/emfdtd_farm.py", "/tmp/a.log"
    detail = autostart.on(py, tool, log, platform=platform, home=home)
    assert detail
    if platform == "darwin":
        path = autostart.plist_path(home)
        assert path.read_text() == autostart.launchd_plist(py, tool, log)
        assert ran == [["launchctl", "bootstrap", f"gui/{os.getuid()}", str(path)]]
    elif platform == "linux":
        path = autostart.unit_path(home)
        assert path.read_text() == autostart.systemd_unit(py, tool)
        assert ran == [
            ["systemctl", "--user", "daemon-reload"],
            ["systemctl", "--user", "enable", autostart.UNIT],
        ]
    else:
        path = None
        assert ran == [autostart.schtasks_create_argv(py, tool)]
    ran.clear()
    autostart.off(platform=platform, home=home)
    if path is not None:
        assert not path.exists()
    expected = {
        "darwin": [["launchctl", "bootout", f"gui/{os.getuid()}/{autostart.LABEL}"]],
        "linux": [
            ["systemctl", "--user", "disable", autostart.UNIT],
            ["systemctl", "--user", "daemon-reload"],
        ],
        "win32": [["schtasks", "/Delete", "/TN", autostart.TASK, "/F"]],
    }[platform]
    assert ran == expected


# --- scenarios against a live coordinator -----------------------------------------------------------


@given(
    parsers.parse(
        'this machine is enrolled as the worker "{name}" and the agent is configured for the stub GPU'
    )
)
def _prepared(farm, ctx, name):
    policy.add_device("worker", name)
    pki.save_identity("worker", pki.issue("worker", name))
    config.set_enabled("worker", True)
    (config.home() / "coordinator.json").write_text(
        json.dumps({"host": "127.0.0.1", "port": farm.coord.port})
    )
    _stub_devices(agent.work_dir({}))
    agent.save(
        {
            "solver": STUB,
            "devices": ["stub gpu a"],
            "turn": {"mode": "none", "lock_command": []},
            "watchdog": 5,
        }
    )
    ctx["worker"] = name


@given("the worker identity is replaced by one issued by a different farm")
def _foreign(ctx, tmp_path):
    other = tmp_path / "other-farm"
    code = (
        f"import json,sys; sys.path.insert(0, {SRC!r}); from emfdtd_plugin.farm import pki; pki.init_ca(); "
        "print(json.dumps(pki.issue('worker', 'agent-box')))"
    )
    r = subprocess.run(
        [sys.executable, "-c", code],
        env={**os.environ, "EMFDTD_FARM_HOME": str(other)},
        capture_output=True,
        text=True,
        check=True,
    )
    pki.save_identity("worker", json.loads(r.stdout))


@then("every check passes")
def _all_ok(ctx):
    report = _json(ctx["out"])
    assert ctx["code"] == 0 and report["ok"] is True, report
    assert all(c["ok"] and c["fix"] == "" for c in report["checks"]), report


@then("the certificate check says the coordinator accepted us")
def _cert_ok(ctx):
    cert = {c["id"]: c for c in _json(ctx["out"])["checks"]}["certificate"]
    assert "connected to the coordinator" in cert["detail"]


@then(parsers.parse('the check "{cid}" fails saying "{text}"'))
def _check_fails(ctx, cid, text):
    check = {c["id"]: c for c in _json(ctx["out"])["checks"]}[cid]
    assert check["ok"] is False and text in check["detail"], check


@when("the user starts the agent")
def _start(ctx):
    ctx.pop("error", None)
    try:
        st = agent.start()
        ctx["pids"].append(st["pid"])
    except agent.AgentError as e:
        ctx["error"] = str(e)


@when("the user starts the agent again")
def _start_again(ctx):
    _start(ctx)


@when("the user restarts the agent")
def _restart(ctx):
    agent.stop(10)
    _start(ctx)


@when("the user stops the agent")
def _stop(ctx):
    ctx["stopped"] = agent.stop(10)


@then(parsers.parse('the coordinator sees the worker "{name}" online'))
def _sees(farm, name):
    _wait(lambda: farm.coord.sched.workers.get(name, {}).get("online"), 20, "the worker to connect")


@then("the agent status says running and connected to the coordinator with a pid and an uptime")
def _running(farm, ctx):
    st = _wait(
        lambda: (lambda s: s if s["running"] and s["connected"] else None)(agent.status()),
        20,
        "the agent to connect",
    )
    assert isinstance(st["pid"], int) and st["pid"] > 0
    assert st["uptime_s"] is not None and st["uptime_s"] >= 0
    assert st["coordinator"] == f"127.0.0.1:{farm.coord.port}"
    assert st["last_error"] is None
    if st["pid"] not in ctx["pids"]:
        ctx["pids"].append(st["pid"])


@then("the agent status says not running")
def _not_running():
    assert agent.status()["running"] is False


@then(parsers.parse('the second start fails saying "{text}"'))
@then(parsers.parse('the start fails saying "{text}"'))
def _start_fails(ctx, text):
    assert text in ctx["error"], ctx.get("error")


@then("no agent process was started")
def _none_started():
    assert not agent.pid_file().exists()


@then("the agent status shows a job with a phase")
def _job_phase(farm):
    st = _wait(lambda: (lambda s: s if s["job"] else None)(agent.status()), 20, "a job on the agent")
    assert set(st["job"]) == {"id", "label", "phase", "percent"}
    assert isinstance(st["job"]["phase"], str) and st["job"]["phase"]
    assert st["device"]


@given("a pid file naming a process that is gone")
def _dead_pid():
    p = subprocess.Popen([sys.executable, "-c", "pass"])
    p.wait()
    config.ensure_home()
    agent.pid_file().write_text(json.dumps({"pid": p.pid, "started": time.time()}))


@given("a pid file naming an unrelated running process")
def _unrelated(ctx):
    p = subprocess.Popen(["sleep", "60"])
    ctx["procs"].append(p)
    ctx["unrelated"] = p
    config.ensure_home()
    agent.pid_file().write_text(json.dumps({"pid": p.pid, "started": time.time()}))


@then("the unrelated process is still running")
def _unrelated_alive(ctx):
    assert ctx["unrelated"].poll() is None


@given("the worker role is turned off")
def _role_off():
    config.set_enabled("worker", False)


@when("the solver is running")
def _solver_running(farm):
    _wait(lambda: Path(farm.pid).exists(), 25, "the solver to start")
    time.sleep(0.2)


@then("the agent process is gone")
def _agent_gone(ctx):
    assert ctx["pids"], "no agent pid was recorded"
    _wait(lambda: not agent.pid_alive(ctx["pids"][-1]), 15, "the agent to end")


@then("the solver process is gone")
def _solver_gone(farm):
    pid = int(Path(farm.pid).read_text())
    _wait(lambda: not agent.pid_alive(pid), 15, "the solver to end")


@then("the pid changed")
def _pid_changed(ctx):
    assert len(set(ctx["pids"])) >= 2, ctx["pids"]


@then("the log mentions that the worker is connecting out")
def _log_mentions():
    _wait(lambda: "connecting out" in agent.log_path().read_text(errors="replace"), 15, "the log line")


# --- setup ---------------------------------------------------------------------------------------


@given(parsers.parse('an open invitation for a worker "{name}"'))
def _invitation(farm, ctx, name):
    ctx["invite_code"] = enroll.create_invite(name, "worker")
    server = farm.run(enroll.serve("127.0.0.1", 0, farm.bundle))
    ctx["invite_port"] = server.sockets[0].getsockname()[1]
    ctx["fingerprint"] = pki.fingerprint(farm.bundle["cert"])
    ctx["worker_name"] = name
    ctx["server"] = server


def _new_machine(ctx, tmp_path):
    home = tmp_path / ("machine-" + ctx.get("worker_name", "x"))
    env = {**os.environ, "EMFDTD_FARM_HOME": str(home)}
    ctx.setdefault("other_homes", []).append(env)
    ctx["env"] = env
    return env


def _tool(ctx, *args, input=None):
    r = subprocess.run(
        [sys.executable, TOOL, *args],
        env=ctx["env"],
        capture_output=True,
        text=True,
        input=input,
        timeout=120,
    )
    ctx.update(code=r.returncode, out=r.stdout, err=r.stderr)
    return r


@when("a new machine runs setup with every answer as a flag")
def _setup_flags(farm, ctx, tmp_path):
    _new_machine(ctx, tmp_path)
    _tool(
        ctx,
        "setup",
        "--yes",
        "--invitation",
        f"127.0.0.1:{ctx['invite_port']}",
        "--fingerprint",
        ctx["fingerprint"],
        "--code",
        ctx["invite_code"],
        "--coordinator",
        f"127.0.0.1:{farm.coord.port}",
        "--solver",
        STUB,
        "--device",
        "stub gpu a",
        "--no-lock",
        "--watchdog",
        "5",
        "--start",
    )


@when("a new machine runs setup and types its answers")
def _setup_typed(farm, ctx, tmp_path):
    _new_machine(ctx, tmp_path)
    answers = [
        "enable worker",
        f"127.0.0.1:{ctx['invite_port']}",
        ctx["fingerprint"],
        ctx["invite_code"],
        f"127.0.0.1:{farm.coord.port}",
        STUB,
        "stub gpu a",
        "n",
        "y",
    ]
    _tool(ctx, "setup", "--watchdog", "5", input="\n".join(answers) + "\n")


@when("a new machine runs setup with --yes and no other flags")
def _setup_nothing(ctx, tmp_path):
    _new_machine(ctx, tmp_path)
    _tool(ctx, "setup", "--yes")


@then("setup ends successfully and says the checks passed")
def _setup_ok(ctx):
    assert ctx["code"] == 0, (ctx["out"], ctx["err"])
    assert "all checks passed" in ctx["out"]


@then(parsers.parse('setup fails saying "{text}"'))
def _setup_fails(ctx, text):
    assert ctx["code"] != 0
    assert text in ctx["err"] + ctx["out"], (ctx["err"], ctx["out"])


@then(parsers.parse('the coordinator knows the worker "{name}" and sees it online'))
def _knows(farm, name):
    assert f"worker:{name}" in pki.members()
    _wait(lambda: farm.coord.sched.workers.get(name, {}).get("online"), 25, "the new worker to connect")


@then("that machine's agent status says running and connected")
def _machine_running(ctx):
    def poll():
        r = subprocess.run(
            [sys.executable, TOOL, "agent", "status", "--json"],
            env=ctx["env"],
            capture_output=True,
            text=True,
            timeout=60,
        )
        st = json.loads(r.stdout)
        return st if st["running"] and st["connected"] else None

    st = _wait(poll, 25, "the new machine's agent to connect")
    ctx["machine_pid"] = st["pid"]


@when("that machine stops its agent")
def _machine_stop(ctx):
    r = subprocess.run(
        [sys.executable, TOOL, "agent", "stop", "--json"],
        env=ctx["env"],
        capture_output=True,
        text=True,
        timeout=60,
    )
    assert r.returncode == 0, r.stdout + r.stderr


@then("that machine's agent process is gone")
def _machine_gone(ctx):
    _wait(lambda: not agent.pid_alive(ctx["machine_pid"]), 15, "the new machine's agent to end")


@when("setup is run on a Python that has no TLS 1.3")
def _setup_no_tls(ctx, monkeypatch):
    monkeypatch.setattr(agent.pki, "tls13_available", lambda: False)
    _cli(ctx, "setup --yes")


@then("the worker role is still off")
def _role_still_off():
    assert config.enabled("worker") is False


# --- doctor problems and the JSON of start and stop -----------------------------------------


@given("no worker identity is stored")
def _no_identity():
    (config.home() / "identity" / "worker.json").unlink()


@given("no coordinator address is stored")
def _no_address():
    (config.home() / "coordinator.json").unlink()


@given("the coordinator is stopped")
def _coordinator_stopped(farm):
    farm.run(farm.coord.stop())


@given("the recorded devices are deleted")
def _no_devices():
    (agent.work_dir({}) / "devices.json").unlink()


@given("the configured device matches nothing")
def _no_match():
    cfg = agent.load()
    cfg["devices"] = ["no such gpu"]
    agent.save(cfg)


@given("the work folder has almost no free disk")
def _low_disk(monkeypatch):
    usage = type("Usage", (), {"free": 1 << 30})()
    monkeypatch.setattr(agent.shutil, "disk_usage", lambda path: usage)


@given("this Python has no TLS 1.3")
def _no_tls(monkeypatch):
    monkeypatch.setattr(agent.pki, "tls13_available", lambda: False)


@then("that check has a fix")
def _has_fix(ctx):
    report = _json(ctx["out"])
    assert all(c["fix"] for c in report["checks"] if not c["ok"])


@then("the output is a status with the fixed keys and running true")
def _start_json(ctx):
    st = _json(ctx["out"])
    assert ctx["code"] == 0, ctx
    assert set(st) == {
        "running",
        "pid",
        "started",
        "uptime_s",
        "connected",
        "coordinator",
        "job",
        "device",
        "last_error",
        "log",
    }
    assert st["running"] is True and isinstance(st["pid"], int)


@then("the output has exactly the keys was_running, stopped and forced and says it stopped a running agent")
def _stop_json(ctx):
    r = _json(ctx["out"])
    assert set(r) == {"was_running", "stopped", "forced"}
    assert r["was_running"] is True and r["stopped"] is True


@then("the output says there was no running agent")
def _stop_none(ctx):
    r = _json(ctx["out"])
    assert r == {"was_running": False, "stopped": True, "forced": False}


@when("the agent is frozen so that it cannot answer a polite stop")
def _freeze(ctx):
    if sys.platform == "win32":
        pytest.skip("SIGSTOP does not exist on Windows (taskkill /F is used there)")
    import signal

    ctx["frozen"] = agent.status()["pid"]
    os.kill(ctx["frozen"], signal.SIGSTOP)


@when(parsers.parse("the user stops the agent with {n:d} seconds of patience"))
def _stop_patient(ctx, n):
    ctx["stopped"] = agent.stop(n)


@then("the stop had to kill it")
def _forced(ctx):
    assert ctx["stopped"]["was_running"] is True and ctx["stopped"]["forced"] is True
    assert ctx["stopped"]["stopped"] is True
