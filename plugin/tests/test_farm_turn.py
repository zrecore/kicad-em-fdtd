import asyncio
import json
import os
import subprocess
import sys
import time
from pathlib import Path

import pytest
from pytest_bdd import given, parsers, scenarios, then, when

from emfdtd_plugin.farm import config, jobs, policy
from emfdtd_plugin.farm.worker import Worker, clamp_limits, parse_devices
from farm_stub.paths import GATE_HOLD, GATE_RELEASE, LOCKWRAP, STUB

scenarios("features/farm_turn.feature")


def _worker_args(farm_):
    return {
        "bundle": {},
        "host": "127.0.0.1",
        "port": 1,
        "solver": STUB,
        "work_root": farm_.tmp / "w",
        "allow": [],
    }


@then("constructing a worker with no turn mechanism fails")
def _none():
    with pytest.raises(ValueError):
        Worker({}, "127.0.0.1", 1, STUB, Path("."), [])


@then("constructing a worker with two turn mechanisms fails")
def _two():
    with pytest.raises(ValueError):
        Worker({}, "127.0.0.1", 1, STUB, Path("."), [], lock_command=["x"], no_lock=True)
    with pytest.raises(ValueError):
        Worker({}, "127.0.0.1", 1, STUB, Path("."), [], turn_from_coordinator=True, no_lock=True)


@then("offered limits are clamped to the worker's own ceilings")
def _clamp():
    caps = {
        "max_minutes": 60,
        "max_steps": 1000,
        "max_input_bytes": 1000,
        "max_result_bytes": 5000,
        "max_tier": "advanced",
    }
    out = clamp_limits(
        {
            "max_minutes": 9999,
            "max_steps": 10,
            "max_input_bytes": 10**9,
            "max_tier": "expert",
            "max_result_bytes": True,
        },
        caps,
    )
    assert out == {**caps, "max_steps": 10}
    assert clamp_limits({"max_tier": "common", "max_minutes": 30, "max_input_bytes": -5}, caps) == {
        **caps, "max_tier": "common", "max_minutes": 30,
    }  # fmt: skip
    assert clamp_limits({"max_tier": "nonsense", "max_minutes": "30"}, caps) == caps
    assert jobs.TIERS.index(clamp_limits({"max_tier": "expert"}, caps)["max_tier"]) <= jobs.TIERS.index(
        "advanced"
    )


@given(parsers.parse('a worker "{name}" offering "{allow}" that wraps every GPU use in a lock command'))
def _locked_worker(farm, name, allow):  # noqa: F811
    farm.lock_log = farm.tmp / "lock.log"
    farm.add_worker(name, allow, lock_command=[sys.executable, LOCKWRAP, str(farm.lock_log), "0"])


@when("the worker has stayed connected through several heartbeats")
def _beats(farm):  # noqa: F811
    time.sleep(1.8)  # the heartbeat interval in these tests is 0.3 s


def _lock_lines(farm):  # noqa: F811
    return farm.lock_log.read_text().splitlines() if farm.lock_log.exists() else []


@then("the lock command was never used just to connect")
def _not_to_connect(farm):  # noqa: F811
    assert _lock_lines(farm) == [], _lock_lines(farm)


@then("the lock command has not been called")
def _not_called(farm):  # noqa: F811
    assert _lock_lines(farm) == [], _lock_lines(farm)


@then("the lock command wrapped exactly one device check and the solve")
def _wrapped(farm):  # noqa: F811
    lines = _lock_lines(farm)
    assert len(lines) == 2, lines
    assert sum("--list-devices" in x for x in lines) == 1, lines  # the check before the job, inside the turn
    assert sum("--gpu" in x for x in lines) == 1, lines  # the solve


@then("it never wrapped the settings listing and the solver was never asked for a dry run")
def _no_settings(farm):  # noqa: F811
    assert not any("--list-settings" in x for x in _lock_lines(farm))
    assert not any("--dry-run" in x for x in _lock_lines(farm))


