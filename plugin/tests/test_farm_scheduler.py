import pytest
from pytest_bdd import given, parsers, scenarios, then, when

from emfdtd_plugin.farm.scheduler import Scheduler, SchedulerError

scenarios("features/farm_scheduler.feature")


def dev(uuid, name, mcells, driver="drv"):
    return {"uuid": uuid, "name": name, "driver": driver, "mcells_per_s": float(mcells), "ok": True}


@pytest.fixture
def ctx():
    return {"s": Scheduler(), "now": 1000.0, "jobs": [], "leases": {}, "offers": []}


def _submit(ctx, owner, n=1, **kw):
    for _ in range(n):
        j = ctx["s"].submit(owner, {}, "0" * 64, 10, "t", ctx["now"], **kw)
        ctx["jobs"].append(j)
    return ctx["jobs"][-n:]


def _arrive(ctx, jobs):
    for j in jobs:
        ctx["s"].input_received(j.id)


def _offer(ctx):
    offers = ctx["s"].next_offers(ctx["now"])
    for job, _worker, _device in offers:
        ctx["leases"][job.id] = job.lease
    ctx["offers"] += offers
    return offers


@given(parsers.parse('a scheduler with a worker "{name}" offering one RTX at {m:d} Mcells/s'))
def _bg(ctx, name, m):
    ctx["s"].worker_hello(name, [dev("u-rtx", "RTX 4060 Ti", m, "NVIDIA")], ctx["now"])


@given(parsers.parse('the worker "{name}" offers a second device at {m:d} Mcells/s'))
def _second(ctx, name, m):
    ctx["s"].worker_hello(name, [dev("u-m1", "Apple M1 Pro", m, "KK")], ctx["now"])


@given(parsers.parse('the worker "{name}" offers two devices of one kind at {m:d} Mcells/s'))
def _two(ctx, name, m):
    ctx["s"].worker_hello(
        name, [dev("u-a", "Apple M1 Pro", m, "KK"), dev("u-b", "Apple M1 Pro", m, "KK")], ctx["now"]
    )


@when(parsers.parse('"{owner}" submits a job and its input arrives'))
def _one(ctx, owner):
    _arrive(ctx, _submit(ctx, owner))


@when(parsers.parse('"{owner}" submits a job'))
def _one_no_input(ctx, owner):
    _submit(ctx, owner)


@when(parsers.parse('"{owner}" submits {n:d} jobs and their inputs arrive'))
def _many(ctx, owner, n):
    _arrive(ctx, _submit(ctx, owner, n))


@when(parsers.parse('"{owner}" submits {n:d} job and its input arrives'))
def _one_again(ctx, owner, n):
    _arrive(ctx, _submit(ctx, owner, n))


@when(parsers.parse('"{owner}" submits {n:d} jobs of one sweep and their inputs arrive'))
def _sweep(ctx, owner, n):
    jobs = [ctx["s"].submit(owner, {}, "0" * 64, 10, "t", ctx["now"], group="sweep-1") for _ in range(n)]
    ctx["jobs"] += jobs
    _arrive(ctx, jobs)


@when(parsers.parse('"{owner}" submits jobs until the limit of {n:d} waiting jobs is reached'))
def _limit(ctx, owner, n):
    for _ in range(n):
        _submit(ctx, owner, max_queued=n)
    ctx["limit"] = n
    ctx["owner"] = owner


@when("offers are made")
def _offers(ctx):
    _offer(ctx)


@when("the running job finishes")
def _finish(ctx):
    job = next(j for j in ctx["jobs"] if j.state == "running")
    assert ctx["s"].finish(job.id, ctx["leases"][job.id], {"s11.s1p": {}}, ctx["now"])


@when("the first offer runs and finishes 1 job")
def _first(ctx):
    _offer(ctx)
    _finish(ctx)


@when(parsers.parse('the worker "{name}" is lost'))
def _lost(ctx, name):
    ctx["s"].worker_lost(name, ctx["now"])


@when(parsers.parse('the worker "{name}" comes back and offers are made'))
def _back(ctx, name):
    ctx["s"].worker_hello(name, [dev("u-rtx", "RTX 4060 Ti", 1200, "NVIDIA")], ctx["now"])
    _offer(ctx)


@when(parsers.parse("{n:d} seconds pass without a heartbeat"))
def _quiet(ctx, n):
    ctx["now"] += n
    ctx["s"].expire(ctx["now"])


