"""Shared harness and steps for the farm scenarios: a coordinator and workers in one process."""

import asyncio
import json
import os
import subprocess
import threading
import time
from pathlib import Path

import pytest
from pytest_bdd import given, parsers, then, when

from emfdtd_plugin.farm import client, pki, policy
from emfdtd_plugin.farm.coordinator import Coordinator
from emfdtd_plugin.farm.worker import Worker, parse_devices
from farm_stub.paths import STUB


class Proxy:
    """A TCP relay between a worker and the coordinator that can go silent without closing, like a network
    partition: while paused it reads and holds data in both directions, but still notices a close."""

    def __init__(self, host, port):
        self.target = (host, port)
        self.port = None
        self.go = None
        self.server = None

    async def start(self):
        self.go = asyncio.Event()
        self.go.set()
        self.server = await asyncio.start_server(self._accept, "127.0.0.1", 0)
        self.port = self.server.sockets[0].getsockname()[1]

    def pause(self):
        self.go.clear()

    def resume(self):
        self.go.set()

    async def _accept(self, reader, writer):
        up_reader, up_writer = await asyncio.open_connection(*self.target)

        async def relay(src, dst):
            try:
                while True:
                    data = await src.read(65536)
                    if not data:
                        break
                    await self.go.wait()
                    dst.write(data)
                    await dst.drain()
            except (ConnectionError, OSError):
                pass
            finally:
                dst.close()

        asyncio.ensure_future(relay(reader, up_writer))
        asyncio.ensure_future(relay(up_reader, writer))


class Farm:
    def __init__(self, tmp_path, monkeypatch, coord_args=None):
        self.tmp = tmp_path
        monkeypatch.setenv("EMFDTD_FARM_HOME", str(tmp_path / "farm"))
        pki.init_ca()
        self.loop = asyncio.new_event_loop()
        self.thread = threading.Thread(target=self.loop.run_forever, daemon=True)
        self.thread.start()
        self.bundle = pki.issue("coordinator", "main", hosts=["127.0.0.1"])
        self.coord_args = dict(coord_args or {})
        self.coord = Coordinator(self.bundle, "127.0.0.1", 0, check_every=0.2, **self.coord_args)
        self.run(self.coord.start())
        self.workers, self.worker_tasks, self.clients = {}, {}, {}
        self.jobs = []
        self.result = {}

    def restart_coordinator(self):
        """Stop the coordinator and start a new one on the same port with the same farm folder (as `serve` run
        again would): the queue comes back from jobs.json, workers reconnect by themselves."""
        port = self.coord.port
        self.run(self.coord.stop())
        self.coord = Coordinator(self.bundle, "127.0.0.1", port, check_every=0.2, **self.coord_args)
        self.run(self.coord.start())

    def run(self, coro, timeout=30):
        return asyncio.run_coroutine_threadsafe(coro, self.loop).result(timeout)

    def start_proxy(self):
        self.proxy = Proxy("127.0.0.1", self.coord.port)
        self.run(self.proxy.start())
        return self.proxy

    def save_devices(self, name):
        """What `devices --save` records: the stub's devices, in the worker's folder, made once and offered by
        every later connect."""
        out = subprocess.run([STUB, "--list-devices"], capture_output=True, text=True, check=True).stdout
        work = self.tmp / ("work-" + name)
        work.mkdir(parents=True, exist_ok=True)
        devs = [{**d, "vulkan": list(d["vulkan"])} for d in parse_devices(out)]
        (work / "devices.json").write_text(json.dumps(devs))

    def add_worker(self, name, allow, limits=None, via_proxy=False, **worker_args):
        """A worker with the stub solver. Unless a test passes a turn mechanism, the stub GPU is nobody
        else's: an explicit no_lock, which the Worker would otherwise refuse to start without."""
        policy.add_device("worker", name, limits=limits)
        bundle = pki.issue("worker", name)
        if not any(k in worker_args for k in ("lock_command", "turn_from_coordinator")):
            worker_args["no_lock"] = True
        worker_args.setdefault("checkpoint_wait_s", 1.0)  # the stub that hangs ignores a pause request

        self.save_devices(name)  # a worker makes no Vulkan call to connect: it offers the recorded devices

        async def make():
            port = self.proxy.port if via_proxy else self.coord.port
            w = Worker(
                bundle, "127.0.0.1", port, STUB, self.tmp / ("work-" + name), [allow],
                heartbeat=0.3, **worker_args,
            )  # fmt: skip
            self.workers[name] = w
            self.worker_tasks[name] = asyncio.ensure_future(w.run_forever())
            await asyncio.wait_for(w.connected.wait(), 15)
            return w

        return self.run(make())

    def kill_worker(self, name):
        async def go():
            self.workers[name].stop()
            self.worker_tasks[name].cancel()

        self.run(go())

    def add_client(self, name, limits=None):
        policy.add_device("client", name, limits=limits)
        self.clients[name] = pki.issue("client", name)
        return self.clients[name]

    def call(self, fn, who, *args, **kw):
        return self.run(fn(self.clients.get(who) or who, "127.0.0.1", self.coord.port, *args, **kw))

    def snapshot(self, **stub):
        path = self.tmp / f"snap{len(self.jobs)}.json"
        path.write_text(json.dumps({"stub": stub}))
        return path

    def submit(self, who, spec, **stub):
        job = self.call(client.submit, who, spec, self.snapshot(**stub), "test")
        self.jobs.append(job)
        return job

    def job_info(self, who, job):
        st = self.call(client.status, who)
        return next((j for j in st["jobs"] if j["job"] == job), None)

    def wait_state(self, who, job, states, timeout=25):
        end = time.time() + timeout
        info = None
        while time.time() < end:
            info = self.job_info(who, job)
            if info and info["state"] in states:
                return info
            time.sleep(0.1)
        raise AssertionError(f"job did not reach {states}: {info}")

    def close(self):
        async def go():
            if getattr(self, "proxy", None):
                self.proxy.resume()
                self.proxy.server.close()
            for w in self.workers.values():
                w.stop()
            for t in self.worker_tasks.values():
                t.cancel()
            await self.coord.stop()

        try:
            self.run(go(), 15)
        finally:
            self.loop.call_soon_threadsafe(self.loop.stop)
            self.thread.join(5)


