import time
from pathlib import Path

from pytest_bdd import given, parsers, scenarios, then, when

from conftest import spec
from emfdtd_plugin.farm import client, store

scenarios("features/farm_checkpoint.feature")

# The stub writes a checkpoint every `checkpoint_lines` lines and ends when DIR/stop appears (exit 75).
LONG = {"lines": 400, "sleep": 0.05, "checkpoint_lines": 5}  # runs far past the ceiling
TWO_RUNS = {
    "lines": 60,
    "sleep": 0.05,
    "checkpoint_lines": 5,
}  # about 3 s: one pause at a 2.4 s ceiling, then done
SEVERAL_SECONDS = {"lines": 80, "sleep": 0.1, "checkpoint_lines": 5}


def folder(farm, name="w1"):
    return farm.tmp / f"work-{name}" / "jobs" / store.job_dir(farm.jobs[0]).name


def wait(check, seconds=15.0, what="the condition"):
    end = time.time() + seconds
    while time.time() < end:
        got = check()
        if got:
            return got
        time.sleep(0.1)
    raise AssertionError(f"timed out waiting for {what}")


def submit(farm, who, stub):
    farm.who = who
    farm.submit(who, spec(), **stub)


@given(parsers.parse('the worker "{name}" has no time limit'))
@when(parsers.parse('the worker "{name}" has no time limit'))
def _unlimited(farm, name):
    farm.kill_worker(name)
    farm.add_worker(name, "stub gpu a")


@given(parsers.parse('the worker "{name}" is replaced by one without checkpoint support'))
def _old_worker(farm, name):
    farm.kill_worker(name)
    farm.add_worker(name, "stub gpu a", limits={"max_minutes": 0.04}, checkpoint=False)


@given(parsers.parse("the coordinator waits only {n:d} seconds for a worker's checkpoint"))
def _short_hold(farm, n):
    farm.coord.sched.hold_s = float(n)


@when(parsers.parse('"{who}" submits a long job that writes checkpoints'))
def _long(farm, who):
    submit(farm, who, LONG)


@when(parsers.parse('"{who}" submits a job that needs two runs'))
def _two(farm, who):
    submit(farm, who, TWO_RUNS)


@when(parsers.parse('"{who}" submits a job that runs for a few seconds and writes checkpoints'))
def _few(farm, who):
    submit(farm, who, SEVERAL_SECONDS)


@when("the job is paused with a checkpoint step")
@then("the job is paused with a checkpoint step")
def _paused(farm):
    farm.paused = farm.wait_state(farm.who, farm.jobs[0], ("paused",))
    assert farm.paused["checkpoint_step"] and farm.paused["checkpoint_step"] > 0, farm.paused


@then(parsers.parse('the reason says "{text}" and how to resume it'))
def _reason(farm, text):
    reason = farm.paused["reason"]
    assert text in reason and f"emfdtd-farm resume {farm.jobs[0]} [--max-minutes N]" in reason, reason
    assert reason.startswith(f"paused at step {farm.paused['checkpoint_step']} on w1"), reason


@then("the worker still holds the checkpoint folder")
def _holds(farm):
    assert (folder(farm) / "ckpt").is_dir() and (folder(farm) / "checkpoint.json").exists()


@when("the job has written its first checkpoint")
def _first(farm):
    wait(
        lambda: (farm.job_info(farm.who, farm.jobs[0]) or {}).get("checkpoint_step"),
        what="the first checkpoint",
    )


@when(parsers.parse('"{who}" resumes it'))
def _resume(farm, who):
    farm.call(client.resume, who, farm.jobs[0])


@when(parsers.parse('"{who}" restarts it'))
def _restart(farm, who):
    farm.call(client.restart, who, farm.jobs[0])
    farm.after_restart = farm.coord.sched.jobs[farm.jobs[0]]


@when(parsers.parse('"{who}" cancels it'))
def _cancel_paused(farm, who):
    farm.call(client.cancel, who, farm.jobs[0])