@when(parsers.parse("a heartbeat arrives every {step:d} seconds for {total:d} seconds"))
def _beats(ctx, step, total):
    for _ in range(step, total + 1, step):
        ctx["now"] += step
        ctx["s"].heartbeat("win", [{"uuid": "u-rtx", "ok": True}], ctx["now"])
        ctx["s"].expire(ctx["now"])


@when("the device reports not ok")
def _not_ok(ctx):
    ctx["s"].heartbeat("win", [{"uuid": "u-rtx", "ok": False}], ctx["now"])


@when("the worker reports a fatal failure")
def _fatal(ctx):
    job = ctx["jobs"][0]
    assert ctx["s"].fail(job.id, ctx["leases"][job.id], "bad settings", ctx["now"], retry=False)


@then(parsers.parse('the job is running on "{worker}" and the device is busy'))
def _running_on(ctx, worker):
    job = ctx["jobs"][0]
    assert job.state == "running" and job.worker == worker
    assert ctx["s"].workers[worker]["devices"][job.device]["busy"] == job.id


@then("no job is running")
def _none_running(ctx):
    assert not [j for j in ctx["jobs"] if j.state == "running"]


@then(parsers.parse("{r:d} job is running and {q:d} is queued"))
def _counts(ctx, r, q):
    assert sum(j.state == "running" for j in ctx["jobs"]) == r
    assert sum(j.state == "queued" for j in ctx["jobs"]) == q


@then(parsers.parse("{q:d} jobs are queued"))
def _queued(ctx, q):
    assert sum(j.state == "queued" for j in ctx["jobs"]) == q


@then(parsers.parse('the running job belongs to "{owner}"'))
def _belongs(ctx, owner):
    running = [j for j in ctx["jobs"] if j.state == "running"]
    assert len(running) == 1 and running[0].owner == owner


@then(parsers.parse("the job is queued with {n:d} attempt"))
def _requeued(ctx, n):
    job = ctx["jobs"][0]
    assert job.state == "queued" and job.attempts == n


@then("a result for the old lease is refused")
def _stale(ctx):
    job = ctx["jobs"][0]
    assert ctx["s"].finish(job.id, ctx["leases"][job.id], {}, ctx["now"]) is False
    assert job.state == "queued"


@then("the job has failed and says it gave up")
def _gave_up(ctx):
    job = ctx["jobs"][0]
    assert job.state == "failed" and "gave up" in job.reason


@then(parsers.parse('"{who}" cannot cancel it'))
def _cannot(ctx, who):
    with pytest.raises(SchedulerError):
        ctx["s"].cancel(ctx["jobs"][0].id, who, ctx["now"])


@then(parsers.parse('"{who}" can cancel it'))
def _can(ctx, who):
    ctx["s"].cancel(ctx["jobs"][0].id, who, ctx["now"])
    assert ctx["jobs"][0].state == "cancelled"


@then(parsers.parse('"{who}" sees {n:d} job and the admin view shows {m:d}'))
def _views(ctx, who, n, m):
    assert len(ctx["s"].listing(who)) == n
    assert len(ctx["s"].listing()) == m


@then("the next submission is refused")
def _refused(ctx):
    with pytest.raises(SchedulerError):
        _submit(ctx, ctx["owner"], max_queued=ctx["limit"])


@then("the job has failed and the device is idle")
def _failed_idle(ctx):
    job = ctx["jobs"][0]
    assert job.state == "failed"
    assert all(d["busy"] is None for d in ctx["s"].workers["win"]["devices"].values())


@then(parsers.parse('both jobs run on "{worker}"'))
def _both(ctx, worker):
    assert [j.worker for j in ctx["jobs"]] == [worker, worker]
    assert all(j.state == "running" for j in ctx["jobs"])


@when(parsers.parse("{n:d} days pass"))
def _days(ctx, n):
    ctx["now"] += n * 86400


@then(parsers.parse("forgetting jobs older than {n:d} days removes it"))
def _forget(ctx, n):
    gone = ctx["s"].forget_finished(n * 86400, ctx["now"])
    assert gone == [ctx["jobs"][0].id]
    assert ctx["s"].listing() == []


@when(parsers.parse('the solver reports "{line}"'))
def _progress_line(ctx, line):
    job = ctx["jobs"][0]
    assert ctx["s"].progress(job.id, ctx["leases"][job.id], line)


@then(parsers.parse("the job shows {n:d} percent"))
@then(parsers.parse("the job still shows {n:d} percent"))
def _percent(ctx, n):
    assert ctx["jobs"][0].percent == n
    assert ctx["s"].listing()[0]["percent"] == n
