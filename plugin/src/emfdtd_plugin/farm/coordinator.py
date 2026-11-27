"""The coordinator: one TLS listener for workers and clients, the scheduler, and the audit log.

Every connection is mutually authenticated (pki.py); the certificate gives the identity, and policy.py is
asked about each request, not once per connection. Workers dial in and wait for pushed events (an offer, a
cancel); clients submit and ask. Bulk files travel on their own short connections, bound to one job.
"""

import asyncio
import contextlib
import json
import os
import re
import ssl
import time

from . import audit, config, gpulock, jobs, pki, policy, procs, protocol, store
from .scheduler import Scheduler, SchedulerError

READ_TIMEOUT = 30.0
LOCK_NAME = re.compile(r"^(cpu|gpu):[A-Za-z0-9._-]{1,64}(:[A-Za-z0-9._-]{1,64})?$")
SHA256 = re.compile(r"^[0-9a-f]{64}$")
GROUP = re.compile(r"^[A-Za-z0-9_-]{1,40}$")


class _Session:
    def __init__(self, writer, watchdog_s=20.0):
        self.writer = writer
        self.outbox = asyncio.Queue()
        self.task = None
        self.pinger = None
        self.watchdog_s = watchdog_s  # how long the worker waits in silence before killing its solver
        self.advertised_minutes = None  # the longest job the worker says it will run (None: it did not say)
        self.features = set()  # what the worker said it can do ("checkpoint"); an old worker says nothing

    def close(self):
        if self.task:
            self.task.cancel()
        if self.pinger:
            self.pinger.cancel()
        with contextlib.suppress(Exception):
            self.writer.close()