@given(parsers.parse('the coordinator holds the turn for the worker "{name}"'))
def _gate(farm, name):  # noqa: F811
    farm.gate_log = farm.tmp / "gate.log"
    flag = str(farm.tmp / "gate.flag")
    farm.coord.gates[name] = {
        "hold": [sys.executable, GATE_HOLD, str(farm.gate_log), "{worker}-{job}", flag],
        "release": [sys.executable, GATE_RELEASE, str(farm.gate_log), "{worker}-{job}", flag],
    }


@given(parsers.parse('a worker "{name}" offering "{allow}" that relies on the coordinator\'s turn'))
def _turn_worker(farm, name, allow):  # noqa: F811
    # the devices the worker offers were recorded earlier, during a turn (`devices --save`)
    out = subprocess.run([STUB, "--list-devices"], capture_output=True, text=True, check=True).stdout
    work = farm.tmp / ("work-" + name)
    work.mkdir(parents=True, exist_ok=True)
    Worker({}, "", 0, STUB, work, [], no_lock=True).save_devices(
        [{**d, "vulkan": list(d["vulkan"])} for d in parse_devices(out)]
    )
    farm.add_worker(name, allow, turn_from_coordinator=True)


@then("the turn was held before the job and released after it")
def _turn_order(farm):  # noqa: F811
    end = time.time() + 5
    while time.time() < end and not any(
        x.startswith("release") for x in farm.gate_log.read_text().splitlines()
    ):
        time.sleep(0.1)
    lines = farm.gate_log.read_text().splitlines()
    end = time.time() + 5
    while time.time() < end and not any(
        x.startswith("hold-exit") for x in farm.gate_log.read_text().splitlines()
    ):
        time.sleep(0.1)
    lines = farm.gate_log.read_text().splitlines()
    # the hold command is released first and then exits by itself (it can clean up), it is not killed
    assert [x.split()[0] for x in lines] == ["hold", "release", "hold-exit"], lines
    assert not farm.coord.turns


@then("the turn is released")
def _released(farm):  # noqa: F811
    end = time.time() + 5
    while time.time() < end and not any(
        x.startswith("release") for x in farm.gate_log.read_text().splitlines()
    ):
        time.sleep(0.1)
    assert [x.split()[0] for x in farm.gate_log.read_text().splitlines()][:2] == ["hold", "release"]


@then("restricting a file on Windows removes inheritance and grants only the current user")
def _icacls(monkeypatch, tmp_path):
    calls = []
    monkeypatch.setattr(config.sys, "platform", "win32")
    monkeypatch.setenv("USERNAME", "alex")
    monkeypatch.setenv("USERDOMAIN", "BOX")
    monkeypatch.setattr(config.subprocess, "run", lambda args, **kw: calls.append(args))
    config.restrict(tmp_path / "key.json")
    config.restrict(tmp_path, directory=True)
    assert calls[0] == ["icacls", str(tmp_path / "key.json"), "/inheritance:r", "/grant:r", "BOX\\alex:F"]
    assert calls[1] == ["icacls", str(tmp_path), "/inheritance:r", "/grant:r", "BOX\\alex:(OI)(CI)F"]


def test_policy_import_is_used():  # keeps the policy import (limits come from it in the clamp test)
    assert policy.DEFAULT_LIMITS["max_tier"] == "advanced"


# --- partitions, a hold command that ends, a capped gate ---------------------------------------------


def _gate_lines(farm):
    return [x.split() for x in farm.gate_log.read_text().splitlines()] if farm.gate_log.exists() else []


def _set_gate(farm, name, hold_extra=(), cap=None):
    farm.gate_log = farm.tmp / "gate.log"
    flag = str(farm.tmp / "gate.flag")
    gate = {
        "hold": [sys.executable, GATE_HOLD, str(farm.gate_log), "{worker}-{job}", flag, *hold_extra],
        "release": [sys.executable, GATE_RELEASE, str(farm.gate_log), "{worker}-{job}", flag],
    }
    if cap is not None:
        gate["max_minutes"] = cap
    farm.coord.gates[name] = gate


