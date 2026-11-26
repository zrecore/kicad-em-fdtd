"""Who runs what, where. Pure logic with an injected clock: no sockets, no files, no threads, so every rule
(fair share, leases, requeue, stale results) is tested directly.

A job moves receiving -> queued -> running -> done | failed | cancelled, or running -> paused -> queued (the
worker wrote a checkpoint at its time ceiling; the owner resumes it). A running job holds a lease that
heartbeats renew; if the worker goes quiet the lease expires and the job goes back to the queue (up to
MAX_ATTEMPTS), and a late result from the old lease is refused. Owners are served round-robin by how much
they have already been given, so one person's hundred-point sweep does not starve everyone else.
"""

import itertools
import re
import uuid

MAX_ATTEMPTS = 2
HEARTBEAT_TIMEOUT = 45.0  # seconds without a heartbeat before a worker's leases are lost
DEFAULT_MAX_QUEUED = 100
HOLD_SECONDS = 3600.0  # a job that lost its worker waits this long for it to come back with its checkpoint
RECEIVE_TIMEOUT = 3600.0  # a submission whose input never arrived is dropped after this long
# What is kept across a coordinator restart (progress text and percentages are not: a job starts over)
JOB_FIELDS = (
    "id", "owner", "spec", "input_sha256", "input_bytes", "label", "group",
    "state", "attempts", "created", "finished", "reason", "files",
    "ckpt", "hold_worker", "hold_until", "resume", "run_limit",
)  # fmt: skip
# the solver's own progress line: "progress: 41200 steps, about 37%, 312 s"
SOLVER_PROGRESS = re.compile(r"^progress: (\d+) steps, about (\d{1,3})%")
CHECKPOINT_REFUSED = "checkpoint refused:"  # a worker's fail reason when it cannot continue from a checkpoint
TIME_LIMIT = re.compile(r"^over the time limit of ([0-9.]+) minutes")
TIME_MARGIN = 1.25  # a run is slower than the speed measured at connect (a bigger grid, a shared GPU)


class SchedulerError(Exception):
    pass


class Job:
    def __init__(self, job_id, owner, spec, input_sha256, input_bytes, label, now, group=None):
        self.id = job_id
        self.owner = owner
        self.spec = spec
        self.input_sha256 = input_sha256
        self.input_bytes = input_bytes
        self.label = label
        self.group = group
        self.state = "receiving"
        self.attempts = 0
        self.lease = None
        self.worker = None
        self.device = None
        self.deadline = None
        self.created = now
        self.started = None  # when the current attempt began running (for "running 6 min")
        self.finished = None
        self.progress = ""
        self.percent = None  # from the solver's progress line when it prints one (an estimate)
        self.reason = ""
        self.files = {}
        self.cancel_requested = False
        self._waiting = ""  # the wait reason this scheduler wrote (so it can clear exactly that)
        # Checkpoints live on the worker that wrote them and resume only there (docs/checkpoint.md).
        self.ckpt = None  # {"worker", "device", "step"}: the newest checkpoint the worker reported
        self.hold_worker = None  # the job may only run on this worker (it holds the checkpoint)
        self.hold_until = (
            None  # when waiting for that worker is given up (None: for ever, an explicit resume)
        )
        self.resume = False  # the next run continues from the checkpoint
        self.run_limit = None  # minutes: a shorter ceiling for the next run only (resume --max-minutes)

    def public(self):
        return {
            "job": self.id,
            "owner": self.owner,
            "label": self.label,
            "state": self.state,
            "attempts": self.attempts,
            "worker": self.worker,
            "device": self.device,
            "progress": self.progress,
            "percent": self.percent,
            "started": self.started,
            "reason": self.reason,
            "checkpoint_step": self.ckpt["step"] if self.ckpt else None,
        }