@pytest.fixture
def farm(tmp_path, monkeypatch):
    monkeypatch.setattr("emfdtd_plugin.farm.enroll.FAIL_DELAY", 0.0)
    f = Farm(tmp_path, monkeypatch)
    yield f
    f.close()


def spec(**sets):
    return {"profile": "bluetooth", "sets": sets}


# --- set-up ----------------------------------------------------------------------------------------


@given("a farm with a coordinator")
def _farm(farm):
    assert farm.coord.port


@given(parsers.parse('a worker "{name}" offering "{allow}"'))
def _worker(farm, name, allow):
    farm.add_worker(name, allow)


@given(parsers.parse('a client "{name}"'))
def _client(farm, name):
    farm.add_client(name)


@given(parsers.parse('the worker "{name}" has a time limit of {m:f} minutes'))
def _limit(farm, name, m):
    farm.kill_worker(name)
    farm.add_worker(name, "stub gpu a", limits={"max_minutes": m})


@given(parsers.parse('the client "{name}" is limited to {n:d} bytes of input'))
def _tiny(farm, name, n):
    farm.add_client(name, limits={"max_input_bytes": n})


# --- submissions -----------------------------------------------------------------------------------


@when(parsers.parse('"{who}" submits a job with the setting mesh_preset "{value}"'))
def _submit(farm, who, value):
    farm.who = who
    farm.submit(who, spec(mesh_preset=value))


@when(parsers.parse('"{who}" submits a job with the setting backend "{value}"'))
def _submit_bad(farm, who, value):
    try:
        farm.submit(who, spec(backend=value))
    except client.ClientError as e:
        farm.result["error"] = str(e)


@when(parsers.parse('"{who}" submits a job the solver rejects'))
def _reject(farm, who):
    farm.who = who
    farm.submit(who, spec(), reject=True)


@when(parsers.parse('"{who}" submits a job whose solver crashes'))
def _crash(farm, who):
    farm.who = who
    farm.submit(who, spec(), crash=True)