@then(parsers.parse('"{who}" is refused when resuming it with "{text}"'))
def _refused_resume(farm, who, text):
    try:
        farm.call(client.resume, who, farm.jobs[0])
    except client.ClientError as e:
        assert text in str(e), str(e)
        return
    raise AssertionError("the resume was accepted")


@then(parsers.parse('resuming it with a ceiling of {n:d} minutes is refused with "{text}"'))
def _refused_ceiling(farm, n, text):
    try:
        farm.call(client.resume, farm.who, farm.jobs[0], n)
    except client.ClientError as e:
        assert text in str(e), str(e)
        return
    raise AssertionError("the resume was accepted")


@then("the coordinator has dropped the job's checkpoint")
def _dropped(farm):
    job = farm.after_restart
    assert job.ckpt is None and job.hold_worker is None and job.resume is False


@then("the worker no longer holds the checkpoint folder")
def _gone(farm):
    wait(lambda: not folder(farm).exists(), what="the checkpoint folder to be removed")


@when("the checkpoint files on the worker are damaged")
def _damage(farm):
    for name in ("ckpt.a", "ckpt.b"):
        path = folder(farm) / "ckpt" / name
        if path.exists():
            path.write_text("garbage")


def summary(farm):
    got = farm.call(client.fetch, farm.who, farm.jobs[0], str(farm.tmp / "got"))
    return Path(got["summary.txt"]).read_text()


@then("the summary says the run was resumed at the checkpoint step")
def _resumed(farm):
    text = summary(farm)
    assert "resumed at step" in text, text
    assert int(text.split("resumed at step")[1].split()[0]) > 0


@then("the summary does not say the run was resumed")
def _not_resumed(farm):
    assert "resumed at step" not in summary(farm)


@then("the worker reports one held checkpoint for the job")
def _reports(farm):
    held = farm.workers["w1"].held_checkpoints()
    assert [h["job"] for h in held] == [farm.jobs[0]] and held[0]["step"] > 0, held


@then("the worker removes it when it is older than seven days")
def _retention(farm):
    w = farm.workers["w1"]
    w.sweep_jobs(now=time.time() + 6 * 24 * 3600)
    assert w.held_checkpoints(), "kept at six days"
    w.sweep_jobs(now=time.time() + 8 * 24 * 3600)
    assert w.held_checkpoints() == [] and not folder(farm).exists()


@then(parsers.parse('the job fails saying "{text}"'))
def _fails(farm, text):
    info = farm.wait_state(farm.who, farm.jobs[0], ("failed", "paused"))
    assert info["state"] == "failed" and text in info["reason"], info


@when(parsers.parse('the worker "{name}" stops politely'))
def _polite(farm, name):
    farm.run(farm.workers[name].graceful_stop(), 30)
    farm.worker_tasks[name].cancel()


@then(parsers.parse('the job waits for "{name}" and says it holds the checkpoint'))
def _waits(farm, name):
    info = wait(
        lambda: (lambda i: i if i and i["state"] == "queued" and name in i["reason"] else None)(
            farm.job_info(farm.who, farm.jobs[0])
        ),
        what="the job to wait for its worker",
    )
    assert "checkpoint" in info["reason"] and info["checkpoint_step"] > 0, info


@when(parsers.parse('the worker "{name}" connects again'))
def _again(farm, name):
    farm.add_worker(name, "stub gpu a")


@then(parsers.parse('the job stays waiting for "{name}" while "{other}" is idle'))
def _stays(farm, name, other):
    wait(
        lambda: (farm.job_info(farm.who, farm.jobs[0]) or {}).get("state") == "queued",
        what="the job to be queued",
    )
    time.sleep(1.5)
    info = farm.job_info(farm.who, farm.jobs[0])
    assert info["state"] == "queued" and f"waiting for {name} to come back" in info["reason"], info
    idle = [d for d in farm.call(client.status, farm.who)["devices"] if d["worker"] == other]
    assert idle and idle[0]["state"] == "idle", idle


@then(parsers.parse('the job finishes on "{name}"'))
def _finishes_on(farm, name):
    farm.final = farm.wait_state(farm.who, farm.jobs[0], ("done", "failed"), timeout=40)
    assert farm.final["state"] == "done" and farm.final["worker"] == name, farm.final
