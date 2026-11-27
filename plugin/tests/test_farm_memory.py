import pytest
from pytest_bdd import given, parsers, scenarios, then, when

from emfdtd_plugin.farm.scheduler import Scheduler

scenarios("features/farm_memory.feature")
GIB = 1 << 30


def _dev(uuid, name, mcells, mem_gib):
    return {
        "uuid": uuid, "name": name, "driver": "d", "mcells_per_s": float(mcells), "ok": True,
        "memory_bytes": int(mem_gib * GIB),
    }  # fmt: skip


@pytest.fixture
def ctx():
    return {"s": Scheduler(), "now": 1000.0}


@given("a scheduler with a small fast GPU of 4 GiB and a big slow GPU of 16 GiB")
def _two(ctx):
    ctx["s"].worker_hello(
        "box", [_dev("small", "Small GPU", 2000, 4), _dev("big", "Big GPU", 500, 16)], ctx["now"]
    )


@given("a scheduler with one GPU that reports no memory")
def _unknown(ctx):
    ctx["s"].worker_hello("box", [_dev("small", "Small GPU", 2000, 0)], ctx["now"])


def _submit(ctx, gib, pin=None):
    spec = {"estimate": {"cells": 1e6, "steps": 1000, "memory_bytes": gib * GIB}}
    if pin:
        spec["pin"] = pin
    job = ctx["s"].submit("alice", spec, "0" * 64, 10, "t", ctx["now"])
    ctx["s"].input_received(job.id)
    ctx["job"] = job
    ctx["offers"] = ctx["s"].next_offers(ctx["now"])


@when(parsers.parse("a job that needs {gib:d} GiB of GPU memory is submitted and its input arrives"))
def _plain(ctx, gib):
    _submit(ctx, gib)


@when(
    parsers.parse("a job pinned to the small GPU that needs {gib:d} GiB is submitted and its input arrives")
)
def _pinned(ctx, gib):
    _submit(ctx, gib, pin="box:Small")


@then("the job is offered to the big GPU")
def _big(ctx):
    assert [d for _, _, d in ctx["offers"]] == ["big"]


@then("the job is offered to the small fast GPU")
def _small(ctx):
    assert [d for _, _, d in ctx["offers"]] == ["small"]


@then("the job is not offered and its reason names the memory and the largest GPU")
def _none_largest(ctx):
    assert ctx["offers"] == []
    why = ctx["s"]._wait_reason(ctx["job"])
    assert "20.0 GiB" in why and "16.0 GiB" in why, why


@then("the job is not offered and its reason names the memory and the small GPU")
def _none_small(ctx):
    assert ctx["offers"] == []
    why = ctx["s"]._wait_reason(ctx["job"])
    assert "5.0 GiB" in why and "4.0 GiB" in why and "Small" in why, why