@when(parsers.parse('"{who}" submits a job whose solver hangs'))
def _hang(farm, who):
    farm.who = who
    farm.pid = str(farm.tmp / "solver.pid")
    farm.submit(who, spec(), hang=True, pid_file=farm.pid)


@when(parsers.parse('"{who}" submits a slow job'))
def _slow(farm, who):
    farm.who = who
    farm.submit(who, spec(), lines=60, sleep=0.1)


@when(parsers.parse('"{who}" submits {n:d} slow jobs'))
def _slow_n(farm, who, n):
    farm.who = who
    for _ in range(n):
        farm.submit(who, spec(), lines=60, sleep=0.1)


@when('the job is running on "gpu-box"')
def _running(farm):
    info = farm.wait_state(farm.who, farm.jobs[0], ("running",))
    assert info["worker"] == "gpu-box"
    pid = getattr(farm, "pid", None)
    end = time.time() + 10
    while pid and not Path(pid).exists() and time.time() < end:  # the solver has really started
        time.sleep(0.1)
    time.sleep(0.3)


@when(parsers.parse('the worker "{name}" disappears'))
def _gone(farm, name):
    farm.kill_worker(name)


@when(parsers.parse('a worker "{name}" offering "{allow}" connects'))
def _spare(farm, name, allow):
    farm.add_worker(name, allow)


@when(parsers.parse('"{who}" cancels it'))
def _cancel(farm, who):
    farm.call(client.cancel, who, farm.jobs[0])


@when("the job finishes")
def _finishes(farm):
    farm.final = farm.wait_state(farm.who, farm.jobs[0], ("done", "failed", "cancelled"))
    assert farm.final["state"] == "done", farm.final


@when(parsers.parse('the worker "{name}" certificate is revoked'))
def _revoke(farm, name):
    # the certificate the worker holds: re-issue is not needed, the fingerprint is on the worker object
    w = farm.workers[name]
    pki.revoke(pki.fingerprint(w.bundle["cert"]))
    time.sleep(1.5)  # a heartbeat (every 0.3 s) is checked against the revocation list


# --- results ---------------------------------------------------------------------------------------


@then("the job finishes")
def _then_finishes(farm):
    _finishes(farm)


@then("the results hold s11.s1p, summary.txt and settings.json and the run log")
def _results(farm):
    dest = farm.tmp / "got"
    farm.got = farm.call(client.fetch, farm.who, farm.jobs[0], str(dest))
    assert set(farm.got) == {"s11.s1p", "summary.txt", "settings.json", "run.log"}
    assert "2.4e9" in Path(farm.got["s11.s1p"]).read_text()


@then("the summary names the worker's device and the settings file shows mesh_preset=medium")
def _content(farm):
    assert "device aaaa1111bbbb2222cccc3333dddd4444" in Path(farm.got["summary.txt"]).read_text()
    assert "mesh_preset=medium" in Path(farm.got["settings.json"]).read_text()


@then("the worker's job folder is gone")
def _cleanup(farm):
    time.sleep(0.3)
    jobs_dir = farm.tmp / "work-gpu-box" / "jobs"
    assert not jobs_dir.exists() or not list(jobs_dir.iterdir())


@then("both jobs are running at the same time on different workers")
def _parallel(farm):
    end = time.time() + 20
    while time.time() < end:
        st = farm.call(client.status, farm.who)["jobs"]
        running = [j for j in st if j["state"] == "running"]
        if len(running) == 2:
            assert {j["worker"] for j in running} == {"gpu-box", "second-box"}
            return
        time.sleep(0.1)
    raise AssertionError("two jobs were never running together")


@then("the submission is refused")
def _refused(farm):
    assert "not allowed" in farm.result["error"]


@then(parsers.parse('the job fails with "{text}" after {n:d} attempt'))
def _fails(farm, text, n):
    info = farm.wait_state(farm.who, farm.jobs[0], ("failed", "done"))
    assert info["state"] == "failed" and text in info["reason"] and info["attempts"] == n, info


@then("the solver process is gone")
def _gone_proc(farm):
    _wait_dead(farm.pid)


