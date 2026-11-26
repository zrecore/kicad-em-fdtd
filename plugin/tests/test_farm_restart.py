import json
import subprocess
import sys
import time
from pathlib import Path

from pytest_bdd import given, parsers, scenarios, then, when

from emfdtd_plugin.farm import cli, client, config
from emfdtd_plugin.farm.scheduler import RECEIVE_TIMEOUT
from emfdtd_plugin.farm.worker import Worker, parse_devices
from farm_stub.paths import GATE_HOLD, GATE_RELEASE, STUB

scenarios("features/farm_restart.feature")


def _spec():
    return {"profile": "bluetooth", "sets": {"mesh_preset": "medium"}}


@when(parsers.parse('"{who}" submits {n:d} jobs while no worker is connected'))
def _submit_waiting(farm, who, n):
    farm.who = who
    for _ in range(n):
        farm.submit(who, _spec())


@when("the coordinator is restarted")
def _restart(farm):
    farm.restart_coordinator()


@then("both jobs are listed as queued")
def _listed(farm):
    jobs = farm.call(client.status, farm.who)["jobs"]
    assert sorted(j["job"] for j in jobs) == sorted(farm.jobs)
    assert all(j["state"] == "queued" for j in jobs), jobs


@then("both jobs finish")
def _both_finish(farm):
    for job in farm.jobs:
        assert farm.wait_state(farm.who, job, ("done", "failed"), timeout=40)["state"] == "done"


@when("the job is running again after the restart")
def _running_again(farm):
    end = time.time() + 20
    info = None
    while time.time() < end:
        info = farm.job_info(farm.who, farm.jobs[0])
        if info and info["state"] == "running" and info["attempts"] == 2:
            return
        time.sleep(0.1)
    raise AssertionError(f"the job never ran its second attempt: {info}")


@then('"alice" still sees the job as done and can fetch its results')
def _still_done(farm):
    info = farm.job_info("alice", farm.jobs[0])
    assert info and info["state"] == "done", info
    got = farm.call(client.fetch, "alice", farm.jobs[0], str(farm.tmp / "after-restart"))
    assert "s11.s1p" in got and Path(got["s11.s1p"]).read_text().startswith("# Hz")


@when('"alice" announces a job but never sends its input')
def _announce(farm):
    farm.who = "alice"
    path = farm.snapshot()
    reply = farm.run(
        client._ask(
            farm.clients["alice"],
            "127.0.0.1",
            farm.coord.port,
            {
                "type": "submit",
                "spec": _spec(),
                "input_sha256": "0" * 64,
                "input_bytes": path.stat().st_size,
                "label": "never",
            },
        )
    )
    farm.jobs.append(reply["job"])
    assert farm.job_info("alice", reply["job"])["state"] == "receiving"


@when("the announcement is older than the receive timeout")
def _age(farm):
    farm.coord.sched.jobs[farm.jobs[0]].created -= RECEIVE_TIMEOUT + 10  # the expiry loop runs every 0.2 s


# --- gates.json is read at every dispatch ----------------------------------------------------------------


@given("a farm with a coordinator that re-reads gates.json")
def _gate_reader(farm):
    farm.coord.gates_loader = cli.read_gates
    farm.gate_log = farm.tmp / "gate.log"
    farm.gate_flag = str(farm.tmp / "gate.flag")


@given(parsers.parse('a worker "{name}" offering "{allow}" that relies on the coordinator\'s turn'))
def _turn_worker(farm, name, allow):
    out = subprocess.run([STUB, "--list-devices"], capture_output=True, text=True, check=True).stdout
    work = farm.tmp / ("work-" + name)
    work.mkdir(parents=True, exist_ok=True)
    Worker({}, "", 0, STUB, work, [], no_lock=True).save_devices(
        [{**d, "vulkan": list(d["vulkan"])} for d in parse_devices(out)]
    )
    farm.add_worker(name, allow, turn_from_coordinator=True)


@when(parsers.parse('a gate for "{name}" is written to gates.json'))
def _write_gate(farm, name):
    label = "{worker}-{job}-{lease}"
    flag = farm.gate_flag + "-" + label  # one flag file per turn: the release of one must not end the next
    log = str(farm.gate_log)
    gate = {
        name: {
            "hold": [sys.executable, GATE_HOLD, log, label, flag],
            "release": [sys.executable, GATE_RELEASE, log, label, flag],
        }
    }
    (config.home() / "gates.json").write_text(json.dumps(gate))


@when("gates.json is overwritten with broken JSON")
def _break_gates(farm):
    (config.home() / "gates.json").write_text("{ this is not json")


@then("the second job finishes")
def _second(farm):
    assert farm.wait_state(farm.who, farm.jobs[1], ("done", "failed"), timeout=30)["state"] == "done"


def _turn_cycles(farm, n):
    """n turns, each taken, released and its hold command ended, in that order (turns of two jobs may
    overlap in the log by a few milliseconds, so the order is checked per turn)."""
    end = time.time() + 8
    turns = {}
    while time.time() < end:
        turns = {}
        if farm.gate_log.exists():
            for line in farm.gate_log.read_text().splitlines():
                kind, label = line.split()[:2]
                turns.setdefault(label, []).append(kind)
        if len(turns) >= n and all(len(v) >= 3 for v in turns.values()):
            break
        time.sleep(0.1)
    assert len(turns) == n, turns
    assert all(v == ["hold", "release", "hold-exit"] for v in turns.values()), turns


@then("the turn was taken and given back for it")
def _turn_once(farm):
    _turn_cycles(farm, 1)


@then("the turn was taken and given back twice")
def _turn_twice(farm):
    _turn_cycles(farm, 2)


@then("the coordinator is still serving")
def _serving(farm):
    assert farm.call(client.status, farm.who or "alice")["jobs"] is not None


@when("jobs.json is overwritten with damaged data")
def _damage_jobs(farm):
    farm.who = "alice"
    (config.home() / "jobs.json").write_text('[{"id": "x"}, "junk", 5, {"id": "y", "created": "never"}')


@then("jobs.json holds that job as queued with its spec and no progress")
def _jobs_file(farm):
    rows = json.loads((config.home() / "jobs.json").read_text())
    assert [r["id"] for r in rows] == farm.jobs
    row = rows[0]
    assert (
        row["state"] == "queued" and row["spec"]["profile"] == "bluetooth" and row["owner"] == "client:alice"
    )
    assert "progress" not in row and "percent" not in row and "lease" not in row


@then(parsers.parse('the job fails with "{text}" after {n:d} attempts'))
@then(parsers.parse('the job fails with "{text}" after {n:d} attempt'))
def _fails_after(farm, text, n):
    info = farm.wait_state(farm.who, farm.jobs[0], ("failed", "done"), timeout=30)
    assert info["state"] == "failed" and text in info["reason"] and info["attempts"] == n, info