class Scheduler:
    TIME_MARGIN = TIME_MARGIN

    def __init__(self, max_attempts=MAX_ATTEMPTS, heartbeat_timeout=HEARTBEAT_TIMEOUT, hold_s=HOLD_SECONDS):
        self.max_attempts = max_attempts
        self.heartbeat_timeout = heartbeat_timeout
        self.hold_s = hold_s
        self._discards = []  # (worker, job id): checkpoints to tell workers to delete (see take_discards)
        self.jobs = {}
        self.workers = {}  # name -> {"devices": {uuid: dict}, "last": time, "online": bool}
        self._served = {}  # owner -> jobs started so far (the fair-share counter)
        self._groups = {}  # group -> device class the group is pinned to
        self._order = itertools.count()

    # --- jobs ---------------------------------------------------------------------------------

    def submit(
        self, owner, spec, input_sha256, input_bytes, label, now, max_queued=DEFAULT_MAX_QUEUED, group=None
    ):
        open_jobs = sum(
            1 for j in self.jobs.values() if j.owner == owner and j.state in ("receiving", "queued")
        )
        if open_jobs >= max_queued:
            raise SchedulerError(f"{owner} already has {open_jobs} waiting jobs (the limit is {max_queued})")
        job = Job(uuid.uuid4().hex, owner, spec, input_sha256, input_bytes, label, now, group)
        job._seq = next(self._order)
        self.jobs[job.id] = job
        return job

    def input_received(self, job_id):
        job = self._job(job_id)
        if job.state == "receiving":
            job.state = "queued"

    def cancel(self, job_id, requester, now):
        """The owner cancels a job. Returns the (worker, job id) to tell when it was running, else None."""
        job = self._job(job_id)
        if job.owner != requester:
            raise SchedulerError("only the submitter may cancel a job")
        if job.state in ("done", "failed", "cancelled"):
            return None
        was_running = job.state == "running"
        worker = job.worker
        self._drop_checkpoint(job)
        job.state = "cancelled"
        job.reason = "cancelled by the submitter"
        job.finished = now
        job.lease = None
        return (worker, job.id) if was_running else None

    def _job(self, job_id):
        try:
            return self.jobs[job_id]
        except KeyError:
            raise SchedulerError("no such job") from None

    # --- workers ------------------------------------------------------------------------------

    def worker_hello(self, name, devices, now, max_minutes=None, features=(), checkpoints=()):
        """Register (or re-register) a worker. Jobs it held before are lost: a reconnect starts clean (a job
        with a checkpoint on this worker waits for it instead, see _requeue).
        `max_minutes` is the longest job the worker will run (None: it did not say, and nothing is skipped
        on its account until a time-limit failure teaches us). `checkpoints` is what the worker holds,
        [{"job", "step", "device"}]. Returns the ids of jobs whose checkpoint the worker should delete."""
        self._lose_worker_jobs(name, now, "worker reconnected")
        learned = None if max_minutes is not None else (self.workers.get(name) or {}).get("learned")
        self.workers[name] = {
            "devices": {d["uuid"]: dict(d, busy=None) for d in devices},
            "last": now,
            "online": True,
            "ceiling": max_minutes,
            "learned": learned,
            "features": set(features),
        }
        return self._reconcile_checkpoints(name, checkpoints, now)

    def _reconcile_checkpoints(self, name, checkpoints, now):
        discard, wanted = [], set()
        for c in checkpoints:
            job = self.jobs.get(c["job"])
            if job is None or job.state in ("done", "failed", "cancelled", "receiving"):
                discard.append(c["job"])
            elif job.state == "paused" and job.ckpt and job.ckpt["worker"] == name:
                job.ckpt["step"] = c["step"]
                wanted.add(job.id)
            elif job.state == "queued" and job.hold_worker == name:
                job.ckpt = {
                    "worker": name,
                    "device": c.get("device") or (job.ckpt or {}).get("device"),
                    "step": c["step"],
                }
                job.resume = True
                wanted.add(job.id)
            else:  # it runs elsewhere, or restarts from step 0 anywhere: this checkpoint is not used
                discard.append(c["job"])
        for job in self.jobs.values():
            if job.id in wanted:
                continue
            if job.state == "queued" and job.hold_worker == name:  # the worker came back without it
                self._clear_hold(job, "the checkpoint is gone from " + name + "; starting again from step 0")
            elif job.state == "paused" and job.ckpt and job.ckpt["worker"] == name:
                job.state, job.finished = "failed", now
                job.reason = (
                    f"paused at step {job.ckpt['step']}, but {name} no longer has its checkpoint "
                    "(kept for 7 days); submit the job again"
                )
                job.ckpt = None
        return discard

    def set_ceiling(self, name, max_minutes):
        """The coordinator's word on the longest job `name` will run (its own, the policy's, a gate's)."""
        w = self.workers.get(name)
        if w:
            w["ceiling"] = max_minutes

    def heartbeat(self, name, devices, now):
        w = self.workers.get(name)
        if not w or not w["online"]:
            return
        w["last"] = now
        for d in devices:
            known = w["devices"].get(d.get("uuid"))
            if known is not None:
                known["ok"] = bool(d.get("ok", True))
                speed = d.get("mcells_per_s")
                if isinstance(speed, (int, float)) and not isinstance(speed, bool) and speed > 0:
                    known["mcells_per_s"] = float(speed)
        for job in self.jobs.values():
            if job.state == "running" and job.worker == name:
                job.deadline = now + self.heartbeat_timeout

    def worker_lost(self, name, now, why="worker disconnected"):
        w = self.workers.get(name)
        if w:
            w["online"] = False
        return self._lose_worker_jobs(name, now, why)

    def _lose_worker_jobs(self, name, now, why):
        lost = []
        for job in self.jobs.values():
            if job.state == "running" and job.worker == name:
                self._requeue(job, now, why)
                lost.append(job.id)
        w = self.workers.get(name)
        if w:
            for d in w["devices"].values():
                d["busy"] = None
        return lost

    def _hold(self, job, now, why):
        """Back to the queue, for the worker that holds the job's checkpoint only, for `hold_s` seconds."""
        self._release_device(job)
        job.state, job.lease, job.worker, job.device = "queued", None, None, None
        job.hold_worker = job.ckpt["worker"]
        job.hold_until = now + self.hold_s
        job.resume = True
        job.progress = ""
        job.reason = why

    def _clear_hold(self, job, why=""):
        """Give up the checkpoint: the job may start again from step 0 on any worker."""
        worker = job.ckpt["worker"] if job.ckpt else job.hold_worker
        if worker:
            self._discards.append((worker, job.id))
        job.hold_worker = job.hold_until = None
        job.resume = False
        job.ckpt = None
        job.reason = why

    def _drop_checkpoint(self, job):
        if job.ckpt or job.hold_worker:
            self._clear_hold(job)

    def take_discards(self):
        """The (worker, job id) pairs whose checkpoint should be deleted (the coordinator tells them)."""
        out, self._discards = self._discards, []
        return out

    def checkpointed(self, job_id, lease, step):
        job = self._live(job_id, lease)
        if not job:
            return False
        job.ckpt = {"worker": job.worker, "device": job.device, "step": int(step)}
        return True

    def pause(self, job_id, lease, step, why, now):
        """The solver wrote a checkpoint and left. At the time ceiling the job waits for its owner to resume;
        when the worker itself stopped (an agent restart) the job waits for that worker to come back."""
        job = self._live(job_id, lease)
        if not job:
            return False
        worker = job.worker
        job.ckpt = {"worker": worker, "device": job.device, "step": int(step)}
        if why == "ceiling":
            self._release_device(job)
            job.state, job.lease, job.device, job.progress = "paused", None, None, ""
            job.reason = (
                f"paused at step {step} on {worker} (ceiling reached); "
                f"resume with: emfdtd-farm resume {job.id} [--max-minutes N]"
            )
        else:
            job.attempts = max(0, job.attempts - 1)  # a worker that stopped politely is not a failed attempt
            self._hold(
                job, now, f"paused at step {step} (the worker stopped); it resumes when {worker} is back"
            )
        return True

    def resume(self, job_id, requester, max_minutes, now):
        """The owner continues a paused job. It runs again only on the worker that holds the checkpoint;
        `max_minutes` shortens that run's ceiling (never beyond what the worker itself allows)."""
        job = self._job(job_id)
        if job.owner != requester:
            raise SchedulerError("only the submitter may resume a job")
        if job.state != "paused" or not job.ckpt:
            raise SchedulerError(f"only a paused job can be resumed (this one is {job.state})")
        worker = job.ckpt["worker"]
        w = self.workers.get(worker)
        if max_minutes is not None:
            if isinstance(max_minutes, bool) or not isinstance(max_minutes, (int, float)) or max_minutes <= 0:
                raise SchedulerError("--max-minutes must be a number above 0")
            ceiling = self._ceiling(w) if w else None
            if ceiling and max_minutes > ceiling:
                raise SchedulerError(
                    f"{worker} allows at most {ceiling:g} minutes per run: resume with --max-minutes "
                    f"{ceiling:g} or less (a run that reaches its ceiling pauses again; resume it again)"
                )
            job.run_limit = float(max_minutes)
        job.state, job.resume = "queued", True
        job.hold_worker, job.hold_until = worker, None
        job.attempts = max(0, job.attempts - 1)  # resuming a paused job is not a failed attempt
        job.reason = ""

    def restart(self, job_id, requester, now):
        """Drop the checkpoint of a paused (or waiting) job and start it again from step 0 on any worker."""
        job = self._job(job_id)
        if job.owner != requester:
            raise SchedulerError("only the submitter may restart a job")
        if job.state not in ("paused", "queued") or not (job.ckpt or job.hold_worker):
            raise SchedulerError("only a paused job, or one waiting for its checkpoint, can be restarted")
        self._clear_hold(job, "restarting from step 0 (the checkpoint was discarded)")
        job.state = "queued"
        job.attempts = max(0, job.attempts - 1)

    def _requeue(self, job, now, why):
        if (
            job.ckpt
            and job.ckpt.get("worker") == job.worker
            and self.hold_s > 0
            and job.attempts < self.max_attempts
        ):
            self._hold(
                job,
                now,
                f"{why}; waiting for {job.ckpt['worker']} to come back "
                f"(checkpoint at step {job.ckpt['step']})",
            )
            return
        self._release_device(job)
        self._drop_checkpoint(job)  # not held for a worker: any checkpoint is abandoned
        job.lease = None
        job.worker = None
        job.device = None
        if job.attempts >= self.max_attempts:
            job.state = "failed"
            job.reason = f"{why}; gave up after {job.attempts} attempts"
            job.finished = now
        else:
            job.state = "queued"
            job.reason = why

    def unoffer(self, job_id, lease):
        """The offer could not be made (the GPU turn never came): back to the queue, attempt not counted."""
        job = self._live(job_id, lease)
        if not job:
            return False
        self._release_device(job)
        job.attempts = max(0, job.attempts - 1)
        job.state, job.lease, job.worker, job.device = "queued", None, None, None
        return True

    def _release_device(self, job):
        w = self.workers.get(job.worker)
        if w and job.device in w["devices"] and w["devices"][job.device].get("busy") == job.id:
            w["devices"][job.device]["busy"] = None

    def expire(self, now):
        """Requeue the jobs of workers that went quiet. Returns the ids of the jobs affected."""
        lost = []
        for job in self.jobs.values():
            if job.state == "receiving" and now - job.created > RECEIVE_TIMEOUT:
                job.state, job.reason, job.finished = "failed", "the input never arrived", now
        for job in self.jobs.values():
            if job.state == "queued" and job.hold_until is not None and now > job.hold_until:
                worker = job.hold_worker
                self._clear_hold(
                    job,
                    f"gave up waiting for {worker}'s checkpoint after {self.hold_s / 60:g} min; "
                    "starting from step 0",
                )
        for name, w in self.workers.items():
            if w["online"] and now - w["last"] > self.heartbeat_timeout:
                w["online"] = False
                lost += self._lose_worker_jobs(name, now, "worker stopped answering")
        return lost

    # --- placement ----------------------------------------------------------------------------

    @staticmethod
    def device_class(dev):
        return (dev.get("name", ""), dev.get("driver", ""))

    def next_offers(self, now):
        """Start as many queued jobs as there are idle devices. Returns [(job, worker, device uuid)]."""
        offers = []
        while True:
            pick = self._pick_job()
            if pick is None:
                break
            job, slot = pick
            worker, uuid_ = slot
            dev = self.workers[worker]["devices"][uuid_]
            job.state = "running"
            job.attempts += 1
            job.lease = uuid.uuid4().hex
            job.worker, job.device = worker, uuid_
            job.started = now
            job.deadline = now + self.heartbeat_timeout
            if job._waiting and job.reason == job._waiting:
                job.reason = ""
            job._waiting = ""
            job.hold_worker = job.hold_until = (
                None  # it runs now; `resume` stays set for this attempt's offer
            )
            dev["busy"] = job.id
            self._served[job.owner] = self._served.get(job.owner, 0) + 1
            if job.group and job.group not in self._groups:
                self._groups[job.group] = self.device_class(dev)
            offers.append((job, worker, uuid_))
        for job in self.jobs.values():
            if job.state == "queued" and (job.spec.get("estimate") or job.spec.get("pin") or job.hold_worker):
                why = self._wait_reason(job)
                if why:
                    job.reason = job._waiting = why
        return offers

    def _wait_reason(self, job):
        """Why a queued job with an estimate or a pin is not running, in words for the person who sent it."""
        pin_worker, pin_device = self._pin_parts(job)
        if job.hold_worker:
            step = (job.ckpt or {}).get("step")
            w = self.workers.get(pin_worker)
            if not w or not w["online"]:
                text = f"waiting for {pin_worker} to come back (it holds the checkpoint at step {step})"
            else:
                text = f"waiting for {pin_worker} (busy); it holds the checkpoint at step {step}"
            if job.hold_until is not None:
                text += f"; starts again from step 0 on any worker after {self.hold_s / 60:g} min"
            return text
        if pin_worker:
            w = self.workers.get(pin_worker)
            if not w or not w["online"]:
                return f"waiting for {pin_worker} (not connected)"
            devs = [d for d in w["devices"].values() if self._device_matches(d, pin_device)]
            if not devs:
                return f"{pin_worker} has no device matching '{pin_device}'"
            ok = [d for d in devs if self._fits(job, w, d)]
            if not ok:
                need = min((n for n in (self._need_seconds(job, d) for d in devs) if n), default=None)
                ceiling = self._ceiling(w)
                if need and ceiling:
                    return f"needs about {round(need / 60)} min but {pin_worker} allows {ceiling:g}"
                return f"waiting for {pin_worker} (busy)"
            return f"waiting for {pin_worker} (busy)"
        online = {n: w for n, w in self.workers.items() if w["online"]}
        if not online:
            return "no worker is online"
        fitting = [n for n, w in online.items() if any(self._fits(job, w, d) for d in w["devices"].values())]
        if fitting:
            return "waiting for " + " or ".join(fitting) + " (busy)"
        need = min(
            (n for w in online.values() for d in w["devices"].values() if (n := self._need_seconds(job, d))),
            default=None,
        )
        allow = [f"{self._ceiling(w):g} ({n})" for n, w in online.items() if self._ceiling(w)]
        if need and allow:
            return f"needs about {round(need / 60)} min; the online workers allow " + " and ".join(allow)
        return ""

    @staticmethod
    def _ceiling(w):
        """The longest job (minutes) a worker will take, or None when nobody has said."""
        known = [c for c in (w.get("ceiling"), w.get("learned")) if c]
        return min(known) if known else None

    def _slowest_speed(self):
        speeds = [
            d["mcells_per_s"]
            for w in self.workers.values()
            if w["online"]
            for d in w["devices"].values()
            if d.get("mcells_per_s", 0) > 0
        ]
        return min(speeds) if speeds else None

    def _need_seconds(self, job, dev):
        """Expected seconds for the job on `dev`, or None when it has no estimate or the device's speed
        is unknown and no other is measured either."""
        est = job.spec.get("estimate")
        if not est:
            return None
        speed = dev.get("mcells_per_s", 0.0) or self._slowest_speed()
        if not speed:
            return None
        return est["cells"] * est["steps"] / (speed * 1e6) * self.TIME_MARGIN

    def _fits(self, job, w, dev):
        ceiling = self._ceiling(w)
        need = self._need_seconds(job, dev)
        return ceiling is None or need is None or need <= ceiling * 60

    @staticmethod
    def _pin_parts(job):
        if job.hold_worker:  # a checkpoint lives on one worker and one device
            return job.hold_worker, (job.ckpt or {}).get("device")
        pin = job.spec.get("pin")
        if not pin:
            return None, None
        worker, _, device = pin.partition(":")
        return worker, device or None

    @staticmethod
    def _device_matches(dev, part):
        if not part:
            return True
        part = part.lower()
        return part in dev.get("name", "").lower() or dev.get("uuid", "").startswith(part)

    def _idle_slots(self, job):
        slots = []
        pinned = self._groups.get(job.group) if job.group else None
        pin_worker, pin_device = self._pin_parts(job)
        for name, w in self.workers.items():
            if not w["online"] or (pin_worker and name != pin_worker):
                continue
            for uuid_, d in w["devices"].items():
                free = d.get("busy") is None and d.get("ok", True)
                if not free or (pinned is not None and self.device_class(d) != pinned):
                    continue
                if not self._device_matches(d, pin_device) or (not job.resume and not self._fits(job, w, d)):
                    continue
                slots.append((d.get("mcells_per_s", 0.0), name, uuid_))
        slots.sort(key=lambda s: (-s[0], s[1], s[2]))  # fastest first, then by name for a stable order
        return [(n, u) for _, n, u in slots]

    def _pick_job(self):
        queued = [j for j in self.jobs.values() if j.state == "queued"]
        # fair share: the owner served least so far first, then the oldest job of that owner
        for job in sorted(queued, key=lambda j: (self._served.get(j.owner, 0), j._seq)):
            slots = self._idle_slots(job)
            if slots:
                return job, slots[0]
        return None

    # --- results ------------------------------------------------------------------------------

    def _live(self, job_id, lease):
        job = self.jobs.get(job_id)
        return job if job and job.state == "running" and job.lease == lease else None

    def progress(self, job_id, lease, line):
        job = self._live(job_id, lease)
        if job:
            job.progress = line[-200:]
            m = SOLVER_PROGRESS.match(line)
            if m:
                job.percent = min(100, int(m.group(2)))
        return job is not None

    def finish(self, job_id, lease, files, now):
        """Record a result. False (and nothing changes) if the lease is no longer the job's current one."""
        job = self._live(job_id, lease)
        if not job:
            return False
        self._release_device(job)
        self._drop_checkpoint(job)
        job.state, job.files, job.finished, job.lease = "done", dict(files), now, None
        job.resume = False
        return True

    def fail(self, job_id, lease, reason, now, retry=False):
        job = self._live(job_id, lease)
        if not job:
            return False
        if job.resume and reason.startswith(CHECKPOINT_REFUSED):
            # the worker could not continue from the checkpoint (damaged, or the setup changed): start again
            self._release_device(job)
            self._drop_checkpoint(job)
            job.state, job.lease, job.worker, job.device, job.resume = "queued", None, None, None, False
            job.attempts = max(0, job.attempts - 1)
            job.reason = reason + "; starting again from step 0"
            return True
        m = TIME_LIMIT.match(reason)
        if m:
            w = self.workers.get(job.worker)
            if w:
                limit = float(m.group(1))
                w["learned"] = min(limit, w["learned"]) if w.get("learned") else limit
            reason = (
                f"{reason} on {job.worker} (its ceiling). Raise the ceiling on that machine "
                "(emfdtd-farm agent configure --max-minutes N) or send the job to a faster/longer-allowed "
                "worker with --device."
            )
        if retry:
            self._requeue(job, now, reason)
        else:
            self._release_device(job)
            self._drop_checkpoint(job)
            job.state, job.reason, job.finished, job.lease = "failed", reason, now, None
            job.resume = False
        return True

    def snapshot(self):
        """The jobs as plain data, oldest first, for the coordinator to save."""
        return [
            {f: getattr(j, f) for f in JOB_FIELDS} for j in sorted(self.jobs.values(), key=lambda j: j._seq)
        ]

    def restore(self, rows, now):
        """Load jobs saved by snapshot(). A job that was running has lost its worker (its watchdog killed
        the solver): it goes back to the queue with its attempt counted, or fails when attempts are used up.
        Fair-share counters start again from zero."""

        def created(row):
            value = row.get("created")
            return value if isinstance(value, (int, float)) and not isinstance(value, bool) else 0

        for row in sorted(
            (r for r in rows if isinstance(r, dict)), key=created
        ):  # a damaged file never stops a start
            try:
                job = Job(
                    row["id"], row["owner"], row["spec"], row["input_sha256"], row["input_bytes"],
                    row.get("label", ""), now, row.get("group"),
                )  # fmt: skip
                job.created = row.get("created", now)
                job.state = row["state"]
                job.attempts = int(row.get("attempts", 0))
                job.finished = row.get("finished")
                job.reason = row.get("reason", "")
                job.files = row.get("files", {})
                ck = row.get("ckpt")
                if (
                    isinstance(ck, dict)
                    and isinstance(ck.get("worker"), str)
                    and isinstance(ck.get("step"), int)
                ):
                    job.ckpt = {"worker": ck["worker"], "device": ck.get("device"), "step": ck["step"]}
                hw = row.get("hold_worker")
                job.hold_worker = hw if isinstance(hw, str) and job.ckpt else None
                hu = row.get("hold_until")
                job.hold_until = hu if isinstance(hu, (int, float)) and not isinstance(hu, bool) else None
                job.resume = bool(row.get("resume")) and job.ckpt is not None
                rl = row.get("run_limit")
                job.run_limit = (
                    rl if isinstance(rl, (int, float)) and not isinstance(rl, bool) and rl > 0 else None
                )
            except (KeyError, TypeError, ValueError):
                continue  # a damaged row is skipped, never fatal
            job._seq = next(self._order)
            if job.state == "running":
                if job.ckpt:
                    job.worker = job.ckpt[
                        "worker"
                    ]  # so _requeue can hold the job for the worker's checkpoint
                self._requeue(job, now, "the coordinator restarted")
            self.jobs[job.id] = job

    def forget_finished(self, older_than, now):
        """Drop records of jobs that ended more than `older_than` seconds ago. Returns their ids."""
        old = [
            j.id
            for j in self.jobs.values()
            if j.state in ("done", "failed", "cancelled")
            and j.finished is not None
            and now - j.finished > older_than
        ]
        for job_id in old:
            del self.jobs[job_id]
        return old

    # --- views --------------------------------------------------------------------------------

    def listing(self, owner=None):
        """Jobs for status. An owner sees only their own."""
        return [j.public() for j in self.jobs.values() if owner is None or j.owner == owner]

    def device_listing(self):
        out = []
        for name, w in self.workers.items():
            for uuid_, d in w["devices"].items():
                out.append(
                    {
                        "worker": name,
                        "uuid": uuid_,
                        "name": d.get("name", ""),
                        "driver": d.get("driver", ""),
                        "mcells_per_s": d.get("mcells_per_s", 0.0),
                        "ceiling_minutes": self._ceiling(w),  # None: the worker has not said (an old worker)
                        "state": "offline" if not w["online"] else "busy" if d.get("busy") else "idle",
                        "ok": d.get("ok", True),
                    }
                )
        return out