@given(parsers.parse("a farm whose coordinator pings every {p:f} seconds with a {m:f} second turn margin"))
def _fast_coordinator(farm, p, m):
    farm.coord.ping_every = p
    farm.coord.turn_margin_s = m
    farm.coord.sched.heartbeat_timeout = (
        2.0  # the lease of a silent worker expires after 2 s here (45 s normally)
    )


@given(
    parsers.parse(
        'the coordinator holds the turn for the worker "{name}" but its hold command ends after {n:f} seconds'
    )
)
def _short_hold(farm, name, n):
    _set_gate(farm, name, hold_extra=(str(n),))


@given(parsers.parse('the coordinator holds the turn for the worker "{name}" with a cap of {m:f} minutes'))
def _capped(farm, name, m):
    _set_gate(farm, name, cap=m)


@given(
    parsers.parse(
        'a worker "{name}" offering "{allow}" that relies on the coordinator\'s turn, '
        "reaches it through a relay and has a {w:f} second watchdog"
    )
)
def _partitionable_worker(farm, name, allow, w):
    out = subprocess.run([STUB, "--list-devices"], capture_output=True, text=True, check=True).stdout
    work = farm.tmp / ("work-" + name)
    work.mkdir(parents=True, exist_ok=True)
    Worker({}, "", 0, STUB, work, [], no_lock=True).save_devices(
        [{**d, "vulkan": list(d["vulkan"])} for d in parse_devices(out)]
    )
    farm.start_proxy()
    farm.add_worker(name, allow, turn_from_coordinator=True, via_proxy=True, watchdog_s=w)


@when("the network between them goes silent")
def _partition(farm):
    farm.t_silent = time.time()
    farm.proxy.pause()


def _wait_dead(pid_file, timeout):
    pid = int(Path(pid_file).read_text())
    end = time.time() + timeout
    while time.time() < end:
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return time.time()
        time.sleep(0.02)
    return None


@then(parsers.parse("the solver is killed within {n:d} seconds"))
def _killed(farm, n):
    farm.t_dead = _wait_dead(farm.pid, n + 1)
    assert farm.t_dead is not None, "the solver is still running"
    if hasattr(farm, "t_silent"):
        assert farm.t_dead - farm.t_silent <= n, farm.t_dead - farm.t_silent


@then("the turn is given back only after the solver was dead")
def _after_dead(farm):
    end = time.time() + 10
    while time.time() < end and not any(x[0] == "release" for x in _gate_lines(farm)):
        time.sleep(0.1)
    rel = [x for x in _gate_lines(farm) if x[0] == "release"]
    path = config.home() / "audit.log"
    actions = [json.loads(x)["action"] for x in path.read_text().splitlines()] if path.exists() else []
    assert rel, (
        _gate_lines(farm),
        actions,
        {j: (v.state, v.attempts) for j, v in farm.coord.sched.jobs.items()},
    )
    assert float(rel[0][2]) >= farm.t_dead - 0.05, (float(rel[0][2]), farm.t_dead)


@then("the audit log says the turn ended early")
def _audit(farm):
    end = time.time() + 6
    lines = []
    while time.time() < end:
        path = config.home() / "audit.log"
        lines = [json.loads(x) for x in path.read_text().splitlines()] if path.exists() else []
        if any(x["action"] == "turn_ended_early" for x in lines):
            return
        time.sleep(0.1)
    raise AssertionError([x["action"] for x in lines])


# --- a cancel that arrives before the solve ------------------------------------------------------------