def _wait_dead(pid_file):
    end = time.time() + 5
    pid = int(Path(pid_file).read_text())
    while time.time() < end:
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return
        time.sleep(0.1)
    raise AssertionError("the solver process is still alive")


@then(parsers.parse('the job finishes on "{worker}" after {n:d} attempts'))
def _requeued(farm, worker, n):
    info = farm.wait_state(farm.who, farm.jobs[0], ("done", "failed"), timeout=40)
    assert info["state"] == "done" and info["worker"] == worker and info["attempts"] == n, info


@then("the job is cancelled and the solver process is gone")
def _cancelled(farm):
    info = farm.wait_state(farm.who, farm.jobs[0], ("cancelled",))
    assert info["state"] == "cancelled"
    _wait_dead(farm.pid)


@then(parsers.parse('"{who}" sees no jobs'))
def _sees_none(farm, who):
    assert farm.call(client.status, who)["jobs"] == []


@then(parsers.parse('"{who}" cannot cancel it or fetch its results'))
def _cannot(farm, who):
    with pytest.raises(client.ClientError):
        farm.call(client.cancel, who, farm.jobs[0])
    with pytest.raises(client.ClientError):
        farm.call(client.fetch, who, farm.jobs[0], str(farm.tmp / "bob-got"))


@then(parsers.parse('the worker "{name}" cannot submit a job'))
def _worker_cannot(farm, name):
    bundle = farm.workers[name].bundle
    with pytest.raises(client.ClientError):
        farm.run(client.submit(bundle, "127.0.0.1", farm.coord.port, spec(), farm.snapshot(), "x"))


@then(parsers.parse('the client "{name}" cannot connect as a worker'))
def _client_cannot(farm, name):
    w = Worker(
        farm.clients[name],
        "127.0.0.1",
        farm.coord.port,
        STUB,
        farm.tmp / "work-evil",
        ["stub gpu"],
        no_lock=True,
    )

    farm.save_devices("evil")  # it has devices recorded like any worker: the coordinator is what refuses it

    async def go():
        from emfdtd_plugin.farm.worker import WorkerRefused

        with pytest.raises(WorkerRefused):
            await w._session("", "")

    farm.run(go())


@then(parsers.parse('the coordinator shows the worker "{name}" offline'))
def _offline(farm, name):
    devs = farm.call(client.status, "alice")["devices"]
    assert devs and all(d["state"] == "offline" for d in devs if d["worker"] == name), devs


@then(parsers.parse('"{who}" submitting a job leaves it queued'))
def _stays_queued(farm, who):
    farm.who = who
    job = farm.submit(who, spec())
    time.sleep(1.0)
    assert farm.job_info(who, job)["state"] == "queued"


@then(parsers.parse('"{who}" cannot submit a normal snapshot'))
def _too_big(farm, who):
    with pytest.raises(client.ClientError):
        farm.submit(who, spec())


@when(parsers.parse('"{who}" submits a job pinned to "{pin}" with an estimate'))
def _pinned_estimate(farm, who, pin):
    farm.who = who
    farm.submit(who, {**spec(), "pin": pin, "estimate": {"cells": 1e5, "steps": 1e3}})


@when(parsers.parse('"{who}" submits a job pinned to "{pin}"'))
def _pinned(farm, who, pin):
    try:
        farm.submit(who, {**spec(), "pin": pin})
    except client.ClientError as e:
        farm.result["error"] = str(e)


@then(parsers.parse('the submission is refused saying "{text}"'))
def _refused_saying(farm, text):
    assert text in farm.result.get("error", ""), farm.result


@given(parsers.parse('the worker "{name}" has its own ceiling of {m:d} minutes'))
def _own_ceiling(farm, name, m):
    farm.add_worker(name, "stub gpu a", caps={"max_minutes": m})


@then(parsers.parse('the coordinator knows the ceiling of "{name}" is {m:d} minutes'))
def _knows(farm, name, m):
    end = time.time() + 5
    while time.time() < end:
        if farm.coord.sched.workers.get(name, {}).get("ceiling") == m:
            return
        time.sleep(0.1)
    raise AssertionError(farm.coord.sched.workers.get(name))
