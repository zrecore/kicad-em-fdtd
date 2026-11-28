"""The worker: dials out to the coordinator (it never listens), offers the GPUs the user chose, and runs jobs.

It runs one fixed solver binary with an argument list it builds itself from a validated job record
(jobs.py), in a private folder per job, with a time limit, a scrubbed environment and a process group it can
kill. Files go over their own connections. If the coordinator or the network goes away it reconnects with
backoff; any job it was running is the coordinator's to requeue.
"""

import asyncio
import contextlib
import json
import os
import re
import shutil
import ssl
import statistics
import time
from pathlib import Path

from . import jobs, pki, policy, procs, protocol, store

DEVICE_LINE = re.compile(
    r"^(?P<index>\d+|-)\s{2}(?P<name>.+?)\s{2}uuid (?P<uuid>[0-9a-f]*)\s{2}(?P<kind>\S+)\s{2}"
    r"(?P<mem>[\d.]+) GiB\s{2}vulkan (?P<major>\d+)\.(?P<minor>\d+)\s{2}driver (?P<driver>.*?)"
    r"(?P<unusable>\s{2}\(below Vulkan 1\.2: not usable\))?$"
)
PROGRESS_PERCENT = re.compile(r"^progress: \d+ steps, about (\d{1,3})%")
SPEED = re.compile(r"in ([\d.]+) s \(([\d.]+) Mcells/s\)")  # the solver's end-of-run line
MIN_RATE_RUN_S = 180.0  # a shorter run is mostly setup: it does not say how fast the device is
RATE_HISTORY = 5  # the device speed is the median of this many recent full runs
ESTIMATE_MB = re.compile(r"estimate:.*?(\d+(?:\.\d+)?) (MB|GB)")
CKPT_LINE = re.compile(r"^checkpoint: step (\d+) written")  # the solver's line after each checkpoint
PAUSED_LINE = re.compile(r"^paused: checkpoint at step (\d+)")
MAX_LOG_BYTES = 5 * 1024 * 1024
PROGRESS_EVERY = 2.0
CHECKPOINT_WAIT_S = 60.0  # how long a solver asked to pause gets to write its checkpoint before it is killed
CHECKPOINT_EVERY_RANGE = (10.0, 3600.0)  # seconds between checkpoints: the offer's value is clamped to this
RETENTION_S = 7 * 24 * 3600  # job folders (and so checkpoints) are kept this long
MAX_CHECKPOINTS_IN_HELLO = 64
EXIT_PAUSED = 75  # the solver's exit status after it wrote a checkpoint at a pause request


class WorkerRefused(Exception):
    pass


def clamp_limits(offered, caps):
    """The limits a job runs under: the coordinator may tighten the worker's ceilings, never raise them."""
    out = dict(caps)
    for key, cap in caps.items():
        if key == "max_tier":
            want = offered.get(key)
            if want in jobs.TIERS and jobs.TIERS.index(want) < jobs.TIERS.index(cap):
                out[key] = want
        elif key in offered:
            value = offered[key]
            if isinstance(value, (int, float)) and not isinstance(value, bool) and 0 < value < cap:
                out[key] = value
    return out


CHECKPOINT_REFUSED = (
    "checkpoint refused:"  # scheduler.CHECKPOINT_REFUSED: the coordinator starts the job again
)


class DeviceGone(Exception):
    pass


class JobCancelled(Exception):
    pass


def parse_devices(text):
    """Devices from `emfdtd --list-devices`."""
    out = []
    for line in text.splitlines():
        m = DEVICE_LINE.match(line.strip())
        if not m:
            continue
        out.append(
            {
                "name": m["name"],
                "uuid": m["uuid"],
                "discrete": m["kind"] == "discrete",
                "memory_bytes": int(float(m["mem"]) * (1 << 30)),
                "vulkan": (int(m["major"]), int(m["minor"])),
                "driver": m["driver"],
                "usable": m["unusable"] is None and m["uuid"] != "",
            }
        )
    return out


def choose(devices, allow):
    """The devices this worker offers: usable, matching an allow entry (a UUID prefix or part of the name,
    any case), one entry per physical GPU (an Apple GPU is listed once per Vulkan driver: keep the newest)."""
    allow = [a.lower() for a in allow]
    picked = []
    for d in devices:
        if not d["usable"]:
            continue
        if any(d["uuid"].startswith(a) or a in d["name"].lower() for a in allow):
            picked.append(d)
    best = {}
    keep = []
    for d in picked:
        if d["discrete"]:
            keep.append(d)
            continue
        key = d["name"]
        if key not in best or d["vulkan"] > best[key]["vulkan"]:
            best[key] = d
    return keep + list(best.values())


