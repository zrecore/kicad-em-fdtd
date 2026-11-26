import json

import pytest
from pytest_bdd import given, parsers, scenarios, then, when

from emfdtd_plugin.farm import jobs
from emfdtd_plugin.farm.scheduler import Scheduler

scenarios("features/farm_fit.feature")

FAST = 1600.0  # the "fast device" of the estimates below: cells * steps / (1600e6) * margin


def dev(uuid, name, mcells, driver="drv"):
    return {"uuid": uuid, "name": name, "driver": driver, "mcells_per_s": float(mcells), "ok": True}


@pytest.fixture
def ctx():
    return {"s": Scheduler(), "now": 1000.0, "jobs": [], "leases": {}}


def estimate_for(minutes):
    """cells and steps that take `minutes` on the fast device once the scheduler's margin is applied."""
    seconds = minutes * 60 / Scheduler.TIME_MARGIN
    cells = 20e6
    return {"cells": cells, "steps": seconds * FAST * 1e6 / cells}


@given(parsers.parse('a scheduler with a worker "{name}" at {m:d} Mcells/s that allows {minutes:d} minutes'))
def _bg(ctx, name, m, minutes):
    ctx["s"].worker_hello(
        name, [dev("u-" + name, "RTX 4060 Ti", m, "NVIDIA")], ctx["now"], max_minutes=minutes
    )


@given(parsers.parse('the worker "{name}" offers a device at {m:d} Mcells/s and allows {minutes:d} minutes'))
def _mac(ctx, name, m, minutes):
    ctx["s"].worker_hello(name, [dev("u-" + name, "Apple M1 Pro", m, "KK")], ctx["now"], max_minutes=minutes)


@given(parsers.parse('the worker "{name}" offers a device at {m:d} Mcells/s and tells no ceiling'))
def _old(ctx, name, m):
    ctx["s"].worker_hello(name, [dev("u-" + name, "Old box", m, "KK")], ctx["now"])


@given(
    parsers.parse('the worker "{name}" offers a device with no measured speed and allows {minutes:d} minutes')
)
def _unmeasured(ctx, name, minutes):
    ctx["s"].worker_hello(name, [dev("u-" + name, "New box", 0, "KK")], ctx["now"], max_minutes=minutes)


@given(parsers.parse('the worker "{name}" is gone'))
def _gone(ctx, name):
    ctx["s"].worker_lost(name, ctx["now"])


@when(
    parsers.parse(
        'a worker "{name}" that allows {minutes:d} minutes connects with a device at {m:d} Mcells/s'
    )
)
def _connects(ctx, name, minutes, m):
    ctx["s"].worker_hello(name, [dev("u-" + name, "Big box", m, "BB")], ctx["now"], max_minutes=minutes)


def _submit(ctx, owner, spec):
    job = ctx["s"].submit(owner, spec, "0" * 64, 10, "t", ctx["now"])
    ctx["s"].input_received(job.id)
    ctx["jobs"].append(job)


@when(
    parsers.parse(
        '"{owner}" submits a job that needs about {minutes:d} minutes on the fast device '
        "and its input arrives"
    )
)
def _est(ctx, owner, minutes):
    _submit(ctx, owner, {"estimate": estimate_for(minutes)})


@when(parsers.parse('"{owner}" submits a job with no estimate and its input arrives'))
def _noest(ctx, owner):
    _submit(ctx, owner, {})


@when(parsers.parse('"{owner}" submits a job pinned to "{pin}" and its input arrives'))
def _pin(ctx, owner, pin):
    _submit(ctx, owner, {"pin": pin})


@when(
    parsers.parse(
        '"{owner}" submits a job pinned to "{pin}" that needs about {minutes:d} minutes on the fast device '
        "and its input arrives"
    )
)
def _pin_est(ctx, owner, pin, minutes):
    _submit(ctx, owner, {"pin": pin, "estimate": estimate_for(minutes)})


@when("offers are made")
def _offers(ctx):
    for job, _w, _d in ctx["s"].next_offers(ctx["now"]):
        ctx["leases"][job.id] = job.lease


@when(parsers.parse('the job fails with "{text}"'))
def _fails(ctx, text):
    job = ctx["jobs"][-1]
    assert ctx["s"].fail(job.id, ctx["leases"][job.id], text, ctx["now"])


def _running_on(job):
    return job.worker if job.state == "running" else None


@then(parsers.parse('the job is running on "{name}"'))
def _run_on(ctx, name):
    assert _running_on(ctx["jobs"][-1]) == name, (ctx["jobs"][-1].state, ctx["jobs"][-1].reason)


@then(parsers.parse('the job is queued with the reason "{reason}"'))
def _queued(ctx, reason):
    job = ctx["jobs"][-1]
    assert job.state == "queued" and job.reason == reason, (job.state, job.reason)


@then(parsers.parse('the job that needs about {minutes:d} minutes is queued with the reason "{reason}"'))
def _queued_long(ctx, minutes, reason):
    job = ctx["jobs"][-1]
    assert job.state == "queued" and job.reason == reason, (job.state, job.reason)


@then(parsers.parse('the second job is queued with the reason "{reason}"'))
def _second(ctx, reason):
    job = ctx["jobs"][-1]
    assert job.state == "queued" and job.reason == reason, (job.state, job.reason)


@then(parsers.parse('the job\'s reason is "{reason}"'))
def _reason(ctx, reason):
    assert ctx["jobs"][-1].reason == reason, ctx["jobs"][-1].reason


LIMITS = {"max_steps": 10**6, "max_tier": "advanced"}


@then(parsers.parse("the job spec {text} is refused"))
def _refused(text):
    with pytest.raises(jobs.JobError):
        jobs.validate(json.loads(text), None, LIMITS)


@then("the worker's copy of a job spec with an estimate and a pin has neither")
def _stripped():
    spec = jobs.validate(
        {"sets": {}, "estimate": {"cells": 1e6, "steps": 1e5}, "pin": "mac:Apple"}, None, LIMITS
    )
    assert spec["estimate"] == {"cells": 1e6, "steps": 1e5} and spec["pin"] == "mac:Apple"
    sent = jobs.worker_spec(spec)
    assert "estimate" not in sent and "pin" not in sent and "sets" in sent


def test_the_device_listing_shows_each_workers_ceiling():
    s = Scheduler()
    s.worker_hello("new", [dev("u-new", "New box", 100)], 1000.0, max_minutes=45)
    s.worker_hello("old", [dev("u-old", "Old box", 100)], 1000.0)
    rows = {r["worker"]: r["ceiling_minutes"] for r in s.device_listing()}
    assert rows == {"new": 45, "old": None}
