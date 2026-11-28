import json
import time
from pathlib import Path

import pytest
from pytest_bdd import given, parsers, scenarios, then, when

from emfdtd_plugin.farm import client, jobs

scenarios("features/farm_keep_fields.feature", "features/farm_keep_fields_job.feature")
MB = 1_000_000
LIMITS = {"max_steps": 10**9, "max_tier": "expert"}


def _spec(**extra):
    return {"profile": "bluetooth", "sets": {}, **extra}


# --- the job record --------------------------------------------------------------------------------


@given("a job that asks to keep its fields", target_fixture="job")
def _asks():
    return jobs.validate(_spec(keep_fields=True), None, LIMITS)


@given("a job that does not ask to keep its fields", target_fixture="job")
def _asks_not():
    return jobs.validate(_spec(), None, LIMITS)


@given(parsers.parse('a job whose keep_fields is the text "{text}"'), target_fixture="bad")
def _bad(text):
    return _spec(keep_fields=text)


@when("the worker builds the solver's command line", target_fixture="argv")
def _argv(job):
    return jobs.build_argv("emfdtd", "in.json", job, "proj", "0")


@then("it contains --keep-fields")
def _has(argv):
    assert "--keep-fields" in argv


@then("it does not contain --keep-fields")
def _has_not(argv):
    assert "--keep-fields" not in argv


@then("the validated job has no keep_fields entry")
def _no_entry(job):
    assert "keep_fields" not in job


@then("validating it is refused")
def _refuse(bad):
    with pytest.raises(jobs.JobError, match="keep_fields"):
        jobs.validate(bad, None, LIMITS)


# --- through the farm --------------------------------------------------------------------------------


@given(parsers.parse('the worker "{name}" has a result limit of {n:d} bytes'))
def _result_limit(farm, name, n):
    farm.kill_worker(name)
    farm.add_worker(name, "stub gpu a", limits={"max_result_bytes": n})


@when('"alice" submits a job that asks to keep its fields')
def _submit_keep(farm):
    farm.who = "alice"
    farm.submit("alice", _spec(keep_fields=True))


@when(parsers.parse('"alice" submits a job that asks to keep {mb:d} MB of fields'))
def _submit_big(farm, mb):
    farm.who = "alice"
    spec = _spec(keep_fields=True, estimate={"cells": 1e6, "steps": 1000, "field_bytes": mb * MB})
    try:
        farm.submit("alice", spec)
    except client.ClientError as e:
        farm.result["error"] = str(e)


@when(
    parsers.parse('"alice" submits a job that asks to keep fields and the solver writes {n:d} bytes of them')
)
def _submit_real_big(farm, n):
    farm.who = "alice"
    farm.submit("alice", _spec(keep_fields=True), field_bytes=n)


@then("the fetched folder holds the run manifest, the board and the field files in fields/")
def _folder(farm):
    dest = farm.tmp / "got"
    farm.got = farm.call(client.fetch, farm.who, farm.jobs[0], str(dest))
    for name in ("s11.s1p", "run.json", "board.json", "settings.json", "summary.txt"):
        assert (dest / name).is_file(), name
    for name in ("slice_e.f16", "volume_e.f16", "farfield.json"):
        assert (dest / "fields" / name).is_file(), name
    assert not list(dest.glob("fields.*")), "flattened names must be placed into fields/"
    assert json.loads((dest / "run.json").read_text())["fields"] is True


@then("the solver was asked for the fields")
def _asked(farm):
    assert "fields: kept" in (farm.tmp / "got" / "summary.txt").read_text()


@then("the submission is refused with the size and the result limit in the reason")
def _refused(farm):
    reason = farm.result["error"]
    assert "about 286 MB" in reason, reason  # 300e6 bytes in units of 2^20
    assert "result limit of 256 MB" in reason, reason


@then("the results hold only the plain files and summary.txt says the fields were not kept")
def _plain(farm):
    dest = farm.tmp / "got"
    farm.got = farm.call(client.fetch, farm.who, farm.jobs[0], str(dest))
    assert set(farm.got) == {"s11.s1p", "summary.txt", "settings.json", "run.log"}
    summary = (dest / "summary.txt").read_text()
    assert "fields requested but not kept:" in summary and "over the result limit of" in summary, summary
    assert "fields: kept" not in summary
    assert not (dest / "fields").exists()
    time.sleep(0.2)
    assert Path(dest / "s11.s1p").stat().st_size > 0


def _plan(spec):
    from emfdtd_plugin.farm.worker import Worker

    return Worker._checkpoint_plan(None, {"spec": spec, "checkpoint": {"every": 60, "resume": False}}, None)


@given("an offer with checkpointing for a job that asks to keep its fields", target_fixture="plan")
def _offer_keep():
    return _plan(_spec(keep_fields=True))


@then("the worker plans no checkpoint")
def _no_plan(plan):
    assert plan is None


@then("an offer with checkpointing for a plain job gets a checkpoint plan")
def _plain_plan():
    assert _plan(_spec()) == {"every": 60.0, "resume": False}