class Worker:
    def __init__(
        self,
        bundle,
        host,
        port,
        solver,
        work_root,
        allow,
        lock_command=(),
        heartbeat=10.0,
        caps=None,
        no_lock=False,
        turn_from_coordinator=False,
        watchdog_s=20.0,
        lock_wait_s=6 * 3600.0,
        status_file=None,
        checkpoint_wait_s=CHECKPOINT_WAIT_S,
        checkpoint=True,
    ):
        modes = [bool(lock_command), bool(no_lock), bool(turn_from_coordinator)]
        if sum(modes) != 1:
            raise ValueError(
                "a worker needs exactly one way to take its GPU turn: a lock command wrapping every GPU use, "
                "or turn_from_coordinator (the coordinator holds the turn and says so in each offer), or an "
                "explicit no_lock for a GPU nobody else shares"
            )
        self.mode = "lock" if lock_command else "coordinator" if turn_from_coordinator else "none"
        # If nothing authenticated arrives from the coordinator for this long, the solver is killed and the
        # connection dropped: a partitioned worker must not keep using a GPU whose turn the coordinator has
        # (rightly) given back. The coordinator pings far more often than this.
        self.watchdog_s = float(watchdog_s)
        # How long a call that goes through the lock command may wait for its turn (a long run on a shared GPU
        # holds the lock for an hour or more). Not a limit on the solver: that is the job's own time limit.
        self.lock_wait_s = float(lock_wait_s)
        self._heard = None
        # A small JSON file the agent commands read (agent.status.json): who we are, whether we are connected,
        # what we are doing. Written atomically; never on the job's critical path (errors are ignored).
        self.status_file = Path(status_file) if status_file else None
        self._state = {
            "pid": os.getpid(), "started": time.time(), "updated": time.time(), "connected": False,
            "coordinator": f"{host}:{port}", "job": None, "device": None, "last_error": None,
            "solver_pids": [],
        }  # fmt: skip
        self.caps = {**policy.DEFAULT_LIMITS, **(caps or {})}
        self.bad_devices = set()  # devices the last pre-job check did not find
        self.bundle, self.host, self.port = bundle, host, port
        self.solver = str(solver)
        self.work_root = Path(work_root)
        self.allow = list(allow)
        self.lock_command = list(lock_command)
        self.heartbeat = heartbeat
        self.running = {}  # job id -> procs.Running (the solve)
        self._children = {}  # job id or None -> other solver children (device listing)
        self.cancelled = set()  # jobs the coordinator told us to stop
        self.tasks = {}
        self._jobs = {}  # job id -> its status entry: two GPUs of this machine may each run a job
        self._lock = None  # asyncio objects are created inside the running loop (Python 3.9 binds them early)
        self._connected = None
        self._tiers = None
        self._speed = {}
        self._rates = {}  # device uuid -> the last full-run rates (the speed is their median)
        self._stop = False
        self.checkpoint_wait_s = float(checkpoint_wait_s)
        self._probed = False
        self.checkpoint = bool(checkpoint)  # False behaves like a worker from before checkpoints existed
        self._ckpt_dirs = {}  # job id -> the checkpoint folder of a running job (where its stop file goes)
        self._ctl_writer = None  # the control connection of the current session

    @property
    def connected(self):
        if self._connected is None:
            self._connected = asyncio.Event()
        return self._connected

    def _status(self, **fields):
        """Merge fields into the status and write the file (when one is configured)."""
        self._state.update(fields)
        self._state["updated"] = time.time()
        if self.status_file is None:
            return
        with contextlib.suppress(OSError):
            tmp = self.status_file.with_suffix(".tmp")
            tmp.write_text(json.dumps(self._state))
            os.replace(str(tmp), str(self.status_file))

    def _job_status(self, job, **fields):
        """Update one job's entry; `job` in the status file is the newest, `jobs` lists all of them."""
        entry = self._jobs.setdefault(job, {"id": job, "label": None, "phase": "starting", "percent": None})
        entry.update(fields)
        self._status(job=entry, jobs=list(self._jobs.values()))

    def _job_done(self, job):
        self._jobs.pop(job, None)
        newest = list(self._jobs.values())[-1] if self._jobs else None
        self._status(job=newest, jobs=list(self._jobs.values()), solver_pids=self._pids())

    def _pids(self):
        return sorted(r.proc.pid for r in self.running.values())

    def _fresh(self):
        loop = asyncio.get_running_loop()
        return self._heard is not None and loop.time() - self._heard <= self.watchdog_s

    def _kill_everything(self):
        for r in list(self.running.values()):
            r.kill_tree()
        for rs in list(self._children.values()):
            for r in list(rs):
                r.kill_tree()

    async def _child(self, job, argv, cwd, timeout):
        """Run a short solver child (device listing) to completion. It is registered so a cancel, the
        watchdog or a timeout kills it, and it is always dead when this returns or raises."""
        running = await procs.start(argv, cwd=cwd)
        self._children.setdefault(job, []).append(running)
        try:
            out, _ = await asyncio.wait_for(running.proc.communicate(), timeout)
            return running.proc.returncode, out.decode("utf-8", "replace")
        finally:
            if running.proc.returncode is None:
                running.kill_tree()
                with contextlib.suppress(Exception):
                    await asyncio.wait_for(running.proc.wait(), 10)
            if running in self._children.get(job, []):
                self._children[job].remove(running)

    async def _reap(self, job):
        """Make sure nothing of this job is alive: the solve and any child."""
        procs_ = [r for r in [self.running.get(job)] if r] + list(self._children.get(job, []))
        for r in procs_:
            r.kill_tree()
        for r in procs_:
            with contextlib.suppress(Exception):
                await asyncio.wait_for(r.proc.wait(), 10)

    def _check_cancelled(self, job):
        if job in self.cancelled:
            raise JobCancelled()

    async def _watchdog(self, writer):
        while True:
            await asyncio.sleep(min(0.25, self.watchdog_s / 4))
            if not self._fresh():
                self._kill_everything()  # first the solver, then the connection
                for t in list(self.tasks.values()):
                    t.cancel()
                with contextlib.suppress(Exception):
                    writer.close()
                return

    def _write_lock(self):
        if self._lock is None:
            self._lock = asyncio.Lock()
        return self._lock

    # --- the solver ---------------------------------------------------------------------------

    def _gpu_prefix(self):
        """What goes in front of a solver call that creates a Vulkan instance: the lock command (the turn)."""
        return self.lock_command if self.mode == "lock" else []

    async def _capture(self, *args, gpu=False):
        """Run the solver for its text output. gpu=True for anything that creates a Vulkan instance (device
        listing, a solve): it goes through the lock command, like every other use of the GPU."""
        self.work_root.mkdir(parents=True, exist_ok=True)
        prefix = self._gpu_prefix() if gpu else []
        waits_for_turn = gpu and bool(prefix)
        timeout = self.lock_wait_s + 60 if waits_for_turn else 60
        return await self._child(None, [*prefix, self.solver, *args], str(self.work_root), timeout)

    async def devices(self):
        """Live enumeration (a Vulkan call): via the lock command, or while the coordinator holds the turn."""
        code, text = await self._capture("--list-devices", gpu=True)
        if code != 0:
            raise RuntimeError("the solver could not list devices: " + text[-200:])
        return choose(parse_devices(text), self.allow)

    def _devices_file(self):
        return self.work_root / "devices.json"

    def save_devices(self, devs):
        self._devices_file().write_text(json.dumps(devs))

    async def devices_for_hello(self):
        """The devices offered at connect time, in every turn mode: the ones recorded earlier with
        `devices --save` (taken once, under a free lock or a held turn). A worker makes no Vulkan call to
        connect, so a GPU that someone else is using for the next hour (a long run holding the shared lock)
        never stops it from joining; the live check that the GPU is still there happens before each job,
        inside the turn."""
        return self.recorded_devices()

    def recorded_devices(self):
        """The saved devices this worker offers (allow list applied), or RuntimeError saying how to save."""
        try:
            return choose(json.loads(self._devices_file().read_text()), self.allow)
        except (OSError, ValueError):
            raise RuntimeError(
                f"no recorded devices in {self._devices_file()}: run `emfdtd-farm devices --solver "
                f"{self.solver} --save --device NAME` first, with --lock-command (it waits for your turn) "
                "or --no-lock (you hold the turn now)"
            ) from None

    async def probe_checkpoint(self):
        """A solver built before checkpoints existed would fail every job on the unknown flag, so a worker
        offers the feature only if the solver's usage text names it (no GPU call; empty text keeps it)."""
        if not self.checkpoint or self._probed:
            return
        self._probed = True
        code, text = await self._capture()  # no arguments: the solver prints its usage (it has no --help)
        if text.strip() and "--checkpoint-dir" not in text:
            self.checkpoint = False
            self._status(last_error="the solver has no --checkpoint-dir: jobs run without checkpoints")

    async def tiers(self):
        if self._tiers is None:
            code, text = await self._capture("--list-settings")
            if code != 0:
                raise RuntimeError("the solver could not list its settings")
            self._tiers = jobs.parse_list_settings(text)
        return self._tiers

    def _speed_file(self):
        return self.work_root / "speed.json"

    def _load_speed(self):
        with contextlib.suppress(OSError, ValueError, TypeError):
            data = json.loads(self._speed_file().read_text())
            if isinstance(data.get("speed"), dict):
                self._speed = {k: float(v) for k, v in data["speed"].items()}
                self._rates = {k: [float(x) for x in v] for k, v in (data.get("rates") or {}).items()}
            else:  # the older format: {device uuid: speed}
                self._speed = {k: float(v) for k, v in data.items()}
                self._rates = {k: [v] for k, v in self._speed.items()}

    def _note_speed(self, uuid, mcells, seconds=None, resumed=False):
        """Record what a run says about the device. The speed is the median of the last few whole runs:
        a resumed run's end line covers only its last segment, and a run of a few minutes or less is mostly
        setup, so neither counts (unless the device has no figure at all yet)."""
        if resumed:
            return
        if seconds is not None and seconds < MIN_RATE_RUN_S and uuid in self._speed:
            return
        rates = self._rates.setdefault(uuid, [])
        rates.append(float(mcells))
        del rates[:-RATE_HISTORY]
        self._speed[uuid] = float(statistics.median(rates))
        with contextlib.suppress(OSError):
            self._speed_file().write_text(json.dumps({"speed": self._speed, "rates": self._rates}))

    def _wire_devices(self, devs):
        return [
            {
                "uuid": d["uuid"],
                "name": d["name"],
                "driver": d["driver"],
                "memory_bytes": d["memory_bytes"],
                "mcells_per_s": self._speed.get(d["uuid"], 0.0),
            }
            for d in devs
        ]

    # --- connection ---------------------------------------------------------------------------

    async def _connect(self):
        ctx = pki.client_context(self.bundle)
        reader, writer = await asyncio.wait_for(asyncio.open_connection(self.host, self.port, ssl=ctx), 15)
        ident = pki.peer_identity(writer.get_extra_info("ssl_object"))
        if ident is None or ident[0] != "coordinator":
            writer.close()
            raise WorkerRefused("the server is not a coordinator of this farm")
        return reader, writer

    async def _send(self, writer, message):
        async with self._write_lock():
            writer.write(protocol.encode(message))
            await writer.drain()

    async def run_forever(self, data_hash="", solver_version=""):
        self.work_root.mkdir(parents=True, exist_ok=True)
        self._load_speed()
        self._jobs.clear()
        self._status(connected=False, last_error=None, job=None, jobs=[], solver_pids=[])
        delay = 1.0
        while not self._stop:
            try:
                await self._session(data_hash, solver_version)
                delay = 1.0
            except WorkerRefused as e:
                delay = 30.0
                self._status(last_error=f"the coordinator refused this worker: {e}")
            except (
                OSError,
                ssl.SSLError,
                protocol.ProtocolError,
                asyncio.TimeoutError,
                ConnectionError,
            ) as e:
                self._status(last_error=f"{type(e).__name__}: {e}"[:300])
            self.connected.clear()
            self._jobs.clear()
            self._status(connected=False, job=None, jobs=[], solver_pids=[])
            await asyncio.sleep(delay)
            delay = min(30.0, delay * 2)

    def stop(self):
        self._stop = True
        for r in list(self.running.values()):
            r.kill_tree()

    async def graceful_stop(self, wait=None):
        """Stop the worker politely: every running solver that can checkpoint is asked to write one and leave
        (the job is reported paused and continues when this worker is back); whatever is left after `wait`
        seconds is killed. The connection stays up meanwhile so the coordinator hears about it."""
        self._stop = True
        wait = self.checkpoint_wait_s + 15.0 if wait is None else wait
        await (
            self._say_unavailable()
        )  # first: the paused job must not be offered back to this stopping worker
        for job in list(self.running):
            folder = self._ckpt_dirs.get(job)
            if folder is not None:
                with contextlib.suppress(OSError):
                    (folder / "stop").write_text("stop\n")
        loop = asyncio.get_running_loop()
        end = loop.time() + wait
        while self.running and loop.time() < end:
            await asyncio.sleep(0.1)
        self.stop()

    async def _say_unavailable(self):
        """Tell the coordinator every GPU of this worker is out (an immediate heartbeat with ok false)."""
        try:
            uuids = [d["uuid"] for d in self.recorded_devices()]
        except RuntimeError:
            return
        self.bad_devices.update(uuids)
        if self._ctl_writer is not None:
            payload = [{"uuid": u, "ok": False, "mcells_per_s": self._speed.get(u, 0.0)} for u in uuids]
            with contextlib.suppress(Exception):
                await self._send(self._ctl_writer, {"type": "heartbeat", "devices": payload})

    # --- checkpoints held on this machine ---------------------------------------------------------

    def _job_folder(self, job):
        return self.work_root / "jobs" / store.job_dir(job).name

    @staticmethod
    def _write_sidecar(work, job, step, device):
        """What `hello` reports: the newest checkpoint of this job (the solver's own files are binary)."""
        with contextlib.suppress(OSError):
            tmp = work / "checkpoint.tmp"
            tmp.write_text(
                json.dumps({"job": job, "step": int(step), "device": device, "written": time.time()})
            )
            os.replace(str(tmp), str(work / "checkpoint.json"))

    def held_checkpoints(self):
        """[{"job", "step", "device"}] for the job folders that hold a checkpoint (validated, capped)."""
        out = []
        for side in sorted((self.work_root / "jobs").glob("*/checkpoint.json")):
            try:
                data = json.loads(side.read_text())
                job, step, dev = data["job"], data["step"], data.get("device")
                if (
                    not (isinstance(job, str) and re.match(r"^[0-9a-f]{32}$", job))
                    or isinstance(step, bool)
                    or not isinstance(step, int)
                    or step < 0
                    or store.job_dir(job).name != side.parent.name
                    or job in self.running
                ):
                    continue
            except (OSError, ValueError, KeyError, TypeError):
                continue
            out.append({"job": job, "step": step, "device": dev if isinstance(dev, str) else None})
        return out[:MAX_CHECKPOINTS_IN_HELLO]

    def sweep_jobs(self, now=None):
        """Remove job folders nobody can use: leftovers without a checkpoint, and checkpoints older than the
        retention time (7 days). A folder of a job that is running now is never touched."""
        now = time.time() if now is None else now
        for folder in list((self.work_root / "jobs").glob("*")):
            if not folder.is_dir() or any(self._job_folder(j) == folder for j in self.running):
                continue
            keep = False
            with contextlib.suppress(OSError, ValueError, KeyError, TypeError):
                written = json.loads((folder / "checkpoint.json").read_text())["written"]
                keep = now - float(written) < RETENTION_S
            if not keep:
                shutil.rmtree(str(folder), ignore_errors=True)

    def _discard(self, job):
        """The coordinator does not need this job's checkpoint any more (cancelled, restarted, abandoned)."""
        if job in self.tasks or job in self.running:
            return  # a run is using the folder: its own cleanup removes it
        if isinstance(job, str) and re.match(r"^[0-9a-f]{32}$", job):
            shutil.rmtree(str(self._job_folder(job)), ignore_errors=True)

    async def _session(self, data_hash, solver_version):
        reader, writer = await self._connect()
        self._ctl_writer = writer
        beat = dog = None
        try:
            await self.probe_checkpoint()
            devs = await self.devices_for_hello()
            self.sweep_jobs()  # before the hello lists what is held: nothing past its retention is reported
            hello = {
                "type": "hello",
                "protocol": protocol.PROTOCOL_VERSION,
                "name": "worker",
                "devices": self._wire_devices(devs),
                "solver_version": solver_version,
                "data_hash": data_hash,
                "watchdog_s": self.watchdog_s,
                # the longest job this worker will run: lets the coordinator keep a longer job away from it
                "limits": {"max_minutes": self.caps["max_minutes"]},
                # this worker can pause a job at a checkpoint and resume it (an older worker says nothing)
                "features": ["checkpoint"] if self.checkpoint else [],
                "checkpoints": self.held_checkpoints() if self.checkpoint else [],
            }
            await self._send(writer, hello)
            self._heard = asyncio.get_running_loop().time()
            self.connected.set()
            self._status(connected=True, last_error=None, device=devs[0]["name"] if devs else None)
            beat = asyncio.ensure_future(self._beat(writer))
            dog = asyncio.ensure_future(self._watchdog(writer))
            while True:
                msg = await protocol.read_message(reader)
                if msg is None:
                    return
                self._heard = asyncio.get_running_loop().time()
                kind = msg["type"]
                if kind == "offer":
                    self.tasks[msg["job"]] = asyncio.ensure_future(self._run_offer(msg, writer))
                elif kind == "cancel":
                    self._cancel(msg["job"])
                elif kind == "discard":
                    self._discard(msg["job"])
                elif kind == "refused":
                    raise WorkerRefused(msg["reason"])
        finally:
            if beat:
                beat.cancel()
            if dog:
                dog.cancel()
            for r in list(self.running.values()):  # the coordinator requeues; do not leave orphans running
                r.kill_tree()
            for t in list(self.tasks.values()):
                t.cancel()
            with contextlib.suppress(Exception):
                writer.close()

    async def _beat(self, writer):
        """Liveness only. No Vulkan call (that would be GPU use outside a turn): device health is what
        the pre-job check last found, speeds are what finished jobs measured."""
        while True:
            await asyncio.sleep(self.heartbeat)
            known = set(self._speed) | self.bad_devices
            payload = [
                {"uuid": u, "ok": u not in self.bad_devices, "mcells_per_s": self._speed.get(u, 0.0)}
                for u in known
            ]
            await self._send(writer, {"type": "heartbeat", "devices": payload})
            self._status()  # the file's "updated" time is the agent's proof of life

    # --- one job ------------------------------------------------------------------------------

    async def _transfer(self, job, what):
        ctx = pki.client_context(self.bundle)
        reader, writer = await asyncio.wait_for(asyncio.open_connection(self.host, self.port, ssl=ctx), 15)
        writer.write(protocol.encode({"type": "transfer_open", "job": job, "what": what}))
        await writer.drain()
        return reader, writer

    async def _run_offer(self, offer, ctl):  # noqa: C901  (complexity ratchet: split when next touched)
        job, lease, device = offer["job"], offer["lease"], offer["device"]
        work = self.work_root / "jobs" / store.job_dir(job).name
        fail = {"type": "fail", "job": job, "lease": lease, "retry": False}
        keep = False  # a paused job keeps its folder: the checkpoint is in it
        ckpt = self._checkpoint_plan(offer, work)
        self._job_status(job)
        try:
            if self.mode == "coordinator" and offer.get("turn") is not True:
                raise jobs.JobError("this worker only runs jobs while the coordinator holds the GPU turn")
            await self._send(ctl, {"type": "accept", "job": job, "lease": lease})
            await self._phase(ctl, job, lease, "fetching the input")
            if ckpt and ckpt["resume"]:
                if not (work / "ckpt").is_dir() or not (work / "run").is_dir():
                    raise jobs.JobError(
                        f"{CHECKPOINT_REFUSED} this worker has no checkpoint of the job (it was removed)"
                    )
                shutil.rmtree(
                    str(work / "in"), ignore_errors=True
                )  # the input is fetched again; the rest stays
                (work / "in").mkdir(parents=True)
            else:
                shutil.rmtree(str(work), ignore_errors=True)
                (work / "in").mkdir(parents=True)
                (work / "run").mkdir()
                if ckpt:
                    (work / "ckpt").mkdir()
            limits = clamp_limits(offer.get("limits") or {}, self.caps)
            await self._fetch_input(job, work / "in", offer, limits)
            spec = jobs.validate(offer["spec"], await self.tiers(), limits)
            await self._phase(ctl, job, lease, "checking that the GPU is present")
            if device not in {d["uuid"] for d in await self.devices()}:
                self.bad_devices.add(device)
                raise DeviceGone("the GPU is not present any more")
            self.bad_devices.discard(device)
            argv = jobs.build_argv(
                self.solver,
                work / "in" / "input.json",
                spec,
                work / "run",
                device,
                checkpoint=(work / "ckpt", ckpt["every"], ckpt["resume"]) if ckpt else None,
            )
            if not self._fresh():
                raise ConnectionError("the coordinator has gone quiet: not starting the solver")
            self._check_cancelled(job)
            if ckpt:
                self._ckpt_dirs[job] = work / "ckpt"
            keep = await self._solve(job, lease, argv, work, device, limits, ctl, ckpt) == "paused"
        except jobs.JobError as e:
            await self._send(ctl, {**fail, "reason": str(e)[:300]})
        except DeviceGone as e:
            keep = (work / "checkpoint.json").exists()  # a retry may still resume from an earlier checkpoint
            await self._send(ctl, {**fail, "reason": str(e), "retry": True})
        except (protocol.ProtocolError, OSError, asyncio.TimeoutError, ssl.SSLError, RuntimeError) as e:
            keep = (work / "checkpoint.json").exists()
            with contextlib.suppress(Exception):
                reason = f"worker error: {type(e).__name__}: {e}"[:300]
                await self._send(ctl, {**fail, "reason": reason, "retry": True})
        except JobCancelled:
            pass
        except asyncio.CancelledError:
            if job not in self.cancelled:
                # the session ended or the watchdog fired: nobody to report to. The solver is dead, but a
                # checkpoint it wrote stays: the job waits for this worker (docs/farm.md)
                keep = (work / "checkpoint.json").exists()
                raise
        finally:
            await self._reap(job)  # no solver process of this job may outlive this point
            if job in self.cancelled:
                self.cancelled.discard(job)
                with contextlib.suppress(Exception):
                    await self._send(ctl, {"type": "stopped", "job": job, "lease": lease})
            self.running.pop(job, None)
            self._children.pop(job, None)
            self._ckpt_dirs.pop(job, None)
            self.tasks.pop(job, None)
            self._job_done(job)
            if not keep:
                shutil.rmtree(str(work), ignore_errors=True)

    def _checkpoint_plan(self, offer, work):
        """{"every", "resume"} when the coordinator offered checkpointing (only to a worker that said
        it can), else None: the job then runs as it always did."""
        plan = offer.get("checkpoint")
        if not isinstance(plan, dict):
            return None
        if (offer.get("spec") or {}).get("keep_fields") is True:
            return None  # field accumulators are not in a checkpoint: the solver refuses the two together
        every = plan.get("every")
        if isinstance(every, bool) or not isinstance(every, (int, float)):
            every = 300.0
        low, high = CHECKPOINT_EVERY_RANGE
        return {"every": min(high, max(low, float(every))), "resume": plan.get("resume") is True}

    async def _phase(self, ctl, job, lease, text):
        """Tell the coordinator what this job is doing, so 'running' is never silent."""
        if job in self._jobs:
            self._job_status(job, phase=text)
        with contextlib.suppress(Exception):
            await self._send(ctl, {"type": "progress", "job": job, "lease": lease, "line": text})

    def _cancel(self, job):
        """The coordinator wants this job stopped, wherever it is: fetching input, checking the device, in the
        the solve. Mark it, kill what is running, and cancel the task so nothing new starts."""
        task = self.tasks.get(job)
        if task is None:
            return
        self.cancelled.add(job)
        r = self.running.get(job)
        if r:
            r.kill_tree()
        for c in list(self._children.get(job, [])):
            c.kill_tree()
        task.cancel()

    async def _fetch_input(self, job, dest, offer, limits):
        reader, writer = await self._transfer(job, "input_out")
        try:
            _, size, sha = await protocol.recv_blob(
                reader, str(dest), limits["max_input_bytes"], "input.json"
            )
            if sha != offer["input_sha256"] or size != offer["input_bytes"]:
                raise protocol.ProtocolError("the input is not the one that was announced")
            writer.write(protocol.encode({"type": "blob_ack", "ok": True, "reason": ""}))
            await writer.drain()
        finally:
            writer.close()

    async def _solve(self, job, lease, argv, work, device, limits, ctl, ckpt=None):
        """Run the solver and report. Returns "paused" when it left a checkpoint (the folder is kept)."""
        self._check_cancelled(job)
        if not self._fresh():
            raise ConnectionError("the coordinator has gone quiet: not starting the solver")
        await self._phase(ctl, job, lease, "starting the solver (preparing the model, then solving)")
        cmd = self._gpu_prefix() + argv
        running = await procs.start(cmd, cwd=str(work / "run"))
        self.running[job] = running
        self._status(solver_pids=sorted({*self._pids(), running.proc.pid}))
        loop = asyncio.get_running_loop()
        state = {"last": 0.0, "speed": None, "seconds": None, "ckpt_step": None}
        resumed = bool(ckpt and ckpt["resume"])
        stop_file = work / "ckpt" / "stop"

        def on_line(line):
            c = CKPT_LINE.match(line)
            if c:
                state["ckpt_step"] = int(c.group(1))
                self._write_sidecar(work, job, state["ckpt_step"], device)
                asyncio.ensure_future(
                    self._send(
                        ctl, {"type": "checkpointed", "job": job, "lease": lease, "step": state["ckpt_step"]}
                    )
                )
            m = SPEED.search(line)
            if m:
                state["seconds"], state["speed"] = float(m.group(1)), float(m.group(2))
            p = PROGRESS_PERCENT.match(line)
            if p and job in self._jobs:
                self._job_status(job, phase=line[:120], percent=min(100, int(p.group(1))))
            now = loop.time()
            if now - state["last"] >= PROGRESS_EVERY:
                state["last"] = now
                asyncio.ensure_future(
                    self._send(ctl, {"type": "progress", "job": job, "lease": lease, "line": line[:200]})
                )

        log = work / "run.log"
        code, timed_out, last = await procs.pump(
            running,
            str(log),
            on_line,
            MAX_LOG_BYTES,
            limits["max_minutes"] * 60,
            # at the ceiling: ask for a checkpoint and a clean exit first, kill only if it does not come
            soft_stop=(lambda: stop_file.write_text("stop\n")) if ckpt else None,
            soft_wait=self.checkpoint_wait_s,
        )
        if ckpt and code == EXIT_PAUSED:
            paused = next((PAUSED_LINE.match(x) for x in reversed(last) if PAUSED_LINE.match(x)), None)
            step = int(paused.group(1)) if paused else state["ckpt_step"] or 0
            self._write_sidecar(work, job, step, device)
            await self._send(
                ctl,
                {
                    "type": "paused", "job": job, "lease": lease, "step": step,
                    "why": "ceiling" if timed_out else "stopped",
                },
            )  # fmt: skip
            return "paused"
        if running.killed and not timed_out:
            # cancelled by the coordinator: say the solver is dead, so the GPU turn can be given back
            with contextlib.suppress(Exception):
                await self._send(ctl, {"type": "stopped", "job": job, "lease": lease})
            return
        fail = {"type": "fail", "job": job, "lease": lease, "retry": False}
        if timed_out:
            await self._send(
                ctl, {**fail, "reason": f"over the time limit of {limits['max_minutes']} minutes"}
            )
            return
        if code != 0:
            if ckpt and ckpt["resume"] and any("checkpoint" in x for x in last[-3:]):
                await self._send(
                    ctl, {**fail, "reason": f"{CHECKPOINT_REFUSED} " + " | ".join(last[-2:])[:200]}
                )
                return
            await self._send(
                ctl, {**fail, "reason": f"the solver exited with {code}: " + " | ".join(last[-3:])[:200]}
            )
            return
        files = self._collect(work, "--keep-fields" in argv)
        if not files:
            await self._send(ctl, {**fail, "reason": "the solver finished but wrote no results"})
            return
        files = self._fit_fields(files, limits["max_result_bytes"])
        if sum(Path(p).stat().st_size for p in files.values()) > limits["max_result_bytes"]:
            await self._send(ctl, {**fail, "reason": "the results are over this worker's size limit"})
            return
        await self._upload(job, lease, files, ctl)
        if state["speed"]:
            self._note_speed(device, state["speed"], state["seconds"], resumed)

    @staticmethod
    def _collect(work, keep_fields=False):
        runs = sorted((work / "run" / "em-fdtd" / "results").glob("run-*"))
        files = {}
        if runs:
            for name in ("s11.s1p", "summary.txt", "settings.json"):
                if (runs[-1] / name).exists():
                    files[name] = runs[-1] / name
            if keep_fields:  # what makes the folder a saved run: the manifest, the board and the field files
                for name in store.FIELD_RESULT_NAMES:
                    if (runs[-1] / name).exists():
                        files[name] = runs[-1] / name
                for f in sorted((runs[-1] / "fields").glob("*")) if (runs[-1] / "fields").is_dir() else []:
                    if store.is_result_name("fields." + f.name):
                        files["fields." + f.name] = f
        if (work / "run.log").exists():
            files["run.log"] = work / "run.log"
        return files if "s11.s1p" in files else {}

    @staticmethod
    def _fit_fields(files, limit):
        """The field data was asked for, so it is never dropped silently: when the whole result is over the
        limit the S11 is still delivered and summary.txt says what was not kept and why."""
        extra = [n for n in files if n.startswith("fields.") or n in store.FIELD_RESULT_NAMES]
        total = sum(Path(p).stat().st_size for p in files.values())
        if not extra or total <= limit:
            return files
        kept = {n: p for n, p in files.items() if n not in extra}
        if "summary.txt" in kept:
            text = Path(kept["summary.txt"]).read_text()
            text = "".join(x for x in text.splitlines(True) if not x.startswith("fields: kept"))
            with open(kept["summary.txt"], "w") as f:
                f.write(text)
                f.write(
                    f"note: fields requested but not kept: {jobs.megabytes(total)} over the result limit of "
                    f"{jobs.megabytes(limit)}\n"
                )
        return kept

    async def _upload(self, job, lease, files, ctl):
        reader, writer = await self._transfer(job, "result_in")
        try:
            for name, path in files.items():
                await protocol.send_blob(writer, name, str(path))
            writer.write(protocol.encode({"type": "blob_end"}))
            await writer.drain()
            ack = await asyncio.wait_for(protocol.read_message(reader), 60)
            if not ack or ack["type"] != "blob_ack" or not ack["ok"]:
                raise protocol.ProtocolError("the coordinator did not accept the result")
        finally:
            writer.close()
        info = {
            n: {"bytes": Path(p).stat().st_size, "sha256": protocol.file_sha256(str(p))}
            for n, p in files.items()
        }
        await self._send(ctl, {"type": "result", "job": job, "lease": lease, "files": info})
