"""The scheduler's checkpoint rules without sockets (end to end: farm_checkpoint.feature)."""

import pytest

from emfdtd_plugin.farm.scheduler import Scheduler, SchedulerError

JOB = "a" * 32
DEV = {"uuid": "aaaa", "name": "GPU", "driver": "d", "mcells_per_s": 100.0, "ok": True}


def started(hold_s=3600.0, hello_features=("checkpoint",)):
    s = Scheduler(hold_s=hold_s)
    s.worker_hello("w1", [dict(DEV)], 0.0, max_minutes=45, features=hello_features)
    job = s.submit("client:alice", {}, "0" * 64, 10, "j", 0.0)
    s.input_received(job.id)
    ((job2, _, _),) = s.next_offers(1.0)
    assert job2 is job
    return s, job


def test_a_job_at_its_ceiling_is_paused_not_failed_and_the_device_is_free():
    s, job = started()
    assert s.pause(job.id, job.lease, 1200, "ceiling", 5.0)
    assert job.state == "paused" and job.ckpt == {"worker": "w1", "device": "aaaa", "step": 1200}
    assert "paused at step 1200 on w1 (ceiling reached)" in job.reason
    assert s.workers["w1"]["devices"]["aaaa"]["busy"] is None
    assert s.next_offers(6.0) == []  # nothing resumes by itself


def test_resume_is_pinned_to_the_worker_and_never_counts_as_an_attempt():
    s, job = started()
    s.pause(job.id, job.lease, 1200, "ceiling", 5.0)
    s.worker_hello("w2", [dict(DEV, uuid="bbbb")], 6.0, max_minutes=45, features=("checkpoint",))
    s.resume(job.id, "client:alice", 20, 7.0)
    assert job.state == "queued" and job.resume and job.run_limit == 20 and job.attempts == 0
    ((job2, worker, device),) = s.next_offers(8.0)
    assert (worker, device) == ("w1", "aaaa")  # never the idle w2: the checkpoint is on w1's disk


def test_resume_refusals():
    s, job = started()
    with pytest.raises(SchedulerError, match="only a paused job"):
        s.resume(job.id, "client:alice", None, 2.0)  # still running
    s.pause(job.id, job.lease, 10, "ceiling", 3.0)
    with pytest.raises(SchedulerError, match="only the submitter"):
        s.resume(job.id, "client:bob", None, 4.0)
    with pytest.raises(SchedulerError, match="allows at most 45 minutes per run"):
        s.resume(job.id, "client:alice", 60, 4.0)
    with pytest.raises(SchedulerError, match="above 0"):
        s.resume(job.id, "client:alice", 0, 4.0)


def test_a_worker_that_vanishes_after_a_checkpoint_keeps_the_job_for_itself_for_a_while():
    s, job = started(hold_s=120.0)
    assert s.checkpointed(job.id, job.lease, 500)
    s.worker_lost("w1", 10.0)
    assert job.state == "queued" and job.hold_worker == "w1" and job.resume and job.hold_until == 130.0
    s.next_offers(11.0)
    assert "waiting for w1 to come back (it holds the checkpoint at step 500)" in job.reason
    assert "after 2 min" in job.reason
    s.expire(131.0)  # the waiting time is over: any worker may start it again from step 0
    assert job.hold_worker is None and job.ckpt is None and not job.resume
    assert ("w1", job.id) in s.take_discards()


def test_without_a_checkpoint_a_lost_job_goes_to_any_worker_at_once():
    s, job = started()
    s.worker_lost("w1", 10.0)
    assert job.state == "queued" and job.hold_worker is None and not job.resume


def test_the_hello_of_a_returning_worker_decides():
    s, job = started()
    s.checkpointed(job.id, job.lease, 500)
    s.worker_lost("w1", 10.0)
    other = "b" * 32
    held = [{"job": job.id, "step": 520, "device": "aaaa"}, {"job": other, "step": 1, "device": None}]
    discard = s.worker_hello(
        "w1", [dict(DEV)], 20.0, max_minutes=45, features=("checkpoint",), checkpoints=held
    )
    assert discard == [other]  # a checkpoint of a job the farm does not know
    assert job.resume and job.ckpt["step"] == 520 and job.hold_worker == "w1"
    ((j, worker, device),) = s.next_offers(21.0)
    assert (j, worker, device) == (job, "w1", "aaaa")


def test_a_returning_worker_without_the_checkpoint_releases_the_job():
    s, job = started()
    s.checkpointed(job.id, job.lease, 500)
    s.worker_lost("w1", 10.0)
    s.worker_hello("w1", [dict(DEV)], 20.0, max_minutes=45, features=("checkpoint",), checkpoints=[])
    assert job.hold_worker is None and job.ckpt is None and not job.resume
    assert "the checkpoint is gone from w1" in job.reason