@given(
    parsers.parse(
        'a worker "{name}" offering "{allow}" whose lock command makes it wait {n:d} seconds for the turn'
    )
)
def _slow_lock_worker(farm, name, allow, n):
    farm.lock_log = farm.tmp / "lock.log"
    # the first call is the device listing at connect time: let that one through at once
    farm.add_worker(name, allow, lock_command=[sys.executable, LOCKWRAP, str(farm.lock_log), "0"])
    w = farm.workers[name]
    w.lock_command[3] = str(n)  # from now on the turn takes this long


@when('"alice" submits a job that would write a solver pid file')
def _submit_pid(farm):
    farm.who = "alice"
    farm.pid = str(farm.tmp / "solver.pid")
    farm.submit("alice", {"profile": "bluetooth", "sets": {}}, pid_file=farm.pid)


@when("the worker is waiting for its lock command")
def _waiting(farm):
    end = time.time() + 15
    pidf = Path(str(farm.lock_log) + ".pid")
    t0 = pidf.stat().st_mtime if pidf.exists() else 0
    while time.time() < end:  # a new wrapper process (the pre-job device check) has started
        if pidf.exists() and pidf.stat().st_mtime > t0 + 0.0 and _job_state(farm) == "running":
            break
        time.sleep(0.05)
    farm.wait_pid = int(pidf.read_text())
    farm.t_cancel = time.time()


def _job_state(farm):
    info = farm.job_info("alice", farm.jobs[0])
    return info["state"] if info else None


@then(parsers.parse("the waiting lock command is gone within {n:d} seconds"))
def _lock_gone(farm, n):
    end = time.time() + n
    while time.time() < end:
        try:
            os.kill(farm.wait_pid, 0)
        except ProcessLookupError:
            return
        time.sleep(0.05)
    raise AssertionError("the lock command is still waiting")


@then("the solver never starts")
def _never(farm):
    time.sleep(3)
    assert not Path(farm.pid).exists()


@then("the job is cancelled")
def _is_cancelled(farm):
    assert farm.wait_state("alice", farm.jobs[0], ("cancelled",))["state"] == "cancelled"


@given(
    parsers.parse(
        'a worker "{name}" offering "{allow}" whose lock command always waits {n:d} seconds, '
        "started with a {limit:d} second limit to connect"
    )
)
def _busy_gpu_worker(farm, name, allow, n, limit):  # noqa: F811
    farm.lock_log = farm.tmp / "lock.log"
    t0 = time.time()
    # someone else holds the lock for the next while: every call through the lock command waits
    farm.add_worker(name, allow, lock_command=[sys.executable, LOCKWRAP, str(farm.lock_log), str(n)])
    took = time.time() - t0
    assert took < limit, f"the worker needed {took:.1f} s to connect: it waited for the lock"


@then("a worker that has no recorded devices refuses to connect and names the command")
def _no_devices(farm):  # noqa: F811
    work = farm.tmp / "work-empty"
    w = Worker({}, "127.0.0.1", 1, STUB, work, ["stub gpu a"], no_lock=True)
    with pytest.raises(RuntimeError) as e:
        w.recorded_devices()
    assert "emfdtd-farm devices" in str(e.value) and "--save" in str(e.value)


@then(
    "a device listing through a lock command may wait for its turn for hours and one without a lock may not"
)
def _lock_wait_allowance():
    async def go():
        seen = {}

        async def fake_child(self, job, argv, cwd, timeout):
            seen[tuple(argv[:1])] = timeout
            return 0, ""

        for kwargs in ({"lock_command": ["/usr/bin/true"]}, {"no_lock": True}):
            w = Worker({}, "127.0.0.1", 1, STUB, Path("."), [], **kwargs)
            w._child = fake_child.__get__(w)
            await w._capture("--list-devices", gpu=True)
            seen[str(sorted(kwargs))] = list(seen.values())[-1]
        return seen

    seen = asyncio.run(go())
    assert seen["['lock_command']"] > 3600  # waits for a shared GPU's lock: hours
    assert seen["['no_lock']"] == 60  # a call with no lock to wait for stays short
