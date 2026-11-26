"""Who runs what, where. Pure logic with an injected clock: no sockets, no files, no threads, so every rule
(fair share, leases, requeue, stale results) is tested directly.

A job moves receiving -> queued -> running -> done | failed | cancelled. A running job holds a lease that
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
# the solver's own progress line: "progress: 41200 steps, about 37%, 312 s"
SOLVER_PROGRESS = re.compile(r"^progress: (\d+) steps, about (\d{1,3})%")


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
        self.finished = None
        self.progress = ""
        self.percent = None  # from the solver's progress line when it prints one (an estimate)
        self.reason = ""
        self.files = {}
        self.cancel_requested = False

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
            "reason": self.reason,
        }


class Scheduler:
    def __init__(self, max_attempts=MAX_ATTEMPTS, heartbeat_timeout=HEARTBEAT_TIMEOUT):
        self.max_attempts = max_attempts
        self.heartbeat_timeout = heartbeat_timeout
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

    def worker_hello(self, name, devices, now):
        """Register (or re-register) a worker. Jobs it held before are lost: a reconnect starts clean."""
        self._lose_worker_jobs(name, now, "worker reconnected")
        self.workers[name] = {
            "devices": {d["uuid"]: dict(d, busy=None) for d in devices},
            "last": now,
            "online": True,
        }

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

    def _requeue(self, job, now, why):
        self._release_device(job)
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
            job.deadline = now + self.heartbeat_timeout
            dev["busy"] = job.id
            self._served[job.owner] = self._served.get(job.owner, 0) + 1
            if job.group and job.group not in self._groups:
                self._groups[job.group] = self.device_class(dev)
            offers.append((job, worker, uuid_))
        return offers

    def _idle_slots(self, job):
        slots = []
        pinned = self._groups.get(job.group) if job.group else None
        for name, w in self.workers.items():
            if not w["online"]:
                continue
            for uuid_, d in w["devices"].items():
                free = d.get("busy") is None and d.get("ok", True)
                if free and (pinned is None or self.device_class(d) == pinned):
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
        job.state, job.files, job.finished, job.lease = "done", dict(files), now, None
        return True

    def fail(self, job_id, lease, reason, now, retry=False):
        job = self._live(job_id, lease)
        if not job:
            return False
        if retry:
            self._requeue(job, now, reason)
        else:
            self._release_device(job)
            job.state, job.reason, job.finished, job.lease = "failed", reason, now, None
        return True

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
                        "state": "offline" if not w["online"] else "busy" if d.get("busy") else "idle",
                        "ok": d.get("ok", True),
                    }
                )
        return out