class Coordinator:
    def __init__(
        self,
        bundle,
        host,
        port=0,
        scheduler=None,
        clock=time.time,
        data_hash=None,
        check_every=5.0,
        retention_s=7 * 24 * 3600,
        gates=None,
        gates_loader=None,
        ping_every=5.0,
        turn_margin_s=10.0,
        checkpoint_every_s=300.0,
        hold_s=None,
    ):
        self.bundle = bundle
        self.host, self.requested_port = host, port
        self.sched = scheduler or Scheduler()
        if hold_s is not None:
            self.sched.hold_s = float(hold_s)  # how long a job waits for the worker that holds its checkpoint
        self.checkpoint_every_s = float(checkpoint_every_s)
        self.clock = clock
        self.data_hash = data_hash
        self.check_every = check_every
        self.retention_s = retention_s
        # worker name -> {"hold": [...], "release": [...]}: the coordinator takes the GPU turn for a worker's
        # job (the turn lives on this machine, e.g. win_gpu_lock.sh) and offers only while it is held
        self.gates = gates or {}
        # Called before every dispatch, so a change to gates.json applies to the next job without a restart.
        self.gates_loader = gates_loader
        self._saved = None  # the text last written to jobs.json
        self.turns = {}  # job id -> the held turn (see _gated_offer)
        self.locks = gpulock.GpuLocks()  # the farm's own turn manager: a gate with a "lock" name uses it
        self.ping_every = ping_every
        self.turn_margin_s = (
            turn_margin_s  # added to the worker's watchdog before a turn is given back unconfirmed
        )
        self.sessions = {}
        self.port = None
        self._server = None
        self._expirer = None

    def _jobs_file(self):
        return config.home() / "jobs.json"

    def _load_jobs(self):
        try:
            rows = json.loads(self._jobs_file().read_text())
        except (OSError, ValueError):
            return
        if isinstance(rows, list):
            self.sched.restore(rows, self.clock())

    def _persist(self):
        """Save the queue whenever it changes (atomic replace), so a restart does not lose it."""
        text = json.dumps(self.sched.snapshot(), sort_keys=True)
        if text == self._saved:
            return
        config.ensure_home()
        path = self._jobs_file()
        tmp = path.with_suffix(".tmp")
        tmp.write_text(text)
        os.replace(str(tmp), str(path))
        self._saved = text

    async def start(self):
        self._load_jobs()
        self._persist()
        ctx = pki.server_context(self.bundle)
        self._server = await asyncio.start_server(self._handle, self.host, self.requested_port, ssl=ctx)
        self.port = self._server.sockets[0].getsockname()[1]
        self._expirer = asyncio.ensure_future(self._expire_loop())

    async def stop(self):
        if self._expirer:
            self._expirer.cancel()
        for job_id in list(self.turns):
            await self._release_turn(job_id)
        for s in list(self.sessions.values()):
            s.close()
        if self._server:
            self._server.close()
            with contextlib.suppress(asyncio.TimeoutError):  # a half-open connection must not hang shutdown
                await asyncio.wait_for(self._server.wait_closed(), 5)

    async def _expire_loop(self):
        while True:
            await asyncio.sleep(self.check_every)
            lost = self.sched.expire(self.clock())
            for name, w in self.sched.workers.items():
                if not w["online"] and name in self.sessions:
                    self.sessions.pop(name).close()
            if lost:
                audit.record(
                    ("coordinator", "main"), "lease_expired", True, reason=f"{len(lost)} job(s) requeued"
                )
                self._dispatch()
            self._release_ended()
            for job_id in self.sched.forget_finished(self.retention_s, self.clock()):
                store.forget(job_id)  # inputs and results are not kept for ever
            self._persist()

    # --- connections --------------------------------------------------------------------------

    async def _handle(self, reader, writer):
        try:
            ssl_obj = writer.get_extra_info("ssl_object")
            ident = pki.peer_identity(ssl_obj)
            if ident is None:
                audit.record(None, "connect", False, reason="no valid identity")
                return
            der = ssl_obj.getpeercert(binary_form=True)
            first = await asyncio.wait_for(protocol.read_message(reader), READ_TIMEOUT)
            if first is None:
                return
            if first["type"] == "transfer_open":
                await self._transfer(ident, der, first, reader, writer)
            elif first["type"] == "hello":
                await self._worker_session(ident, der, first, reader, writer)
            elif first["type"] == "lock_hold":
                await self._lock_session(ident, der, first, reader, writer)
            else:
                await self._client_session(ident, der, first, reader, writer)
        except (protocol.ProtocolError, asyncio.TimeoutError, ConnectionError, ssl.SSLError):
            pass
        finally:
            with contextlib.suppress(Exception):
                writer.close()

    @staticmethod
    async def _send(writer, message):
        writer.write(protocol.encode(message))
        await writer.drain()

    def _allowed(self, ident, der, action, job=None, grant=None):
        """Per-request authorisation: not revoked, enrolled, and granted this action (`grant` names the policy
        entry when it differs: resume and restart need the same grant as cancel). Audited either way."""
        if pki.is_revoked(der):
            audit.record(ident, action, False, job, "certificate revoked")
            return False, "certificate revoked"
        ok, why = policy.authorize(ident, grant or action)
        audit.record(ident, action, ok, job, "" if ok else why)
        return ok, why

    # --- lock queue: any enrolled device may wait for a turn (cpu:MACHINE, gpu:MACHINE[:GPU]) -----

    async def _lock_session(self, ident, der, msg, reader, writer):
        """The connection is the turn. Queue for it, tell the peer when it comes, and give it back when the
        peer says so, hangs up or its time is up, so a crashed client can never leave a lock held."""
        ok, why = self._allowed(ident, der, "lock")
        name, label = msg["lock"], str(msg["label"])[:80] or f"{ident[0]}:{ident[1]}"
        if ok and not LOCK_NAME.match(name):
            ok, why = False, "lock names look like cpu:MACHINE or gpu:MACHINE[:GPU]"
        if not ok:
            await self._send(writer, {"type": "refused", "reason": why})
            return
        minutes = min(max(float(msg["max_minutes"]), 0.1), 24 * 60)
        gone = asyncio.ensure_future(protocol.read_message(reader))  # a message or the hang-up
        acq = asyncio.ensure_future(self.locks.acquire(name, label, minutes * 60))
        hold = None
        try:
            await self._send(writer, {"type": "lock_queued", "ahead": self.locks.ahead(name)})
            await asyncio.wait({acq, gone}, return_when=asyncio.FIRST_COMPLETED)
            if not acq.done():
                return  # hung up while waiting (the finally clause leaves the queue)
            hold = acq.result()
            audit.record(ident, "lock_held", True, None, f"{name} {label}")
            await self._send(writer, {"type": "lock_granted"})
            await asyncio.wait({hold.lost, gone}, return_when=asyncio.FIRST_COMPLETED)
            if hold.lost.done():
                await self._send(writer, {"type": "lock_lost"})
        finally:
            gone.cancel()
            if hold is None and acq.done() and not acq.cancelled() and acq.exception() is None:
                hold = acq.result()  # granted just as the peer left
            acq.cancel()
            if hold is not None:
                hold.release()
                audit.record(ident, "lock_released", True, None, f"{name} {label}")

    # --- clients ------------------------------------------------------------------------------

    async def _client_session(self, ident, der, msg, reader, writer):
        while msg is not None:
            reply = self._client_msg(ident, der, msg)
            await self._send(writer, reply)
            msg = await asyncio.wait_for(protocol.read_message(reader), 300)

    def _client_msg(self, ident, der, msg):  # noqa: C901  (complexity ratchet: split when next touched)
        owner = f"{ident[0]}:{ident[1]}"
        kind = msg["type"]
        if kind not in ("submit", "status", "cancel", "results", "resume", "restart"):
            return {"type": "refused", "reason": f"unexpected message {kind}"}
        ok, why = self._allowed(
            ident, der, kind, msg.get("job"), "cancel" if kind in ("resume", "restart") else None
        )
        if not ok:
            return {"type": "refused", "reason": why}
        now = self.clock()
        limits = policy.limits_for(ident)
        try:
            if kind == "submit":
                if not SHA256.match(msg["input_sha256"]):
                    return {"type": "refused", "reason": "input_sha256 must be 64 hex digits"}
                if not 0 < msg["input_bytes"] <= limits["max_input_bytes"]:
                    return {"type": "refused", "reason": "input size is outside the allowed range"}
                spec = jobs.validate(msg["spec"], None, limits)
                pin = spec.get("pin")
                if pin and f"worker:{pin.partition(':')[0]}" not in policy.load()["devices"]:
                    return {
                        "type": "refused",
                        "reason": f"no enrolled worker named '{pin.partition(':')[0]}'",
                    }
                group = msg.get("group")
                if group is not None and not (isinstance(group, str) and GROUP.match(group)):
                    return {"type": "refused", "reason": "bad group name"}
                job = self.sched.submit(
                    owner, spec, msg["input_sha256"], msg["input_bytes"], msg["label"][:80], now,
                    max_queued=limits.get("max_queued", 100), group=f"{owner}/{group}" if group else None,
                )  # fmt: skip
                return {"type": "submitted", "job": job.id}
            if kind == "status":
                return {
                    "type": "status_reply",
                    "jobs": self.sched.listing(owner),
                    "devices": self.sched.device_listing(),
                    "locks": self.locks.status(self._known_locks()),
                }
            if kind in ("resume", "restart"):
                if kind == "resume":
                    self.sched.resume(msg["job"], owner, msg.get("max_minutes"), now)
                else:
                    self.sched.restart(msg["job"], owner, now)
                self._dispatch()
                return {"type": "ok"}
            if kind == "cancel":
                running = self.sched.cancel(msg["job"], owner, now)
                if running:
                    sess = self.sessions.get(running[0])
                    if sess:
                        sess.outbox.put_nowait({"type": "cancel", "job": running[1]})
                self._dispatch()
                return {"type": "ok"}
            job = self.sched.jobs.get(msg["job"])  # results
            if job is None or job.owner != owner or job.state != "done":
                return {"type": "refused", "reason": "no finished job of yours with that id"}
            return {"type": "result_info", "job": job.id, "files": job.files}
        except (jobs.JobError, SchedulerError) as e:
            return {"type": "refused", "reason": str(e)}

    # --- workers ------------------------------------------------------------------------------

    async def _worker_session(self, ident, der, hello, reader, writer):
        ok, why = self._allowed(ident, der, "run")
        if not ok or ident[0] != "worker":
            await self._send(writer, {"type": "refused", "reason": why if not ok else "not a worker"})
            return
        if hello["protocol"] != protocol.PROTOCOL_VERSION:
            await self._send(writer, {"type": "refused", "reason": "protocol version mismatch"})
            return
        if self.data_hash and hello["data_hash"] != self.data_hash:
            await self._send(writer, {"type": "refused", "reason": "the worker's solver data differs"})
            return
        name = ident[1]
        devices = self._clean_devices(hello["devices"])
        old = self.sessions.pop(name, None)
        if old:
            old.close()
        wd = hello.get("watchdog_s")
        wd = (
            float(wd) if isinstance(wd, (int, float)) and not isinstance(wd, bool) and 0 < wd <= 600 else 20.0
        )
        sess = _Session(writer, wd)
        features = [f for f in (hello.get("features") or []) if isinstance(f, str)][:8]
        sess.features = set(features)
        limits = hello.get("limits")
        minutes = limits.get("max_minutes") if isinstance(limits, dict) else None
        if isinstance(minutes, (int, float)) and not isinstance(minutes, bool) and minutes > 0:
            sess.advertised_minutes = float(minutes)
        self.sessions[name] = sess
        discard = self.sched.worker_hello(
            name, devices, self.clock(), max_minutes=sess.advertised_minutes,
            features=features, checkpoints=self._clean_checkpoints(hello.get("checkpoints")),
        )  # fmt: skip
        for job_id in discard:  # checkpoints of jobs that were cancelled, finished, or restart from step 0
            sess.outbox.put_nowait({"type": "discard", "job": job_id})
        sess.task = asyncio.ensure_future(self._pump_outbox(sess))
        sess.pinger = asyncio.ensure_future(self._ping_loop(sess))
        self._dispatch()
        try:
            while True:
                msg = await asyncio.wait_for(protocol.read_message(reader), self.sched.heartbeat_timeout * 2)
                if msg is None:
                    break
                if pki.is_revoked(der) or not policy.authorize(ident, "run")[0]:
                    audit.record(ident, "run", False, reason="no longer allowed; connection closed")
                    break
                self._worker_msg(name, ident, msg)
        finally:
            if self.sessions.get(name) is sess:
                del self.sessions[name]
                self.sched.worker_lost(name, self.clock())
                audit.record(ident, "worker_gone", True)
            sess.close()
            self._dispatch()

    @staticmethod
    def _clean_checkpoints(raw):
        """What a worker says it holds: [{"job", "step", "device"}], validated and capped."""
        out = []
        for c in (raw or [])[:64] if isinstance(raw, list) else []:
            if (
                not isinstance(c, dict)
                or not isinstance(c.get("job"), str)
                or not store.JOB_ID.match(c["job"])
            ):
                continue
            step = c.get("step")
            if isinstance(step, bool) or not isinstance(step, int) or step < 0:
                continue
            dev = c.get("device")
            out.append({"job": c["job"], "step": step, "device": dev if isinstance(dev, str) else None})
        return out

    @staticmethod
    def _clean_devices(raw):
        out = []
        for d in raw[:16]:
            if not (isinstance(d, dict) and isinstance(d.get("uuid"), str)):
                continue
            if not re.match(r"^[0-9a-f]{4,32}$", d["uuid"]):
                continue
            mem, speed = d.get("memory_bytes"), d.get("mcells_per_s")
            out.append(
                {
                    "uuid": d["uuid"],
                    "name": str(d.get("name", ""))[:80],
                    "driver": str(d.get("driver", ""))[:80],
                    "memory_bytes": mem if isinstance(mem, int) and not isinstance(mem, bool) else 0,
                    "mcells_per_s": float(speed) if isinstance(speed, (int, float)) else 0.0,
                    "ok": True,
                }
            )
        return out

    async def _ping_loop(self, sess):
        """The worker kills its solver if it hears nothing for its watchdog time, so it is told often that
        the coordinator is still here."""
        every = max(0.05, min(self.ping_every, sess.watchdog_s / 4))
        while True:
            await asyncio.sleep(every)
            sess.outbox.put_nowait({"type": "ping"})

    async def _pump_outbox(self, sess):
        try:
            while True:
                msg = await sess.outbox.get()
                sess.writer.write(protocol.encode(msg))
                await sess.writer.drain()
        except (ConnectionError, asyncio.CancelledError, ssl.SSLError):
            pass

    def _worker_msg(self, name, ident, msg):
        now = self.clock()
        kind = msg["type"]
        if kind == "heartbeat":
            self.sched.heartbeat(name, [d for d in msg["devices"] if isinstance(d, dict)], now)
            self._dispatch()
        elif kind == "progress":
            self.sched.progress(msg["job"], msg["lease"], str(msg["line"]))
        elif kind == "stopped":
            self._confirm_dead(msg["job"], msg["lease"])
            self.sched.stopped(msg["job"], name)  # a cancelled job's device is free once its solver is gone
            self._dispatch()
        elif kind == "checkpointed":
            self.sched.checkpointed(msg["job"], msg["lease"], msg["step"])
            self._persist()
        elif kind == "paused":
            self._confirm_dead(
                msg["job"], msg["lease"]
            )  # the solver has exited to be paused: the turn can go
            why = "ceiling" if msg.get("why") == "ceiling" else "stopped"
            if self.sched.pause(msg["job"], msg["lease"], msg["step"], why, now):
                audit.record(ident, "job_paused", True, msg["job"], f"step {msg['step']} ({why})")
            self._dispatch()
        elif kind == "result":
            self._confirm_dead(msg["job"], msg["lease"])  # the solver has exited to have a result
            self._finish(ident, msg, now)
        elif kind == "fail":
            self._confirm_dead(msg["job"], msg["lease"])  # a failure is reported after the solver is gone
            if self.sched.fail(
                msg["job"], msg["lease"], str(msg["reason"])[:300], now, bool(msg.get("retry"))
            ):
                audit.record(ident, "job_failed", True, msg["job"], str(msg["reason"])[:200])
            self._dispatch()

    def _finish(self, ident, msg, now):
        limits = policy.limits_for(ident)
        job = self.sched.jobs.get(msg["job"])
        files, total = {}, 0
        try:
            if job is None:
                raise ValueError("no such job")
            for fname, info in msg["files"].items():
                protocol.safe_name(fname)
                if fname not in store.RESULT_NAMES:
                    raise ValueError(f"unexpected result file {fname}")
                path = store.result_dir(job.id) / fname
                if (
                    not path.exists()
                    or path.stat().st_size != info.get("bytes")
                    or protocol.file_sha256(path) != info.get("sha256")
                ):
                    raise ValueError(f"{fname} did not arrive intact")
                total += path.stat().st_size
                files[fname] = {"bytes": info["bytes"], "sha256": info["sha256"]}
            if "s11.s1p" not in files:
                raise ValueError("the result has no s11.s1p")
            if total > limits["max_result_bytes"]:
                raise ValueError("the result is over the size limit")
        except (ValueError, protocol.ProtocolError, AttributeError, TypeError) as e:
            self.sched.fail(msg["job"], msg["lease"], f"result rejected: {e}", now, retry=False)
            audit.record(ident, "result_rejected", False, msg["job"], str(e))
            self._dispatch()
            return
        if self.sched.finish(msg["job"], msg["lease"], files, now):
            audit.record(ident, "job_done", True, msg["job"])
        else:
            audit.record(ident, "stale_result", False, msg["job"], "lease no longer current")
        self._dispatch()

    def _confirm_dead(self, job_id, lease):
        turn = self.turns.get(job_id)
        if turn and turn["lease"] == lease:
            turn["confirmed"] = True

    def _release_ended(self):
        """Give a turn back once its job is over AND the solver is known to be dead: the worker said so, or
        the worker's own watchdog (plus a margin) has certainly expired. Not before: a worker cut off from the
        coordinator keeps its solver running until its watchdog fires: the lock must not say 'free' then."""
        now = self.clock()
        for job_id, turn in list(self.turns.items()):
            job = self.sched.jobs.get(job_id)
            over = job is None or job.state != "running" or job.lease != turn["lease"]
            if not over:
                continue
            turn.setdefault("since", now)
            if turn["confirmed"] or now - turn["since"] >= turn["grace"]:
                asyncio.ensure_future(self._release_turn(job_id))

    async def _watch_hold(self, job_id):
        """If the hold command ends while the job runs, the turn is gone: stop the job and requeue it."""
        turn = self.turns.get(job_id)
        if not turn:
            return
        if turn.get("lock") is not None:
            await turn["lock"].lost
        else:
            await turn["hold"].proc.wait()
        if self.turns.get(job_id) is not turn:
            return  # we released it ourselves
        job = self.sched.jobs.get(job_id)
        if job is not None and job.state == "running" and job.lease == turn["lease"]:
            audit.record(
                ("coordinator", "main"),
                "turn_ended_early",
                False,
                job_id,
                "the lock's time ran out" if turn.get("lock") is not None else "the hold command exited",
            )
            sess = self.sessions.get(turn["worker"])
            if sess:
                sess.outbox.put_nowait({"type": "cancel", "job": job_id})
            self.sched.fail(
                job_id, turn["lease"], "the GPU turn ended while the job was running", self.clock(), True
            )
            self._dispatch()

    async def _release_turn(self, job_id):
        held = self.turns.pop(job_id, None)
        if not held:
            return
        if held.get("lock") is not None:
            held["lock"].release()
        else:
            await self._give_back(held["hold"], held["release"], held.get("wait", 20.0))
        audit.record(("coordinator", "main"), "turn_released", True, job_id)

    @staticmethod
    async def _give_back(hold, release, wait):
        """Run the release command, then let the hold command exit by itself so it can clean up (a lock
        script removes its own state in a trap; killing it first leaves a stale lock). Only a hold command
        that has not exited within `wait` seconds is killed."""
        with contextlib.suppress(Exception):
            rel = await procs.start(release, cwd=str(config.home()))
            await asyncio.wait_for(rel.proc.communicate(), 30)
        if hold.proc.returncode is None:
            with contextlib.suppress(asyncio.TimeoutError, Exception):
                await asyncio.wait_for(hold.proc.wait(), wait)
        hold.kill_tree()  # a no-op once it has exited

    @staticmethod
    async def _wait_holding(running):
        async for raw in running.proc.stdout:
            if raw.decode("utf-8", "replace").startswith("HOLDING"):
                return True
        return False

    def _known_locks(self):
        """Lock names listed even when idle: a CPU lock for every machine that has connected, and the locks
        the gates name."""
        names = [f"cpu:{w}" for w in self.sched.workers]
        return names + [str(g["lock"]) for g in self.gates.values() if "lock" in g]

    def _gate_for(self, worker, device):
        """The gate that guards `device` of `worker`, or None. A key "worker:TEXT" guards the worker's devices
        whose name contains TEXT (case does not matter) or whose uuid starts with it, and wins over the plain
        "worker" key, so two GPUs of one machine can take separate turns."""
        dev = self.sched.workers.get(worker, {}).get("devices", {}).get(device, {})
        text = f"{dev.get('name', '')}".lower()
        for key, gate in self.gates.items():
            name, _, part = key.partition(":")
            part = part.lower()
            if name == worker and part and (part in text or str(device).lower().startswith(part)):
                return gate
        return self.gates.get(worker)

    async def _gated_offer(self, job, worker, device, sess, offer, gate):
        lease = job.lease
        fmt = {
            "worker": worker,
            "job": job.id[:12],
            "lease": lease[:8],  # a retry of the same job must not reuse the previous attempt's turn label
            "minutes": int(offer["limits"].get("max_minutes", 120)) + 10,
        }  # (the gate's own max_minutes already lowered the job's limit: see _dispatch)
        running = None
        held = None
        ok = False
        try:
            if "lock" in gate:  # the farm's own manager: first come, first served, one turn per lock name
                held = await asyncio.wait_for(
                    self.locks.acquire(
                        str(gate["lock"]), f"farm-{fmt['job']}-{fmt['lease']}", fmt["minutes"] * 60
                    ),
                    gate.get("max_wait_s", 6 * 3600),
                )
                ok = True
            else:
                running = await procs.start([a.format(**fmt) for a in gate["hold"]], cwd=str(config.home()))
                ok = await asyncio.wait_for(self._wait_holding(running), gate.get("max_wait_s", 6 * 3600))
        except (asyncio.TimeoutError, OSError):
            ok = False
        still = job.state == "running" and job.lease == lease and self.sessions.get(worker) is sess
        if not ok or not still:
            if held is not None:
                held.release()
            if running:
                await self._give_back(
                    running, [a.format(**fmt) for a in gate["release"]], gate.get("release_wait_s", 20.0)
                )
            if still:
                self.sched.unoffer(job.id, lease)
            audit.record(("coordinator", "main"), "turn_not_obtained", False, job.id, worker)
            self._dispatch()
            return
        self.turns[job.id] = {
            "hold": running,
            "lock": held,
            "release": [a.format(**fmt) for a in gate.get("release", [])],
            "lease": lease,
            "worker": worker,
            "confirmed": False,
            "wait": gate.get("release_wait_s", 20.0),
            "grace": sess.watchdog_s + self.turn_margin_s,
        }
        asyncio.ensure_future(self._watch_hold(job.id))
        audit.record(("coordinator", "main"), "turn_held", True, job.id, worker)
        sess.outbox.put_nowait({**offer, "turn": True})

    def _dispatch(self):
        if self.gates_loader:
            with contextlib.suppress(Exception):  # a broken gates.json keeps the last good one
                self.gates = self.gates_loader()
        self._release_ended()
        self._refresh_ceilings()
        self._dispatch_offers()
        for worker, job_id in self.sched.take_discards():
            sess = self.sessions.get(worker)
            if sess:  # an offline worker is told at its next hello
                sess.outbox.put_nowait({"type": "discard", "job": job_id})
        self._persist()

    def _refresh_ceilings(self):
        """Tell the scheduler the longest job each worker will take: the smallest of what it said itself, what
        the policy allows it and what its gate caps (a worker that said nothing is judged by the other two).
        """
        for name, sess in self.sessions.items():
            known = [sess.advertised_minutes, policy.limits_for(("worker", name)).get("max_minutes")]
            cap = self.gates.get(name, {}).get("max_minutes")
            if isinstance(cap, (int, float)) and cap > 0:
                known.append(cap)
            self.sched.set_ceiling(name, min(c for c in known if c))

    def _dispatch_offers(self):
        for job, worker, device in self.sched.next_offers(self.clock()):
            sess = self.sessions.get(worker)
            limits = policy.limits_for(("worker", worker))
            if sess is None:
                self.sched.worker_lost(worker, self.clock(), "worker has no session")
                continue
            audit.record(("coordinator", "main"), "offer", True, job.id, f"{worker}/{device}")
            offer = {
                "type": "offer",
                "job": job.id,
                "lease": job.lease,
                "expires": job.deadline,
                "spec": jobs.worker_spec(job.spec),  # an older worker refuses fields it does not know
                "device": device,
                "input_sha256": job.input_sha256,
                "input_bytes": job.input_bytes,
                "limits": limits,
            }
            if "checkpoint" in sess.features:
                offer["checkpoint"] = {"every": self.checkpoint_every_s, "resume": bool(job.resume)}
            if job.run_limit:  # resume --max-minutes: a shorter ceiling for this run only
                offer["limits"] = {
                    **offer["limits"],
                    "max_minutes": min(limits["max_minutes"], job.run_limit),
                }
                job.run_limit = None
            gate = self._gate_for(worker, device)
            if gate is not None:
                cap = gate.get("max_minutes")
                if isinstance(cap, (int, float)) and cap > 0:
                    offer["limits"] = {
                        **offer["limits"],
                        "max_minutes": min(offer["limits"]["max_minutes"], cap),
                    }
                asyncio.ensure_future(self._gated_offer(job, worker, device, sess, offer, gate))
            else:
                sess.outbox.put_nowait(offer)

    # --- bulk transfers -----------------------------------------------------------------------

    async def _transfer(self, ident, der, first, reader, writer):  # noqa: C901  (complexity ratchet: split when next touched)
        what = first["what"]
        action = {"input_in": "submit", "result_out": "results", "input_out": "run", "result_in": "run"}.get(
            what
        )
        if action is None:
            await self._send(writer, {"type": "blob_ack", "ok": False, "reason": "unknown transfer"})
            return
        ok, why = self._allowed(ident, der, action, first["job"])
        job = self.sched.jobs.get(first["job"]) if ok else None
        owner = f"{ident[0]}:{ident[1]}"
        limits = policy.limits_for(ident)
        try:
            if not ok or job is None:
                raise protocol.ProtocolError(why if not ok else "no such job")
            if what == "input_in":
                if job.owner != owner or job.state != "receiving":
                    raise protocol.ProtocolError("not your job, or its input already arrived")
                name, size, sha = await protocol.recv_blob(
                    reader, store.input_dir(job.id), limits["max_input_bytes"], expect_name="input.json"
                )
                if size != job.input_bytes or sha != job.input_sha256:
                    raise protocol.ProtocolError("the input does not match what was announced")
                self.sched.input_received(job.id)
                await self._send(writer, {"type": "blob_ack", "ok": True, "reason": ""})
                self._dispatch()
            elif what == "input_out":
                if job.state != "running" or job.worker != ident[1]:
                    raise protocol.ProtocolError("that job is not running on you")
                await protocol.send_blob(writer, "input.json", store.input_dir(job.id) / "input.json")
                await asyncio.wait_for(protocol.read_message(reader), READ_TIMEOUT)
            elif what == "result_in":
                if job.state != "running" or job.worker != ident[1]:
                    raise protocol.ProtocolError("that job is not running on you")
                remaining = limits["max_result_bytes"]
                while True:
                    m = await asyncio.wait_for(protocol.read_message(reader), READ_TIMEOUT)
                    if m is None or m["type"] == "blob_end":
                        break
                    if m["type"] != "blob_header" or m["name"] not in store.RESULT_NAMES:
                        raise protocol.ProtocolError("unexpected result file")
                    _, size, _ = await protocol.recv_blob(
                        reader, store.result_dir(job.id), remaining, header=m
                    )
                    remaining -= size
                await self._send(writer, {"type": "blob_ack", "ok": True, "reason": ""})
            else:  # result_out
                if job.owner != owner or job.state != "done":
                    raise protocol.ProtocolError("no finished job of yours with that id")
                for fname in job.files:
                    await protocol.send_blob(writer, fname, store.result_dir(job.id) / fname)
                await self._send(writer, {"type": "blob_end"})
        except protocol.ProtocolError as e:
            audit.record(ident, "transfer_" + what, False, first["job"], str(e))
            with contextlib.suppress(Exception):
                await self._send(writer, {"type": "blob_ack", "ok": False, "reason": str(e)[:200]})