def test_a_paused_job_whose_checkpoint_is_gone_fails_with_a_reason():
    s, job = started()
    s.pause(job.id, job.lease, 77, "ceiling", 5.0)
    s.worker_lost("w1", 6.0)
    s.worker_hello("w1", [dict(DEV)], 7.0, max_minutes=45, features=("checkpoint",), checkpoints=[])
    assert job.state == "failed" and "no longer has its checkpoint" in job.reason


def test_a_checkpoint_that_cannot_be_used_restarts_the_job_from_step_0_without_costing_an_attempt():
    s, job = started()
    s.pause(job.id, job.lease, 77, "ceiling", 5.0)
    s.resume(job.id, "client:alice", None, 6.0)
    ((_, _, _),) = s.next_offers(7.0)
    assert job.attempts == 1
    assert s.fail(job.id, job.lease, "checkpoint refused: was written for other inputs", 8.0)
    assert job.state == "queued" and not job.resume and job.ckpt is None and job.attempts == 0
    assert "starting again from step 0" in job.reason


def test_restart_and_cancel_drop_the_checkpoint_and_tell_the_worker():
    s, job = started()
    s.pause(job.id, job.lease, 77, "ceiling", 5.0)
    s.restart(job.id, "client:alice", 6.0)
    assert job.state == "queued" and job.ckpt is None and ("w1", job.id) in s.take_discards()
    s2, job2 = started()
    s2.pause(job2.id, job2.lease, 77, "ceiling", 5.0)
    s2.cancel(job2.id, "client:alice", 6.0)
    assert job2.state == "cancelled" and ("w1", job2.id) in s2.take_discards()


def test_a_worker_that_stopped_politely_is_not_a_failed_attempt():
    s, job = started()
    assert job.attempts == 1
    s.pause(job.id, job.lease, 300, "stopped", 5.0)
    assert job.state == "queued" and job.attempts == 0 and job.hold_worker == "w1"
    assert "(the worker stopped)" in job.reason


def test_the_checkpoint_survives_a_coordinator_restart():
    s, job = started()
    s.checkpointed(job.id, job.lease, 900)
    rows = s.snapshot()
    again = Scheduler(hold_s=3600.0)
    again.restore(rows, 100.0)
    restored = again.jobs[job.id]
    assert restored.state == "queued" and restored.hold_worker == "w1" and restored.resume
    assert restored.ckpt["step"] == 900 and restored.hold_until == 3700.0
    s2, job2 = started()
    s2.pause(job2.id, job2.lease, 5, "ceiling", 5.0)
    third = Scheduler()
    third.restore(s2.snapshot(), 100.0)
    assert third.jobs[job2.id].state == "paused" and third.jobs[job2.id].ckpt["step"] == 5


def test_a_damaged_checkpoint_row_is_skipped_not_fatal():
    s, job = started()
    rows = s.snapshot()
    rows[0]["ckpt"] = {"worker": 5, "step": "x"}
    again = Scheduler()
    again.restore(rows, 1.0)
    assert again.jobs[job.id].ckpt is None


def test_the_resume_and_restart_commands_exist():
    from emfdtd_plugin.farm import cli

    p = cli._parser()
    a = p.parse_args(["resume", "J", "--max-minutes", "30", "--json"])
    assert (a.cmd, a.job, a.max_minutes, a.json) == ("resume", "J", 30.0, True)
    b = p.parse_args(["restart", "J"])
    assert (b.cmd, b.job) == ("restart", "J")
    c = p.parse_args(["serve", "--listen", "127.0.0.1", "--hold-minutes", "0"])
    assert c.hold_minutes == 0 and c.checkpoint_every == 300.0


def test_a_worker_offers_checkpoints_only_if_its_solver_names_the_flag(tmp_path):
    """Scenario: a solver built before checkpoints existed (its help has no --checkpoint-dir) must not be
    offered checkpoint jobs, which it would fail on the unknown flag."""
    import asyncio
    import stat
    import sys

    from emfdtd_plugin.farm.worker import Worker

    def probe(help_text):
        solver = tmp_path / f"solver{abs(hash(help_text))}.py"
        # like the real solver: usage with no arguments, "cannot open" for anything else
        solver.write_text(
            f"#!{sys.executable}\nimport sys\nprint({help_text!r} if len(sys.argv) == 1 else 'cannot open')\n"
        )
        solver.chmod(solver.stat().st_mode | stat.S_IEXEC)
        w = Worker({}, "127.0.0.1", 1, str(solver), tmp_path / "work", [], no_lock=True)
        asyncio.run(w.probe_checkpoint())
        return w.checkpoint

    assert probe("usage: emfdtd [--checkpoint-dir DIR]") is True
    assert probe("usage: emfdtd [--cpu]") is False
